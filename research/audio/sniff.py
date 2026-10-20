#!/usr/bin/env python3
"""Capture a Switch 2 console talking to the controller with an nRF52840
running Nordic's nRF Sniffer firmware, and decode the ATT traffic.

Uses Nordic's SnifferAPI (from nrf_sniffer_for_bluetooth_le_4.1.1.zip,
extcap/SnifferAPI) copied to research/audio/data/nrf_sniffer/ (gitignored;
Nordic's licence, not ours). Needs pyserial and psutil in the venv, and
access to the sniffer's serial port (sudo setfacl -m u:$USER:rw /dev/ttyACM0).

Scans, then follows the controller as soon as it advertises (as Nordic's
Wireshark plugin does), then the connection the console makes. Prints a
packet count every second, so a dead capture shows. Prints commands, command responses and link-layer control PDUs as
they happen, and once a second a summary of the high-rate reports (input,
rumble, audio). Recognises the Nintendo 0x15/0x04 key exchange, computes the
LTK (A1 ^ B1, as core/src/s2_link.c does) and hands it to the sniffer so a
later encrypted connection can be decrypted; or pass one with --ltk.

Writes research/audio/data/sniff-STAMP.pcap (Wireshark: nordic_ble), .log
(every decoded packet) and ltk.txt.

Usage: .venv/bin/python research/audio/sniff.py [--ltk=HEX] [--address=AA:BB:..] [--port=/dev/ttyACM0]
                                                 [--channel=37] [--legacy]
"""
import sys
import threading
import time
from collections import Counter
from pathlib import Path

HERE = Path(__file__).resolve().parent
DATA = HERE / "data"
sys.path.insert(0, str(DATA / "nrf_sniffer"))

from SnifferAPI import Filelock, Packet, Pcap, Sniffer, UART  # noqa: E402

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
            if hn == "rumble+cmd" and len(val) > 0x21 and val[0x21]:
                # The console sends its commands at 0x21 of the rumble+command
                # report: [00][16 B rumble L][16 B rumble R][8 B header][data].
                self.command(t, d, "cmd(in rumble)", val[0x21:])
            if hn in HIGH_RATE:
                with self.lock:
                    self.rate[(hn, name, len(val))] += 1
                    self.heads.setdefault((hn, len(val)), val[:6].hex(' '))
                return
            if hn == "cmd" and len(val) >= 8:
                self.command(t, d, "cmd", val)
                return
            extra = ""
            if hn in ("cmd-rsp", "cmd-rsp2") and len(val) >= 8:
                off = 0 if hn == "cmd-rsp" else 0x0F
                v = val[off:]
                extra = f"  [rsp {v[0]:02x}/{v[3]:02x}]" if len(v) >= 4 else ""
                if len(v) >= 25 and v[0] == 0x15 and v[3] == 0x04 and self.a1:
                    # LTK = reverse(A1 ^ B1) as sent; checked against the 0x15/0x02
                    # confirmation (B2 = AES(LTK, reverse(A2))).
                    ltk = bytes(x ^ y for x, y in zip(self.a1, v[9:25]))[::-1]
                    self.out(f"   LTK from the 0x15/0x04 exchange: {ltk.hex()}")
                    (DATA / "ltk.txt").write_text(ltk.hex() + "\n")
                    self.send_ltk(ltk)
            self.out(f"{t:.3f} {d} {name} {hn} {val.hex(' ')}{extra}")
        else:
            self.log.write(f"{t:.4f} {d} {name} {pdu.hex()}\n")
            self.out(f"{t:.3f} {d} {name} {pdu[1:].hex(' ')}")

    def command(self, t, d, where, c):
        if c[0] == 0x15 and c[3] == 0x04 and len(c) >= 25:
            self.a1 = bytes(c[9:25])
        self.out(f"{t:.3f} {d} {where} {c[0]:02x}/{c[3]:02x} {c[:8].hex(' ')} | {c[8:].hex(' ')}")

    def send_ltk(self, ltk):
        # Most significant byte first (the AES key order; checked offline
        # against START_ENC_RSP's MIC). As an LE Secure Connections key: the
        # console encrypts with EDIV = Rand = 0, and the firmware ignored the
        # same key given as a legacy LTK.
        if "--legacy" in sys.argv:
            self.sniffer.sendLegacyLTK(list(ltk))
        else:
            self.sniffer.sendSCLTK(list(ltk))

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

    def rekey():
        # Every scan() also sends an all-zero key (SnifferCollector
        # _startScanning), which wipes the LTK: send it again after each.
        if opt("ltk"):
            dec.send_ltk(bytes.fromhex(opt("ltk")))

    stats = Counter()
    want = [int(a, 16) for a in address.split(":")]
    followed = {"done": False}

    def on_packet(n):
        p = n.msg["packet"]
        stats["packets"] += 1
        b = p.blePacket
        if b is not None and b.type != Packet.PACKET_TYPE_ADVERTISING:
            stats["connection"] += 1
        elif b is not None and list(getattr(b, "advAddress", None) or [])[:6] == want:
            stats[f"controller adv type {b.advType} ch{p.channel}"] += 1
        pcap.write(Pcap.create_packet(bytes([p.boardId] + p.getList()), p.time))
        try:
            dec.packet(p)
        except Exception as e:   # keep capturing whatever one packet does
            out(f"   decode error: {e!r}")

    def on_device(n):
        d = n.msg
        if list(d.address[:6]) != want:
            return
        stats["controller adverts"] += 1
        if not followed["done"]:
            followed["done"] = True
            sniffer.follow(d)
            rekey()
            out(f"{time.time():.3f} controller seen (RSSI {d.RSSI}): following it")

    sniffer.subscribe("NEW_BLE_PACKET", on_packet)
    sniffer.subscribe("DEVICE_ADDED", on_device)
    sniffer.subscribe("DEVICE_UPDATED", on_device)
    # Hopping 37/38/39 loses the controller once it's being followed (two
    # misses out of three pairings); --channel=37 keeps one channel.
    channel = opt("channel")
    sniffer.setAdvHopSequence([int(channel)] if channel else [37, 38, 39])
    sniffer.setSupportedProtocolVersion(3)
    sniffer.start()
    # Follow auxiliary pointers: after Sync the controller's pairing-mode
    # advertising seems to move to extended (BT 5) advertising; without this
    # the sniffer saw it once and then nothing.
    sniffer.scan(findScanRsp=True, findAux=True)
    time.sleep(0.5)
    rekey()
    if opt("ltk"):
        out(f"LTK {opt('ltk')} sent to the sniffer")
    out(f"scanning on {port}, channel {channel or '37/38/39'} (firmware {sniffer.swversion}); will follow {address} once it advertises. Ctrl+C to stop")
    quiet = 0
    try:
        while True:
            time.sleep(1)
            dec.summary()
            pcap.flush()
            log.flush()
            print(f"   [{time.strftime('%H:%M:%S')}] {dict(stats)}", flush=True)
            # Following the controller but hearing nothing (its connection
            # request was missed): scan again and wait for the next advert.
            quiet = quiet + 1 if followed["done"] and not stats else 0
            if quiet >= 3:
                out(f"{time.time():.3f} lost the controller (connection missed?): scanning again")
                followed["done"] = False
                quiet = 0
                sniffer.scan(findScanRsp=True, findAux=True)
                time.sleep(0.2)
                rekey()
            stats.clear()
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
