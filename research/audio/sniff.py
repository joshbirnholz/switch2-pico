#!/usr/bin/env python3
"""Capture a Switch 2 console talking to the controller with an nRF52840
running Nordic's nRF Sniffer firmware, and decode the ATT traffic.

Uses Nordic's SnifferAPI (from nrf_sniffer_for_bluetooth_le_4.1.1.zip,
extcap/SnifferAPI) copied to research/audio/data/nrf_sniffer/ (gitignored;
Nordic's licence, not ours). Needs pyserial and psutil in the venv, and
access to the sniffer's serial port (sudo setfacl -m u:$USER:rw /dev/ttyACM0).

Follows the controller's advertising, then the connection the console
makes. Prints commands, command responses and link-layer control PDUs as
they happen, and once a second a summary of the high-rate reports (input,
rumble, audio). Recognises the Nintendo 0x15/0x04 key exchange, computes the
LTK (A1 ^ B1, as core/src/s2_link.c does) and hands it to the sniffer so a
later encrypted connection can be decrypted; or pass one with --ltk.

Writes research/audio/data/sniff-STAMP.pcap (Wireshark: nordic_ble), .log
(every decoded packet) and ltk.txt.

Usage: .venv/bin/python research/audio/sniff.py [--ltk=HEX] [--address=AA:BB:..] [--port=/dev/ttyACM0]
"""
import sys
import threading
import time
from collections import Counter
from pathlib import Path

HERE = Path(__file__).resolve().parent
DATA = HERE / "data"
sys.path.insert(0, str(DATA / "nrf_sniffer"))

from SnifferAPI import Devices, Filelock, Packet, Pcap, Sniffer, UART  # noqa: E402

# The library locks the port with a file in /var/lock, which only root may
# write; nothing else here uses the sniffer, so skip the lock.
Filelock.lock = lambda *a, **k: None
Filelock.unlock = lambda *a, **k: None

HANDLES = {
    0x000A: "input05", 0x000E: "input09", 0x0012: "rumble", 0x0014: "cmd", 0x0016: "rumble+cmd",
    0x0018: "large-cmd", 0x001A: "cmd-rsp", 0x001E: "cmd-rsp2", 0x0022: "notify22", 0x0026: "fde",
    0x002A: "fdf", 0x002C: "AUDIO-OUT", 0x002E: "audio-in", 0x0032: "audio+cmd?",
}
HIGH_RATE = {"input05", "input09", "rumble", "AUDIO-OUT", "audio-in", "rumble+cmd", "audio+cmd?"}
LL_CTRL = {0x00: "CONNECTION_UPDATE_IND", 0x01: "CHANNEL_MAP_IND", 0x02: "TERMINATE_IND", 0x03: "ENC_REQ",
           0x04: "ENC_RSP", 0x05: "START_ENC_REQ", 0x06: "START_ENC_RSP", 0x07: "UNKNOWN_RSP",
           0x08: "FEATURE_REQ", 0x09: "FEATURE_RSP", 0x0C: "VERSION_IND", 0x0E: "PERIPHERAL_FEATURE_REQ",
           0x0F: "CONNECTION_PARAM_REQ", 0x10: "CONNECTION_PARAM_RSP", 0x12: "PING_REQ", 0x13: "PING_RSP",
           0x14: "LENGTH_REQ", 0x15: "LENGTH_RSP", 0x16: "PHY_REQ", 0x17: "PHY_RSP", 0x18: "PHY_UPDATE_IND"}
ATT_OPS = {0x01: "error", 0x02: "mtu-req", 0x03: "mtu-rsp", 0x04: "find-info", 0x05: "find-info-rsp",
           0x08: "read-by-type", 0x09: "read-by-type-rsp", 0x0A: "read", 0x0B: "read-rsp",
           0x10: "read-by-group", 0x11: "read-by-group-rsp", 0x12: "write-req", 0x13: "write-rsp",
           0x1B: "notify", 0x1D: "indicate", 0x1E: "confirm", 0x52: "write-cmd"}


def opt(name, default=None):
    for a in sys.argv[1:]:
        if a.startswith(f"--{name}="):
            return a.split("=", 1)[1]
    return default


class Decoder:
    def __init__(self, out, log, sniffer):
        self.out = out
        self.log = log
        self.sniffer = sniffer
        self.frag = {True: b"", False: b""}
        self.need = {True: 0, False: 0}
        self.rate = Counter()
        self.sizes = {}
        self.heads = {}
        self.a1 = None
        self.lock = threading.Lock()

    def packet(self, p):
        b = p.blePacket
        if b is None:
            return
        t = p.time if hasattr(p, "time") else time.time()
        if b.type == Packet.PACKET_TYPE_ADVERTISING:
            # note CONNECT_IND (advType 5) to see the console's address
            if b.advType == 5:
                self.out(f"{t:.3f} CONNECT_IND {list(b.payload[:12])} (initiator, advertiser)")
            return
        if not p.crcOK:
            return
        data = bytes(b.payload[:b.length])
        if p.encrypted:
            if not p.micOK:
                self.rate["undecrypted"] += 1
                return
            data = data[:-4]
        if not data:
            return
        d = "C>P" if p.direction else "P>C"
        if b.llid == 3:
            self.out(f"{t:.3f} {d} LL {LL_CTRL.get(data[0], hex(data[0]))} {data[1:].hex(' ')}")
            return
        if b.llid == 2:
            self.frag[p.direction] = data
            self.need[p.direction] = int.from_bytes(data[0:2], "little") + 4
        elif b.llid == 1:
            self.frag[p.direction] += data
        buf = self.frag[p.direction]
        if len(buf) < self.need[p.direction] or len(buf) < 4:
            return
        self.frag[p.direction] = b""
        cid = int.from_bytes(buf[2:4], "little")
        pdu = buf[4:self.need[p.direction]]
        if cid != 4:
            self.out(f"{t:.3f} {d} L2CAP cid {cid:#06x} {pdu.hex(' ')}")
            return
        self.att(t, d, pdu)

    def att(self, t, d, pdu):
        op = pdu[0]
        name = ATT_OPS.get(op, hex(op))
        if op in (0x0A, 0x12, 0x52, 0x1B, 0x1D) and len(pdu) >= 3:
            h = int.from_bytes(pdu[1:3], "little")
            hn = HANDLES.get(h, f"{h:#06x}")
            val = pdu[3:]
            self.log.write(f"{t:.4f} {d} {name} {hn} {val.hex()}\n")
            if hn in HIGH_RATE:
                with self.lock:
                    self.rate[(hn, name, len(val))] += 1
                    self.heads.setdefault((hn, len(val)), val[:6].hex(' '))
                return
            extra = ""
            if hn == "cmd" and len(val) >= 8:
                extra = f"  [cmd {val[0]:02x}/{val[3]:02x}]"
                if val[0] == 0x15 and val[3] == 0x04 and len(val) >= 25:
                    self.a1 = bytes(val[9:25])[::-1]
            if hn in ("cmd-rsp", "cmd-rsp2") and len(val) >= 8:
                off = 0 if hn == "cmd-rsp" else 0x0F
                v = val[off:]
                extra = f"  [rsp {v[0]:02x}/{v[3]:02x}]" if len(v) >= 4 else ""
                if len(v) >= 25 and v[0] == 0x15 and v[3] == 0x04 and self.a1:
                    b1 = bytes(v[9:25])[::-1]
                    ltk = bytes(x ^ y for x, y in zip(self.a1, b1))
                    self.out(f"   LTK from the 0x15/0x04 exchange: {ltk.hex()} (also tried reversed)")
                    (DATA / "ltk.txt").write_text(ltk.hex() + "\n")
                    self.send_ltk(ltk)
            self.out(f"{t:.3f} {d} {name} {hn} {val.hex(' ')}{extra}")
        else:
            self.log.write(f"{t:.4f} {d} {name} {pdu.hex()}\n")
            self.out(f"{t:.3f} {d} {name} {pdu[1:].hex(' ')}")

    def send_ltk(self, ltk):
        # Wireshark's plugin passes the key as typed (big-endian hex). Which
        # order the firmware wants for this LTK isn't known yet; ltk.txt keeps it.
        self.sniffer.sendLegacyLTK(list(ltk))

    def summary(self):
        with self.lock:
            if not self.rate:
                return
            parts = []
            for k, n in sorted(self.rate.items(), key=lambda kv: str(kv[0])):
                if k == "undecrypted":
                    parts.append(f"UNDECRYPTED {n}")
                else:
                    hn, name, ln = k
                    parts.append(f"{hn} {name} {ln}B ×{n} [{self.heads.get((hn, ln), '')}]")
            self.rate.clear()
        self.out("   per s: " + "; ".join(parts))


def main():
    port = opt("port") or (UART.find_sniffer() or ["/dev/ttyACM0"])[0]
    address = opt("address", "3C:A9:AB:69:58:AF")
    DATA.mkdir(exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    pcap = open(DATA / f"sniff-{stamp}.pcap", "wb")
    pcap.write(Pcap.get_global_header())
    log = open(DATA / f"sniff-{stamp}.log", "w")
    console = open(DATA / f"sniff-{stamp}.txt", "w")

    def out(line):
        print(line, flush=True)
        console.write(line + "\n")
        console.flush()

    rates = UART.find_sniffer_baudrates(port)
    if not rates:
        sys.exit(f"no sniffer answering on {port}")
    sniffer = Sniffer.Sniffer(port, rates["default"])
    dec = Decoder(out, log, sniffer)

    def on_packet(n):
        p = n.msg["packet"]
        pcap.write(Pcap.create_packet(bytes([p.boardId] + p.getList()), p.time))
        try:
            dec.packet(p)
        except Exception as e:   # keep capturing whatever one packet does
            out(f"   decode error: {e!r}")

    sniffer.subscribe("NEW_BLE_PACKET", on_packet)
    sniffer.setAdvHopSequence([37, 38, 39])
    sniffer.setSupportedProtocolVersion(3)
    sniffer.start()
    sniffer.scan()
    time.sleep(0.5)
    if opt("ltk"):
        dec.send_ltk(bytes.fromhex(opt("ltk")))
        out(f"LTK {opt('ltk')} sent to the sniffer")
    dev = Devices.Device(address=[int(a, 16) for a in address.split(":")] + [0], name='""', RSSI=0)
    sniffer.addDevice(dev)
    sniffer.follow(dev)
    out(f"following {address} on {port} (firmware {sniffer.swversion}); Ctrl+C to stop")
    try:
        while True:
            time.sleep(1)
            dec.summary()
    except KeyboardInterrupt:
        pass
    finally:
        sniffer.doExit()
        pcap.close()
        log.close()
        out(f"saved {pcap.name}")
        console.close()


if __name__ == "__main__":
    main()
