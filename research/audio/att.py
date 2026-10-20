"""A minimal ATT client over a raw L2CAP LE socket (CID 4).

BlueZ's own GATT client gets stuck on these controllers: they never answer
some discovery requests (secondary services, Read By Type for a CCCD), and
BlueZ then waits out its 30 s ATT timeout and disconnects. A raw socket
keeps bluetoothd out of the connection entirely, so nothing but what we
send goes over the air. Never pairs (no SMP).
"""
import queue
import socket
import threading
import time
import uuid as uuidlib

ATT_CID = 4

OP_ERROR = 0x01
OP_MTU_REQ, OP_MTU_RSP = 0x02, 0x03
OP_FIND_INFO_REQ, OP_FIND_INFO_RSP = 0x04, 0x05
OP_READ_BY_TYPE_REQ, OP_READ_BY_TYPE_RSP = 0x08, 0x09
OP_READ_REQ, OP_READ_RSP = 0x0A, 0x0B
OP_READ_BY_GROUP_REQ, OP_READ_BY_GROUP_RSP = 0x10, 0x11
OP_WRITE_REQ, OP_WRITE_RSP = 0x12, 0x13
OP_NOTIFY = 0x1B
OP_INDICATE, OP_CONFIRM = 0x1D, 0x1E
OP_WRITE_CMD = 0x52

ERR_NOT_SUPPORTED = 0x06
ERR_NOT_FOUND = 0x0A

PROPS = ["broadcast", "read", "write-without-response", "write", "notify", "indicate",
         "authenticated-signed-writes", "extended-properties"]


def uuid_str(b: bytes) -> str:
    if len(b) == 2:
        return f"{int.from_bytes(b, 'little'):04x}"
    return str(uuidlib.UUID(bytes=b[::-1]))


class AttError(Exception):
    def __init__(self, req, handle, code):
        super().__init__(f"ATT error {code:#04x} for request {req:#04x} at handle {handle:#06x}")
        self.code = code


class Att:
    def __init__(self, address: str, connect_timeout: float = 30, log=None):
        self.log = log or (lambda s: None)
        self.sock = socket.socket(socket.AF_BLUETOOTH, socket.SOCK_SEQPACKET, socket.BTPROTO_L2CAP)
        self.sock.bind(("00:00:00:00:00:00", 0, ATT_CID, socket.BDADDR_LE_PUBLIC))
        self.sock.settimeout(connect_timeout)
        self.sock.connect((address, 0, ATT_CID, socket.BDADDR_LE_PUBLIC))
        self.sock.settimeout(None)
        self.mtu = 23
        self.closed = threading.Event()
        self.close_reason = None
        self.responses: queue.Queue[bytes] = queue.Queue()
        self.handlers = {}
        self.lock = threading.Lock()
        self.reader = threading.Thread(target=self._read_loop, daemon=True)
        self.reader.start()

    def close(self):
        if not self.closed.is_set():
            self.closed.set()
            try:
                self.sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            self.sock.close()

    # -- receive

    def _read_loop(self):
        while not self.closed.is_set():
            try:
                pdu = self.sock.recv(1024)
            except OSError as e:
                self.close_reason = str(e)
                break
            if not pdu:
                self.close_reason = "connection closed"
                break
            self._dispatch(pdu, time.monotonic())
        self.closed.set()

    def _dispatch(self, pdu: bytes, t: float):
        op = pdu[0]
        if op in (OP_NOTIFY, OP_INDICATE):
            handle = int.from_bytes(pdu[1:3], "little")
            if op == OP_INDICATE:
                self._send(bytes([OP_CONFIRM]))
            cb = self.handlers.get(handle)
            if cb:
                cb(t, pdu[3:])
            else:
                self.log(f"   unhandled {'notify' if op == OP_NOTIFY else 'indicate'} "
                         f"{handle:#06x}: {pdu[3:].hex(' ')}")
        elif op == OP_MTU_REQ:
            # The peer asks about our (nonexistent) GATT server's MTU.
            self._send(bytes([OP_MTU_RSP]) + (517).to_bytes(2, "little"))
            self.log(f"   peer MTU request {int.from_bytes(pdu[1:3], 'little')}")
        elif op in (OP_FIND_INFO_REQ, OP_READ_BY_TYPE_REQ, OP_READ_BY_GROUP_REQ, 0x06):
            # The peer discovers our GATT server: we have none.
            self.log(f"   peer request {pdu.hex(' ')}: answering Attribute Not Found")
            self._send(bytes([OP_ERROR, op]) + pdu[1:3] + bytes([ERR_NOT_FOUND]))
        elif op & 0x40 or op == OP_CONFIRM:
            pass   # commands and confirmations need no answer
        elif op in (OP_READ_REQ, OP_WRITE_REQ, 0x0C, 0x0E, 0x16, 0x18, 0x20):
            self.log(f"   peer request {pdu.hex(' ')}: answering Request Not Supported")
            self._send(bytes([OP_ERROR, op, 0, 0, ERR_NOT_SUPPORTED]))
        else:
            self.responses.put(pdu)

    # -- send

    def _send(self, pdu: bytes):
        self.sock.send(pdu)

    def request(self, pdu: bytes, timeout: float = 2.0) -> bytes:
        """Send a request and return its response. Raises TimeoutError if
        the controller doesn't answer, AttError on an error response."""
        with self.lock:
            while not self.responses.empty():
                self.log(f"   stray response {self.responses.get_nowait().hex(' ')}")
            self._send(pdu)
            try:
                rsp = self.responses.get(timeout=timeout)
            except queue.Empty:
                raise TimeoutError(f"no answer to {pdu.hex(' ')}") from None
            if rsp[0] == OP_ERROR:
                raise AttError(rsp[1], int.from_bytes(rsp[2:4], "little"), rsp[4])
            return rsp

    def exchange_mtu(self, mtu: int = 517) -> int:
        rsp = self.request(bytes([OP_MTU_REQ]) + mtu.to_bytes(2, "little"))
        self.mtu = min(mtu, int.from_bytes(rsp[1:3], "little"))
        return self.mtu

    def read(self, handle: int) -> bytes:
        return self.request(bytes([OP_READ_REQ]) + handle.to_bytes(2, "little"))[1:]

    def write(self, handle: int, value: bytes, timeout: float = 2.0):
        self.request(bytes([OP_WRITE_REQ]) + handle.to_bytes(2, "little") + value, timeout)

    def write_cmd(self, handle: int, value: bytes):
        self._send(bytes([OP_WRITE_CMD]) + handle.to_bytes(2, "little") + value)

    def on_notify(self, value_handle: int, cb):
        self.handlers[value_handle] = cb

    def subscribe(self, value_handle: int, cb, cccd: int | None = None):
        """Enable notifications by writing the CCCD, at value handle + 1
        unless given (where these controllers keep it, see the dongle's
        write_cccd())."""
        self.on_notify(value_handle, cb)
        self.write(cccd or value_handle + 1, b"\x01\x00")

    def unsubscribe(self, value_handle: int, cccd: int | None = None):
        self.write(cccd or value_handle + 1, b"\x00\x00")
        self.handlers.pop(value_handle, None)

    # -- discovery

    def services(self):
        out, start = [], 1
        while start <= 0xFFFF:
            try:
                rsp = self.request(bytes([OP_READ_BY_GROUP_REQ]) + start.to_bytes(2, "little")
                                   + b"\xff\xff" + (0x2800).to_bytes(2, "little"))
            except AttError as e:
                if e.code == ERR_NOT_FOUND:
                    break
                raise
            n = rsp[1]
            for i in range(2, len(rsp), n):
                e = rsp[i:i + n]
                s, end = int.from_bytes(e[0:2], "little"), int.from_bytes(e[2:4], "little")
                out.append((s, end, uuid_str(e[4:])))
                start = end + 1
        return out

    def characteristics(self, start: int, end: int):
        out = []
        while start <= end:
            try:
                rsp = self.request(bytes([OP_READ_BY_TYPE_REQ]) + start.to_bytes(2, "little")
                                   + end.to_bytes(2, "little") + (0x2803).to_bytes(2, "little"))
            except AttError as e:
                if e.code == ERR_NOT_FOUND:
                    break
                raise
            n = rsp[1]
            for i in range(2, len(rsp), n):
                e = rsp[i:i + n]
                decl = int.from_bytes(e[0:2], "little")
                props = e[2]
                value = int.from_bytes(e[3:5], "little")
                out.append((decl, value, props, uuid_str(e[5:])))
                start = decl + 1
        return out

    def find_information(self, start: int, end: int, timeout: float = 2.0):
        out = []
        while start <= end:
            try:
                rsp = self.request(bytes([OP_FIND_INFO_REQ]) + start.to_bytes(2, "little")
                                   + end.to_bytes(2, "little"), timeout)
            except AttError as e:
                if e.code == ERR_NOT_FOUND:
                    break
                raise
            size = 4 if rsp[1] == 1 else 18
            for i in range(2, len(rsp), size):
                h = int.from_bytes(rsp[i:i + 2], "little")
                out.append((h, uuid_str(rsp[i + 2:i + size])))
                start = h + 1
        return out


def props_str(p: int) -> str:
    return ", ".join(n for i, n in enumerate(PROPS) if p >> i & 1)
