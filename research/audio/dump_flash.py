#!/usr/bin/env python3
"""Phase 2: dump the controller's flash with memory reads (0x02/0x04).

Sends nothing but memory reads, plus the dongle's first two connect
commands (0x07/0x01, 0x16/0x01). Writes as it goes and resumes where it
left off: run it again with the same arguments to continue. Reconnects
when the link drops (wake the controller with a button if it has gone to
sleep; it's paired with this laptop, so no Sync needed).

Output, all in research/audio/data/ (gitignored; the dump is Nintendo's
firmware and must never be committed or uploaded):
  flash.bin   the image, 0xFF where nothing has been read
  flash.state one byte per chunk: 0 not read, 1 read, 2 refused
  dump.log    progress, refusals, odd responses

Usage (Wi-Fi off, KDE Connect closed):
  research/audio/wifi-off.sh .venv/bin/python research/audio/dump_flash.py \\
      3C:A9:AB:69:58:AF [--end 0x200000] [--max-minutes 90] [--window 1] [--retry-refused] [--probe]
"""
import argparse
import os
import queue
import time
from pathlib import Path

from att import Att
from s2 import CHAR_CMD, CHAR_CMD_RSP, build_command, memory_read_payload

DATA = Path(__file__).resolve().parent / "data"
CHUNK = 0x40
NOT_READ, READ, REFUSED = 0, 1, 2

# Handles from gatt-pro2.txt (firmware of HEW70006172650). Checked against
# discovery on every connect.
CMD_HANDLE, RSP_HANDLE = 0x0014, 0x001A


class LinkLost(Exception):
    pass


class Dumper:
    def __init__(self, args):
        self.args = args
        DATA.mkdir(exist_ok=True)
        self.log_f = open(DATA / "dump.log", "a")
        self.size = args.end
        self.nchunks = self.size // CHUNK
        self.bin_path = DATA / "flash.bin"
        self.state_path = DATA / "flash.state"
        self._open_files()
        self.att = None
        self.rsp: queue.Queue[bytes] = queue.Queue()

    def log(self, line):
        line = f"{time.strftime('%H:%M:%S')} {line}"
        print(line, flush=True)
        self.log_f.write(line + "\n")
        self.log_f.flush()

    def _open_files(self):
        if not self.bin_path.exists():
            with open(self.bin_path, "wb") as f:
                f.write(b"\xff" * self.size)
        elif self.bin_path.stat().st_size < self.size:
            with open(self.bin_path, "ab") as f:
                f.write(b"\xff" * (self.size - self.bin_path.stat().st_size))
        if not self.state_path.exists():
            self.state_path.write_bytes(bytes(self.nchunks))
        elif self.state_path.stat().st_size < self.nchunks:
            with open(self.state_path, "ab") as f:
                f.write(bytes(self.nchunks - self.state_path.stat().st_size))
        self.bin = open(self.bin_path, "r+b")
        self.state_f = open(self.state_path, "r+b")
        self.state = bytearray(self.state_f.read())

    def save(self):
        self.bin.flush()
        os.fsync(self.bin.fileno())
        self.state_f.seek(0)
        self.state_f.write(self.state)
        self.state_f.flush()
        os.fsync(self.state_f.fileno())

    # -- link

    def connect(self, deadline):
        while time.monotonic() < deadline:
            try:
                self.log("connecting (press a button on the controller if it's asleep)")
                att = Att(self.args.address, connect_timeout=30, log=self.log)
            except OSError as e:
                self.log(f"connect failed: {e}")
                time.sleep(2)
                continue
            try:
                att.exchange_mtu()
                self.check_handles(att)
                self.rsp = queue.Queue()
                att.subscribe(RSP_HANDLE, lambda t, d: self.rsp.put(bytes(d)))
                self.att = att
                for cmd, sub in ((0x07, 0x01), (0x16, 0x01)):
                    att.write_cmd(CMD_HANDLE, build_command(cmd, sub))
                    self.wait_response(cmd, sub)
                self.log(f"connected, mtu {att.mtu}")
                return
            except (OSError, TimeoutError, LinkLost) as e:
                self.log(f"setup failed: {e}")
                att.close()
                self.att = None
                time.sleep(2)
        raise TimeoutError("couldn't reconnect")

    def check_handles(self, att):
        for start, end, suuid in att.services():
            for decl, value, props, cuuid in att.characteristics(start, end):
                if cuuid == CHAR_CMD and value != CMD_HANDLE:
                    raise RuntimeError(f"command characteristic at {value:#06x}, expected {CMD_HANDLE:#06x}")
                if cuuid == CHAR_CMD_RSP and value != RSP_HANDLE:
                    raise RuntimeError(f"response characteristic at {value:#06x}, expected {RSP_HANDLE:#06x}")

    def wait_response(self, cmd, sub, timeout=2.0):
        deadline = time.monotonic() + timeout
        while True:
            if self.att.closed.is_set():
                raise LinkLost(self.att.close_reason)
            try:
                r = self.rsp.get(timeout=min(0.2, max(0.0, deadline - time.monotonic())))
            except queue.Empty:
                if time.monotonic() >= deadline:
                    raise TimeoutError(f"no response to {cmd:02x}/{sub:02x}") from None
                continue
            if r[0] == cmd and r[3] == sub:
                return r
            self.log(f"stray response {r.hex(' ')}")

    # -- reads

    def send_read(self, addr):
        pkt = memory_read_payload(addr, CHUNK)
        assert pkt[0] == 0x02 and pkt[3] == 0x04   # memory read, nothing else
        self.att.write_cmd(CMD_HANDLE, pkt)

    def take_read(self, pending, timeout=2.0):
        """Wait for a response to any outstanding read; returns
        (addr, data or None if refused)."""
        deadline = time.monotonic() + timeout
        while True:
            r = self.wait_response(0x02, 0x04, max(0.05, deadline - time.monotonic()))
            d = r[8:]
            addr = int.from_bytes(d[4:8], "little") if len(d) >= 8 else None
            if addr is None and pending:
                # A short reply without the address: the oldest read's (the
                # controller answers in order).
                addr = next(iter(pending))
            if addr not in pending:
                self.log(f"response for unexpected address: {r.hex(' ')}")
                continue
            if len(d) >= 8 + CHUNK and d[0] == CHUNK:
                return addr, d[8:8 + CHUNK]
            self.log(f"refused {addr:#08x}: {r.hex(' ')}")
            return addr, None

    def run(self):
        a = self.args
        stop_at = time.monotonic() + a.max_minutes * 60
        if a.retry_refused:
            self.state = bytearray(NOT_READ if x == REFUSED else x for x in self.state)
        todo = [i for i in range(a.start // CHUNK, self.nchunks) if self.state[i] == NOT_READ]
        self.log(f"start: {len(todo)} chunks to read of {self.nchunks} "
                 f"({sum(1 for s in self.state if s == READ)} read, "
                 f"{sum(1 for s in self.state if s == REFUSED)} refused)")
        self.connect(stop_at)
        tries = {}
        done_since_save, t_rate, n_rate = 0, time.monotonic(), 0
        i = 0
        pending = {}
        while (i < len(todo) or pending) and time.monotonic() < stop_at:
            try:
                while i < len(todo) and len(pending) < a.window:
                    addr = todo[i] * CHUNK
                    self.send_read(addr)
                    pending[addr] = todo[i]
                    i += 1
                try:
                    addr, data = self.take_read(pending)
                except TimeoutError:
                    # Resend everything outstanding; give up on a chunk after 3 tries.
                    for addr, chunk in list(pending.items()):
                        tries[chunk] = tries.get(chunk, 0) + 1
                        if tries[chunk] >= 3:
                            self.log(f"no answer for {addr:#08x} after 3 tries: marking refused")
                            self.state[chunk] = REFUSED
                        else:
                            self.log(f"timeout at {addr:#08x}, retrying")
                            todo.insert(i, chunk)
                        del pending[addr]
                    continue
                chunk = pending.pop(addr)
                if data is None:
                    self.state[chunk] = REFUSED
                else:
                    self.bin.seek(addr)
                    self.bin.write(data)
                    self.state[chunk] = READ
                done_since_save += 1
                n_rate += 1
                if done_since_save >= 256:
                    self.save()
                    done_since_save = 0
                now = time.monotonic()
                if now - t_rate >= 30:
                    rate = n_rate / (now - t_rate)
                    left = len(todo) - i + len(pending)
                    self.log(f"at {addr:#08x}: {rate * CHUNK / 1024:.1f} KiB/s, "
                             f"{left} chunks left, ~{left / max(rate, 1e-6) / 60:.0f} min")
                    t_rate, n_rate = now, 0
            except (LinkLost, OSError) as e:
                self.log(f"link lost: {e}")
                for addr, chunk in pending.items():
                    todo.insert(i, chunk)
                pending.clear()
                self.save()
                if self.att:
                    self.att.close()
                self.connect(stop_at)
        self.save()
        left = sum(1 for s in self.state if s == NOT_READ)
        refused = [k for k, s in enumerate(self.state) if s == REFUSED]
        self.log(f"stopped: {left} chunks not read, {len(refused)} refused")
        for start, end in ranges(refused):
            self.log(f"  refused {start * CHUNK:#08x}-{end * CHUNK + CHUNK - 1:#08x}")
        if left == 0 and a.probe:
            self.probe()
        if self.att:
            self.att.close()

    def probe(self):
        """Reads past the end, to see whether the flash is bigger (or wraps)."""
        self.log("probing past the end")
        first = self.bin_read(0)
        for addr in (self.size, self.size * 2, self.size * 4, self.size * 8 - CHUNK):
            pending = {addr: None}
            try:
                self.send_read(addr)
                _, data = self.take_read(pending)
            except TimeoutError:
                self.log(f"  {addr:#08x}: no answer")
                continue
            if data is None:
                self.log(f"  {addr:#08x}: refused")
            else:
                same = " (same as 0x0: wraps)" if data == first else ""
                self.log(f"  {addr:#08x}: {data[:16].hex(' ')}…{same}")

    def bin_read(self, addr):
        self.bin.seek(addr)
        return self.bin.read(CHUNK)


def ranges(idx):
    out = []
    for k in idx:
        if out and k == out[-1][1] + 1:
            out[-1][1] = k
        else:
            out.append([k, k])
    return out


def main():
    p = argparse.ArgumentParser()
    p.add_argument("address")
    p.add_argument("--start", type=lambda s: int(s, 0), default=0)
    p.add_argument("--end", type=lambda s: int(s, 0), default=0x200000)
    p.add_argument("--max-minutes", type=float, default=90)
    p.add_argument("--window", type=int, default=1, help="reads in flight (the dongle uses 1)")
    p.add_argument("--retry-refused", action="store_true",
                   help="read chunks marked refused again (with --window > 1 a lost reply can mark the wrong one)")
    p.add_argument("--probe", action="store_true",
                   help="after a full dump, read a few chunks past --end to find the flash size")
    args = p.parse_args()
    assert args.start % CHUNK == 0 and args.end % CHUNK == 0
    d = Dumper(args)
    try:
        d.run()
    finally:
        d.save()


if __name__ == "__main__":
    main()
