"""Switch 2 controller protocol helpers shared by the research scripts.

Mirrors core/src/s2_proto.[ch]. See research/headset-audio.md.
"""
import queue
import time

SERVICE = "ab7de9be-89fe-49ad-828f-118f09df7fd0"
CHAR_INPUT = "ab7de9be-89fe-49ad-828f-118f09df7fd2"
CHAR_CMD = "649d4ac9-8eb7-4e6c-af44-1ea54fe5f005"
CHAR_CMD_RSP = "c765a961-d9d8-4d36-a20a-5315b111836a"
CHAR_RUMBLE_PRO = "cc483f51-9258-427d-a939-630c31f72b05"
CHAR_AUDIO_OUT = "cc483f51-9258-427d-a939-630c31f72b06"
CHAR_PRO_INPUT9 = "7492866c-ec3e-4619-8258-32755ffcc0f8"
CHAR_AUDIO_IN = "7492866c-ec3e-4619-8258-32755ffcc0f9"

BUTTONS = ["Y", "X", "B", "A", "SR", "SL", "R", "ZR", "-", "+", "RS", "LS", "HOME", "CAPT", "C", "",
           "DOWN", "UP", "RIGHT", "LEFT", "SR", "SL", "L", "ZL", "GR", "GL", "", "", "HEADSET"]


def build_command(cmd: int, sub: int, data: bytes = b"") -> bytes:
    return bytes([cmd, 0x91, 0x01, sub, 0x00, len(data), 0x00, 0x00]) + data


def memory_read_payload(addr: int, length: int) -> bytes:
    return build_command(0x02, 0x04, bytes([length, 0x7E, 0, 0]) + addr.to_bytes(4, "little"))


def unpack12(b: bytes) -> tuple[int, int]:
    return b[0] | ((b[1] & 0x0F) << 8), (b[1] >> 4) | (b[2] << 4)


def fmt_input(d: bytes) -> str:
    if len(d) < 0x3C:
        return f"short report ({len(d)}): {bytes(d).hex()}"
    counter = int.from_bytes(d[0:4], "little")
    buttons = int.from_bytes(d[4:8], "little")
    names = [n for i, n in enumerate(BUTTONS) if n and buttons >> i & 1]
    ls, rs = unpack12(d[0x0A:0x0D]), unpack12(d[0x0D:0x10])
    mv = int.from_bytes(d[0x1F:0x21], "little")
    return (f"#{counter:<8} L={ls[0]:4},{ls[1]:4} R={rs[0]:4},{rs[1]:4} "
            f"bat={mv}mV btn={buttons:08x} {' '.join(names)}")


class Commander:
    """Sends commands over an att.Att link and waits for the matching
    response on the command-response characteristic. Like the dongle,
    uses Write Requests only when the characteristic allows them ("write"
    property), and Write Commands otherwise."""

    def __init__(self, att, cmd_handle: int, cmd_props: int, rsp_handle: int, out,
                 timeout: float = 2.0):
        self.att = att
        self.cmd_handle = cmd_handle
        self.use_req = bool(cmd_props & 0x08)
        self.rsp_handle = rsp_handle
        self.out = out
        self.timeout = timeout
        self.queue: queue.Queue[bytes] = queue.Queue()

    def start(self):
        self.att.subscribe(self.rsp_handle, lambda t, d: self.queue.put(bytes(d)))

    def send(self, cmd: int, sub: int, data: bytes = b"") -> bytes | None:
        return self.send_raw(build_command(cmd, sub, data))

    def send_raw(self, pkt: bytes) -> bytes | None:
        while not self.queue.empty():
            self.out(f"   stray response: {self.queue.get_nowait().hex(' ')}")
        self.out(f">> {pkt.hex(' ')}")
        if self.use_req:
            self.att.write(self.cmd_handle, pkt)
        else:
            self.att.write_cmd(self.cmd_handle, pkt)
        deadline = time.monotonic() + self.timeout
        while True:
            try:
                rsp = self.queue.get(timeout=max(0.0, deadline - time.monotonic()))
            except queue.Empty:
                self.out("<< (no response)")
                return None
            self.out(f"<< {rsp.hex(' ')}")
            if rsp[0] == pkt[0] and rsp[3] == pkt[3]:
                return rsp
