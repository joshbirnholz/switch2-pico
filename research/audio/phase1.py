#!/usr/bin/env python3
"""Phase 1: connect to a Pro Controller 2 without pairing, dump the GATT
table, run the dongle's connect sequence and print input reports live.

Only sends commands the dongle sends (core/src/s2_link.c init_run()),
minus the 0x15 pairing steps. Talks ATT directly (att.py), so BlueZ's GATT
client stays out of it.

Usage: .venv/bin/python research/audio/phase1.py ADDRESS [seconds] [--pair]

--pair runs the 0x15 app-layer pairing steps like the dongle does for a
controller in pairing mode. Without them, the controller sends input
reports with live motion data but buttons and sticks held at rest. The
controller then remembers this laptop's address as a host.
The controller must be advertising (hold the sync button). Logs go to
research/audio/data/ (gitignored); the GATT table goes to
research/audio/gatt-pro2.txt.
"""
import os
import sys
import time
from pathlib import Path

from att import Att, AttError, props_str
from s2 import (CHAR_CMD, CHAR_CMD_RSP, CHAR_INPUT, Commander, fmt_input,
                memory_read_payload)

HERE = Path(__file__).resolve().parent
DATA = HERE / "data"


def main(address: str, seconds: float, pair: bool) -> None:
    DATA.mkdir(exist_ok=True)
    stamp = time.strftime('%Y%m%d-%H%M%S')
    log = open(DATA / f"phase1-{stamp}.log", "w")
    raw = open(DATA / f"phase1-{stamp}.raw", "w")

    def out(line=""):
        print(line, flush=True)
        log.write(line + "\n")
        log.flush()

    out(f"connecting to {address}")
    t = time.monotonic()
    att = Att(address, log=out)
    t0 = time.monotonic()
    out(f"connected in {t0 - t:.1f} s")
    try:
        run(att, out, t0, seconds, pair, raw)
    finally:
        if att.closed.is_set():
            out(f"link closed: {att.close_reason}")
        att.close()
        log.close()
        raw.close()


def local_address(att) -> bytes:
    """This adapter's address, little-endian, as the pairing step sends it."""
    addr = att.sock.getsockname()[0]
    if addr == "00:00:00:00:00:00":
        raise RuntimeError("couldn't get the adapter's address")
    return bytes.fromhex(addr.replace(":", ""))[::-1]


def pair_steps(cmd, att, out):
    """The dongle's ST_PAIR_ADDR..ST_PAIR_FINALIZE (s2_link.c init_run())."""
    addr = local_address(att)
    out(f"   pairing with host address {addr[::-1].hex(':')}")
    if not cmd.send(0x15, 0x01, bytes([0x00, 0x02]) + addr + addr):
        return False
    a1 = os.urandom(16)
    rsp = cmd.send(0x15, 0x04, bytes([0x00]) + a1[::-1])
    if not rsp:
        return False
    if len(rsp) >= 8 + 17:
        b1 = rsp[9:25][::-1]
        out(f"   ltk {bytes(x ^ y for x, y in zip(a1, b1)).hex()}")
    if not cmd.send(0x15, 0x02, bytes([0x00]) + os.urandom(16)):
        return False
    return cmd.send(0x15, 0x03, bytes([0x00])) is not None


def run(att, out, t0, seconds, pair, raw):
    out(f"mtu {att.exchange_mtu()}")

    table = []
    chars = {}
    for start, end, suuid in att.services():
        table.append(f"service {suuid} handles {start:#06x}-{end:#06x}")
        for decl, value, props, cuuid in att.characteristics(start, end):
            table.append(f"  char {cuuid} decl {decl:#06x} value {value:#06x} [{props_str(props)}]")
            chars[cuuid] = (value, props)
    for line in table:
        out(line)

    cmd = Commander(att, *chars[CHAR_CMD], chars[CHAR_CMD_RSP][0], out)
    cmd.start()

    out("\n-- connect sequence")
    cmd.send(0x07, 0x01)
    info = cmd.send_raw(memory_read_payload(0x13000, 0x40))
    if info:
        p = info[16:]
        out(f"   serial={p[2:16].rstrip(b'\0').decode(errors='replace')} "
            f"pid={int.from_bytes(p[0x14:0x16], 'little'):#06x}")
    cmd.send(0x16, 0x01)
    if pair:
        out(f"   pairing {'complete' if pair_steps(cmd, att, out) else 'FAILED'}")
    cmd.send(0x0A, 0x02, bytes([0x03, 0, 0, 0]))            # click
    cmd.send(0x09, 0x07, bytes([0x01, 0, 0, 0, 0, 0, 0, 0]))  # player 1 LED
    cmd.send(0x0C, 0x02, bytes([0x2F, 0, 0, 0]))
    for addr in (0x13080, 0x130C0, 0x1FC040, 0x1FC080):
        cmd.send_raw(memory_read_payload(addr, 0x40))
    cmd.send(0x0A, 0x08, bytes([0x01, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x35,
                                0x00, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00]))
    cmd.send(0x0C, 0x04, bytes([0x2F, 0, 0, 0]))

    out(f"\n-- input reports for {seconds:.0f} s (press buttons, move sticks)")
    state = {"n": 0, "last": 0.0, "buttons": None, "rate_t": time.monotonic(), "rate_n": 0}

    def on_input(t, data):
        raw.write(f"{t - t0:.4f} {bytes(data).hex()}\n")
        state["n"] += 1
        state["rate_n"] += 1
        buttons = int.from_bytes(data[4:8], "little")
        if buttons != state["buttons"] or t - state["last"] > 1.0:
            rate = state["rate_n"] / max(t - state["rate_t"], 1e-3)
            out(f"{t - t0:8.3f} {fmt_input(data)} ~{rate:.0f}/s")
            state["last"], state["buttons"] = t, buttons
            if t - state["rate_t"] > 2:
                state["rate_t"], state["rate_n"] = t, 0

    att.subscribe(chars[CHAR_INPUT][0], on_input)
    att.closed.wait(seconds)
    out(f"\n{state['n']} input reports")
    if att.closed.is_set():
        return
    att.unsubscribe(chars[CHAR_INPUT][0])

    # Last, since the controller may ignore it (BTstack's CCCD lookup never
    # got an answer): descriptors, and the values of readable characteristics.
    out("\n-- descriptors and readable values")
    try:
        for h, duuid in att.find_information(1, 0xFFFF):
            table.append(f"  attr {h:#06x} {duuid}")
    except (TimeoutError, AttError) as e:
        table.append(f"  (find information: {e})")
    for cuuid, (value, props) in chars.items():
        if props & 0x02:
            try:
                table.append(f"  read {cuuid} {value:#06x}: {att.read(value).hex(' ')}")
            except (TimeoutError, AttError) as e:
                table.append(f"  read {cuuid} {value:#06x}: {e}")
    for line in table:
        out(line)
    (HERE / "gatt-pro2.txt").write_text("\n".join(table) + "\n")


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    main(args[0], float(args[1]) if len(args) > 1 else 30, "--pair" in sys.argv)
