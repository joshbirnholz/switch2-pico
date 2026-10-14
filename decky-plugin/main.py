# Switch2-Pico Decky plugin: backend.
#
# Talks to the dongle over its USB configuration interface (the one the
# configuration page uses through WebUSB): a vendor-class interface named
# "Switch2-Pico Config" with one bulk endpoint each way. Requests and
# responses are framed as in core/src/webusb.c:
#   request:  "S2PQ" <u32 LE length> "<METHOD> <path>\n" <body>
#   response: "S2PR" <u16 LE status> <u16 0> <u32 LE length> <body>
# and carry the same HTTP-like API as the page (core/src/web_api.c).
#
# The interface is reached through Linux usbdevfs directly (no libusb), so
# the plugin needs no extra Python packages; it runs as root (plugin.json
# flag "_root") for access to /dev/bus/usb.

import asyncio
import ctypes
import hashlib
import io
import errno
import fcntl
import glob
import json
import os
import select
import shutil
import ssl
import struct
import subprocess
import tempfile
import threading
import time
import urllib.request
import zipfile
import zlib

import decky

CONFIG_INTERFACE_NAME = "Switch2-Pico Config"
# CI publishes every build of the default branch here (see .github/workflows/build.yml).
FW_BASE = "https://joshbirnholz.github.io/switch2-pico/fw/"
# ... and this plugin, built from decky-plugin/ (latest.json: version, file, sha256).
PLUGIN_BASE = "https://joshbirnholz.github.io/switch2-pico/decky/"
PLUGIN_FILES = ("main.py", "plugin.json", "package.json", "LICENSE", "README.md", "dist/index.js")

# <linux/usbdevice_fs.h>
USBDEVFS_BULK = 0xC0185502             # _IOWR('U', 2, struct usbdevfs_bulktransfer), 64-bit
USBDEVFS_CLAIMINTERFACE = 0x8004550F   # _IOR('U', 15, unsigned int)
USBDEVFS_RELEASEINTERFACE = 0x80045510 # _IOR('U', 16, unsigned int)

POLL_S = 3.0                # background status poll (notifications)
EVENTS_S = 0.05             # button events poll (Quick Access menu)
UPDATE_CHECK_S = 24 * 3600  # automatic update check interval
FW_CHUNK = 4096


class DongleError(Exception):
    pass


def _read(path):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return None


def find_dongle():
    """The dongle's configuration interface: {bus, dev, itf, ep_in, ep_out} or None."""
    for itf_dir in glob.glob("/sys/bus/usb/devices/*:*"):
        if _read(os.path.join(itf_dir, "interface")) != CONFIG_INTERFACE_NAME:
            continue
        dev_dir = os.path.dirname(os.path.realpath(itf_dir))
        bus, dev = _read(os.path.join(dev_dir, "busnum")), _read(os.path.join(dev_dir, "devnum"))
        itf = _read(os.path.join(itf_dir, "bInterfaceNumber"))
        ep_in = ep_out = None
        for ep_dir in glob.glob(os.path.join(itf_dir, "ep_*")):
            if _read(os.path.join(ep_dir, "type")) != "Bulk":
                continue
            addr = int(_read(os.path.join(ep_dir, "bEndpointAddress")) or "0", 16)
            if addr & 0x80:
                ep_in = addr
            else:
                ep_out = addr
        if bus and dev and itf is not None and ep_in and ep_out:
            return {"bus": int(bus), "dev": int(dev), "itf": int(itf, 16), "ep_in": ep_in, "ep_out": ep_out}
    return None


class Dongle:
    """One request at a time over the configuration interface. The interface
    is claimed only for the length of an exchange, so the configuration page
    (Chrome) can use it in between."""

    def __init__(self):
        self.info = None

    def _bulk(self, fd, ep, data, length, timeout_ms):
        buf = ctypes.create_string_buffer(length) if data is None else ctypes.create_string_buffer(data, length)
        arg = bytearray(struct.pack("IIIxxxxQ", ep, length, timeout_ms, ctypes.addressof(buf)))
        n = fcntl.ioctl(fd, USBDEVFS_BULK, arg)
        return buf.raw[:n]

    def exchange(self, method, path, body=b"", timeout_ms=3000):
        """(status, body bytes) for one request. Raises DongleError."""
        info = self.info or find_dongle()
        if not info:
            self.info = None
            raise DongleError("not found")
        self.info = info
        node = "/dev/bus/usb/%03d/%03d" % (info["bus"], info["dev"])
        try:
            fd = os.open(node, os.O_RDWR)
        except OSError:
            self.info = None   # re-enumerated (restart, mode change): look again
            raise DongleError("not found")
        try:
            try:
                fcntl.ioctl(fd, USBDEVFS_CLAIMINTERFACE, struct.pack("I", info["itf"]))
            except OSError as e:
                if e.errno == errno.EBUSY:
                    raise DongleError("in use by another app (close the configuration page)")
                raise
            try:
                payload = ("%s %s\n" % (method, path)).encode() + body
                msg = b"S2PQ" + struct.pack("<I", len(payload)) + payload
                self._bulk(fd, info["ep_out"], msg, len(msg), timeout_ms)
                got = b""
                need = 12
                status = 0
                while len(got) < need:
                    got += self._bulk(fd, info["ep_in"], None, 4096, timeout_ms)
                    if need == 12 and len(got) >= 12:
                        if got[:4] != b"S2PR":
                            raise DongleError("bad response")
                        status = struct.unpack_from("<H", got, 4)[0]
                        need = 12 + struct.unpack_from("<I", got, 8)[0]
                return status, got[12:need]
            finally:
                try:
                    fcntl.ioctl(fd, USBDEVFS_RELEASEINTERFACE, struct.pack("I", info["itf"]))
                except OSError:
                    pass
        except OSError as e:
            self.info = None
            raise DongleError(os.strerror(e.errno) if e.errno else str(e))
        finally:
            os.close(fd)


def uf2_image(data, family, base, max_len):
    """The flash image for one UF2 family (gaps 0xFF), as the page's uf2Image()."""
    blocks = []
    end = 0
    for o in range(0, len(data) - 511, 512):
        m0, m1, flags, addr, length = struct.unpack_from("<IIIII", data, o)
        if m0 != 0x0A324655 or m1 != 0x9E5D5157 or flags & 1:
            continue
        if flags & 0x2000 and struct.unpack_from("<I", data, o + 28)[0] != family:
            continue
        if length > 476 or addr < base or addr + length - base > max_len:
            raise DongleError("the firmware doesn't fit this board")
        blocks.append((addr - base, o + 32, length))
        end = max(end, addr + length - base)
    if not blocks:
        raise DongleError("no firmware for this board in the file")
    img = bytearray(b"\xff" * end)
    for off, src, length in blocks:
        img[off:off + length] = data[src:src + length]
    return bytes(img)


# SInput mode: Steam reads the controller through hidraw. SteamOS's own rules
# may not cover its ID (2e8a:10c6), so the plugin adds one.
UDEV_RULE_PATH = "/etc/udev/rules.d/70-switch2-pico.rules"
UDEV_RULE = (
    "# Added by the Switch2-Pico Decky plugin: lets Steam read the dongle in SInput mode.\n"
    'KERNEL=="hidraw*", ATTRS{idVendor}=="2e8a", ATTRS{idProduct}=="10c6", MODE="0660", TAG+="uaccess"\n'
)
UDEV_DIRS = ("/etc/udev/rules.d", "/usr/lib/udev/rules.d", "/lib/udev/rules.d", "/run/udev/rules.d")


def system_env():
    """The environment for system tools: without the libraries Decky's
    bundled Python points LD_LIBRARY_PATH at."""
    env = dict(os.environ)
    env.pop("LD_LIBRARY_PATH", None)
    env.pop("LD_PRELOAD", None)
    return env


def install_udev_rule():
    """Best effort: add the hidraw rule unless a rule for the ID exists."""
    try:
        if _read(UDEV_RULE_PATH) == UDEV_RULE.strip():
            return
        for d in UDEV_DIRS:
            for path in glob.glob(os.path.join(d, "*.rules")):
                if path == UDEV_RULE_PATH:
                    continue
                text = (_read(path) or "").lower()
                if "2e8a" in text and "10c6" in text:
                    return
        os.makedirs(os.path.dirname(UDEV_RULE_PATH), exist_ok=True)
        with open(UDEV_RULE_PATH, "w") as f:
            f.write(UDEV_RULE)
        subprocess.run(["udevadm", "control", "--reload-rules"], timeout=10, check=False, env=system_env())
        subprocess.run(["udevadm", "trigger", "--subsystem-match=hidraw", "--action=change"], timeout=10, check=False,
                       env=system_env())
        decky.logger.info("installed %s", UDEV_RULE_PATH)
    except Exception as e:
        decky.logger.warning("udev rule not installed: %s", e)


def vcmp(a, b):
    x = [int(p) for p in str(a).split(".")[:3]] + [0, 0, 0]
    y = [int(p) for p in str(b).split(".")[:3]] + [0, 0, 0]
    return (x[:3] > y[:3]) - (x[:3] < y[:3])


# Decky's bundled Python doesn't find the system's root certificates by
# itself ("certificate verify failed"): point it at the system bundle.
CA_FILES = (
    "/etc/ssl/certs/ca-certificates.crt",   # SteamOS / Arch, Debian
    "/etc/ca-certificates/extracted/tls-ca-bundle.pem",
    "/etc/pki/tls/certs/ca-bundle.crt",     # Fedora
    "/etc/ssl/cert.pem",
)


def _ssl_context():
    for path in CA_FILES:
        if os.path.exists(path):
            return ssl.create_default_context(cafile=path)
    return ssl.create_default_context()


SSL_CONTEXT = _ssl_context()


def http_get(url, timeout=20):
    req = urllib.request.Request(url, headers={"Cache-Control": "no-cache", "User-Agent": "switch2-pico-decky"})
    with urllib.request.urlopen(req, timeout=timeout, context=SSL_CONTEXT) as r:
        return r.read()

# ---------------------------------------------------------------------------
# Motion (DSU / cemuhook) server: the connected controller's gyro and
# accelerometer for emulators (Cemu, Dolphin, Ryujinx, ...), which under
# Steam Input only see Steam's virtual controller. The motion comes from the
# emulated controller's own input reports, read from its hidraw node next to
# Steam (hidraw lets several readers have every report).
# ---------------------------------------------------------------------------
DSU_DEFAULT_PORT = 26761   # not 26760: SteamDeckGyroDSU's (the Deck's own gyro)
DSU_CLIENT_TIMEOUT_S = 5.0
DSU_PROTOCOL = 1001
DSU_MAC = bytes.fromhex("025332500001")

# Emulated controllers with motion in their input reports: name by USB ID.
MOTION_MODES = {
    (0x057E, 0x2009): "switch_pro",
    (0x054C, 0x0CE6): "dualsense",
    (0x054C, 0x0DF2): "dualsense",
    (0x2E8A, 0x10C6): "sinput",
}


def find_motion_hidraw():
    """(/dev/hidrawN, mode, (vid, pid)) of the dongle's controller interface,
    or (None, None, (vid, pid) or None) when it has no motion (or isn't there)."""
    ids = None
    for node in sorted(glob.glob("/sys/class/hidraw/hidraw*")):
        itf = os.path.dirname(os.path.realpath(os.path.join(node, "device")))   # .../X-Y:1.N
        dev = os.path.dirname(itf)
        if _read(os.path.join(itf, "bInterfaceNumber")) != "00":
            continue
        # The dongle: its configuration interface is on the same device.
        if not any(_read(os.path.join(p, "interface")) == CONFIG_INTERFACE_NAME
                   for p in glob.glob(os.path.join(dev, os.path.basename(dev) + ":*"))):
            continue
        try:
            ids = (int(_read(os.path.join(dev, "idVendor")), 16), int(_read(os.path.join(dev, "idProduct")), 16))
        except (TypeError, ValueError):
            continue
        mode = MOTION_MODES.get(ids)
        if mode:
            return "/dev/" + os.path.basename(node), mode, ids
    return None, None, ids


def _s16(b, o):
    return struct.unpack_from("<h", b, o)[0]


def motion_from_report(mode, r):
    """(accel g, gyro deg/s) in SDL's frame (x right, y up, z towards the
    player) from one input report (report ID first), or None. The layouts and
    scales are the firmware's (core/src/sinput.c, ds5.c, procon.c)."""
    if mode == "sinput" and len(r) >= 35 and r[0] == 0x01:
        a, g = 8.0 / 32768.0, 2000.0 / 32768.0
        # Report: X = -x, Y = -z, Z = y.
        acc = (-_s16(r, 23) * a, _s16(r, 27) * a, -_s16(r, 25) * a)
        gyr = (-_s16(r, 29) * g, _s16(r, 33) * g, -_s16(r, 31) * g)
        return acc, gyr
    if mode == "dualsense" and len(r) >= 28 and r[0] == 0x01:
        gyr = tuple(_s16(r, 16 + 2 * i) / 16.0 for i in range(3))
        acc = tuple(_s16(r, 22 + 2 * i) / 8192.0 for i in range(3))
        return acc, gyr
    if mode == "switch_pro" and len(r) >= 49 and r[0] == 0x30:
        # Three samples a report, the Switch frame (s1 = (y, -x, z) of the
        # Switch 2's); the latest.
        o = 13 + 2 * 12
        a, g = 1.0 / 4096.0, 936.0 / 13371.0
        s1a = [_s16(r, o + 2 * i) for i in range(3)]
        s1g = [_s16(r, o + 6 + 2 * i) for i in range(3)]
        return ((-s1a[1] * a, s1a[2] * a, -s1a[0] * a), (-s1g[1] * g, s1g[2] * g, -s1g[0] * g))
    return None


def dsu_motion(acc, gyr):
    """SDL frame to DSU's: accel (g) and gyro pitch / yaw / roll (deg/s); a
    controller lying flat reads -1 g on Y (as SteamDeckGyroDSU sends)."""
    return (-acc[0], -acc[1], -acc[2]), (gyr[0], -gyr[1], -gyr[2])


def dsu_packet(server_id, msg_type, payload):
    body = struct.pack("<I", msg_type) + payload
    head = b"DSUS" + struct.pack("<HHII", DSU_PROTOCOL, len(body), 0, server_id)
    pkt = bytearray(head + body)
    struct.pack_into("<I", pkt, 8, zlib.crc32(pkt) & 0xFFFFFFFF)
    return bytes(pkt)


def dsu_slot_info(slot, connected):
    # Slot, state (2 connected), model (2 full gyro), connection (1 USB), MAC, battery (5 full).
    if slot != 0 or not connected:
        return struct.pack("<BBBB6sB", slot, 0, 0, 0, bytes(6), 0)
    return struct.pack("<BBBB6sB", 0, 2, 2, 1, DSU_MAC, 5)


def dsu_pad_data(counter, timestamp_us, acc, gyr):
    """Motion only: buttons and sticks neutral (the game has them through
    Steam Input already)."""
    return (dsu_slot_info(0, True) + struct.pack("<BI", 1, counter & 0xFFFFFFFF) +
            bytes(4) + bytes([128, 128, 128, 128]) + bytes(12) + bytes(12) +
            struct.pack("<Q6f", timestamp_us, *acc, *gyr))


class DsuServer:
    """A cemuhook / DSU server on UDP `port` with the dongle's controller in
    slot 0, fed from its hidraw node by a reader thread."""

    def __init__(self, loop, port):
        self.loop = loop
        self.port = port
        self.server_id = int.from_bytes(os.urandom(4), "little")
        self.clients = {}      # addr -> last request time
        self.counter = 0
        self.transport = None
        self.stop = threading.Event()
        self.thread = None
        self.mode = None       # what the reader is reading ("sinput", ...)
        self.ids = None
        self.error = ""

    async def start(self):
        server = self

        class Proto(asyncio.DatagramProtocol):
            def datagram_received(self, data, addr):
                server._request(data, addr)

        self.transport, _ = await self.loop.create_datagram_endpoint(Proto, local_addr=("0.0.0.0", self.port))
        self.thread = threading.Thread(target=self._reader, daemon=True)
        self.thread.start()

    def close(self):
        self.stop.set()
        if self.transport:
            self.transport.close()

    def status(self):
        now = time.time()
        return {"running": True, "port": self.port, "mode": self.mode, "ids": self.ids, "error": self.error,
                "clients": sum(1 for t in self.clients.values() if now - t < DSU_CLIENT_TIMEOUT_S)}

    def _send(self, addr, msg_type, payload):
        if self.transport:
            self.transport.sendto(dsu_packet(self.server_id, msg_type, payload), addr)

    def _request(self, data, addr):
        if len(data) < 20 or data[:4] != b"DSUC":
            return
        msg_type = struct.unpack_from("<I", data, 16)[0]
        if msg_type == 0x100000:            # protocol version
            self._send(addr, msg_type, struct.pack("<H", DSU_PROTOCOL))
        elif msg_type == 0x100001:          # slot info
            count = max(0, min(4, struct.unpack_from("<i", data, 20)[0])) if len(data) >= 24 else 0
            for slot in data[24:24 + count]:
                self._send(addr, msg_type, dsu_slot_info(slot, self.mode is not None) + b"\x00")
        elif msg_type == 0x100002:          # subscribe to pad data
            self.clients[addr] = time.time()

    def _publish(self, acc, gyr):
        now = time.time()
        self.counter += 1
        payload = dsu_pad_data(self.counter, int(time.monotonic() * 1e6), acc, gyr)
        for addr, t in list(self.clients.items()):
            if now - t > DSU_CLIENT_TIMEOUT_S:
                del self.clients[addr]
            else:
                self._send(addr, 0x100002, payload)

    def _reader(self):
        while not self.stop.is_set():
            path, mode, ids = find_motion_hidraw()
            self.ids = ids
            if not path:
                self.mode = None
                self.error = "" if ids else "dongle not found"
                self.stop.wait(1.0)
                continue
            try:
                fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
            except OSError as e:
                self.mode, self.error = None, str(e)
                self.stop.wait(1.0)
                continue
            self.mode, self.error = mode, ""
            last = 0.0
            try:
                while not self.stop.is_set():
                    r, _, _ = select.select([fd], [], [], 0.5)
                    if not r:
                        continue
                    data = os.read(fd, 128)
                    m = motion_from_report(mode, data)
                    if not m:
                        continue
                    # At most every 4 ms (250 Hz): plenty for emulators.
                    now = time.monotonic()
                    if now - last < 0.004:
                        continue
                    last = now
                    acc, gyr = dsu_motion(*m)
                    self.loop.call_soon_threadsafe(self._publish, acc, gyr)
            except OSError:
                pass   # unplugged or re-enumerated (mode change): look again
            finally:
                os.close(fd)
                self.mode = None


class Plugin:
    async def _main(self):
        self.dongle = Dongle()
        self.lock = asyncio.Lock()
        self.prefs_path = os.path.join(decky.DECKY_PLUGIN_SETTINGS_DIR, "prefs.json")
        self.prefs = {"check_updates": True, "dsu_enabled": False, "dsu_port": DSU_DEFAULT_PORT}
        try:
            with open(self.prefs_path) as f:
                self.prefs.update(json.load(f))
        except (OSError, ValueError):
            pass
        self.latest = None          # latest.json from GitHub
        self.latest_at = 0.0
        self.notified_version = None
        self.plugin_latest = None   # the plugin's latest.json
        self.plugin_latest_at = 0.0
        self.plugin_notified = None
        self.updating = False
        self.decky_enabled = False  # turned the dongle's Decky options on (once per connection)
        loop = asyncio.get_event_loop()
        self.monitor = loop.create_task(self._monitor())
        self.events = loop.create_task(self._events())
        self.dsu = None
        await self._dsu_apply()
        await asyncio.to_thread(install_udev_rule)
        decky.logger.info("Switch2-Pico plugin started")

    async def _unload(self):
        self.monitor.cancel()
        self.events.cancel()
        if self.dsu:
            self.dsu.close()

    # ---- Motion (DSU) server ---------------------------------------------
    async def _dsu_apply(self):
        """Start, restart (another port) or stop the server per the prefs."""
        want = bool(self.prefs.get("dsu_enabled"))
        port = int(self.prefs.get("dsu_port") or DSU_DEFAULT_PORT)
        if self.dsu and (not want or self.dsu.port != port):
            self.dsu.close()
            self.dsu = None
        self.dsu_error = ""
        if want and not self.dsu:
            server = DsuServer(asyncio.get_event_loop(), port)
            try:
                await server.start()
                self.dsu = server
                decky.logger.info("DSU server on UDP port %d", port)
            except OSError as e:
                server.close()
                self.dsu_error = "Port %d: %s" % (port, e.strerror or e)
                decky.logger.warning("DSU server: %s", self.dsu_error)

    async def get_dsu_status(self):
        if self.dsu:
            return self.dsu.status()
        return {"running": False, "port": int(self.prefs.get("dsu_port") or DSU_DEFAULT_PORT),
                "error": getattr(self, "dsu_error", ""), "clients": 0, "mode": None}

    # ---- Transport -------------------------------------------------------
    async def _request(self, method, path, body=b"", timeout_ms=3000):
        async with self.lock:
            return await asyncio.to_thread(self.dongle.exchange, method, path, body, timeout_ms)

    async def _json(self, method, path, body=b""):
        status, data = await self._request(method, path, body)
        if status != 200:
            raise DongleError(data.decode(errors="replace") or "error %d" % status)
        return json.loads(data)

    # ---- Called from the frontend ------------------------------------------
    async def get_status(self):
        """{"ok": true, "status": {...}} or {"ok": false, "error": "..."}"""
        try:
            return {"ok": True, "status": await self._json("GET", "/api/status")}
        except DongleError as e:
            return {"ok": False, "error": str(e)}

    async def get_settings(self):
        try:
            return {"ok": True, "settings": await self._json("GET", "/api/settings")}
        except DongleError as e:
            return {"ok": False, "error": str(e)}

    async def set_settings(self, form):
        """POST /api/settings with an urlencoded body (only the keys that change)."""
        try:
            return {"ok": True, "settings": await self._json("POST", "/api/settings", form.encode())}
        except DongleError as e:
            # A change of USB mode restarts the dongle before it answers.
            return {"ok": False, "error": str(e)}

    async def action(self, what):
        try:
            status, data = await self._request("POST", "/api/action?do=" + what)
            return {"ok": status == 200, "error": data.decode(errors="replace") if status != 200 else ""}
        except DongleError as e:
            return {"ok": False, "error": str(e)}

    async def get_prefs(self):
        return self.prefs

    async def set_pref(self, key, value):
        if key == "dsu_port":
            value = int(value)
            if not 1024 <= value <= 65535:
                return self.prefs
        self.prefs[key] = value
        try:
            os.makedirs(os.path.dirname(self.prefs_path), exist_ok=True)
            with open(self.prefs_path, "w") as f:
                json.dump(self.prefs, f)
        except OSError as e:
            decky.logger.warning("prefs not saved: %s", e)
        if key in ("dsu_enabled", "dsu_port"):
            await self._dsu_apply()
        return self.prefs

    async def check_update(self, force=False):
        """{"ok", "latest", "file", "available"} for the connected dongle."""
        try:
            if force or not self.latest or time.time() - self.latest_at > 600:
                data = await asyncio.to_thread(http_get, FW_BASE + "latest.json?%d" % int(time.time()))
                self.latest = json.loads(data)
                self.latest_at = time.time()
        except Exception as e:
            return {"ok": False, "error": "Couldn't reach GitHub (%s)" % e}
        st = await self.get_status()
        if not st["ok"]:
            return {"ok": True, "latest": self.latest.get("version"), "available": False, "file": None}
        status = st["status"]
        file = (self.latest.get("files") or {}).get(status.get("platform"))
        avail = bool(file) and vcmp(self.latest.get("version"), status.get("version")) > 0
        return {"ok": True, "latest": self.latest.get("version"), "file": file, "available": avail,
                "installed": status.get("version"), "platform": status.get("platform")}

    async def install_update(self):
        """Download the latest firmware and install it; progress arrives as
        "fw_progress" events (stage, percent). Returns {"ok", "error"}."""
        if self.updating:
            return {"ok": False, "error": "an update is already running"}
        self.updating = True
        try:
            up = await self.check_update(force=True)
            if not up.get("ok") or not up.get("file"):
                raise DongleError(up.get("error") or "no firmware for this board")
            await decky.emit("fw_progress", "download", 0)
            data = await asyncio.to_thread(http_get, FW_BASE + up["file"] + "?%s" % self.latest.get("sha", ""), 60)
            info = await self._json("GET", "/api/fw/info")
            img = uf2_image(data, info["family"], info["base"], info["max"])
            crc = zlib.crc32(img) & 0xFFFFFFFF
            await decky.emit("fw_progress", "check", 0)

            async def post(path, body=b""):
                status, resp = await self._request("POST", path, body, 10000)
                if status != 200:
                    raise DongleError(resp.decode(errors="replace") or "error %d" % status)

            await post("/api/fw/begin?size=%d&crc=%d" % (len(img), crc))
            for off in range(0, len(img), FW_CHUNK):
                await post("/api/fw/chunk?off=%d" % off, img[off:off + FW_CHUNK])
                await decky.emit("fw_progress", "write", off * 100 // len(img))
            await post("/api/fw/end")
            await decky.emit("fw_progress", "restart", 100)
            try:
                await self._request("POST", "/api/action?do=reboot")
            except DongleError:
                pass
            decky.logger.info("firmware %s installed", up["latest"])
            return {"ok": True, "version": up["latest"]}
        except Exception as e:
            decky.logger.error("update failed: %s", e)
            return {"ok": False, "error": str(e)}
        finally:
            self.updating = False

    # ---- Plugin self-update ------------------------------------------------
    async def check_plugin_update(self, force=False):
        """{"ok", "installed", "latest", "available"} for this plugin."""
        installed = self._installed_version()
        try:
            if force or not self.plugin_latest or time.time() - self.plugin_latest_at > 600:
                data = await asyncio.to_thread(http_get, PLUGIN_BASE + "latest.json?%d" % int(time.time()))
                self.plugin_latest = json.loads(data)
                self.plugin_latest_at = time.time()
        except Exception as e:
            return {"ok": False, "installed": installed, "error": "Couldn't reach GitHub (%s)" % e}
        latest = self.plugin_latest.get("version", "0")
        # The frontend hands url / sha256 to Decky's own installer.
        return {"ok": True, "installed": installed, "latest": latest, "available": vcmp(latest, installed) > 0,
                "url": PLUGIN_BASE + self.plugin_latest.get("file", "Switch2-Pico.zip"),
                "sha256": self.plugin_latest.get("sha256", "")}

    def _installed_version(self):
        # package.json next to this file is what was installed (and what an
        # update replaces); Decky's own idea of it as a fallback.
        try:
            with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "package.json")) as f:
                return json.load(f)["version"]
        except (OSError, ValueError, KeyError):
            return getattr(decky, "DECKY_PLUGIN_VERSION", "0")

    async def install_plugin_update(self):
        """Fallback when Decky's installer isn't reachable from the frontend:
        download the latest plugin, put its files in place and restart Decky.
        (Decky may not let a plugin write its own folder.)"""
        try:
            up = await self.check_plugin_update(force=True)
            if not up.get("ok"):
                raise DongleError(up.get("error"))
            info = self.plugin_latest
            data = await asyncio.to_thread(http_get, PLUGIN_BASE + info["file"] + "?%s" % info.get("sha256", "")[:12], 60)
            if info.get("sha256") and hashlib.sha256(data).hexdigest() != info["sha256"]:
                raise DongleError("download corrupted (checksum mismatch)")
            await asyncio.to_thread(self._replace_plugin, data)
            decky.logger.info("plugin %s installed; restarting Decky", info.get("version"))
            # Restart once this call has answered.
            asyncio.get_event_loop().call_later(1.0, lambda: subprocess.Popen(
                ["systemctl", "restart", "plugin_loader"], start_new_session=True, env=system_env()))
            return {"ok": True, "version": info.get("version")}
        except Exception as e:
            decky.logger.error("plugin update failed: %s", e)
            return {"ok": False, "error": str(e)}

    def _replace_plugin(self, data):
        dest = decky.DECKY_PLUGIN_DIR
        with zipfile.ZipFile(io.BytesIO(data)) as z, tempfile.TemporaryDirectory() as tmp:
            members = {}
            for name in z.namelist():
                # Switch2-Pico/<file>: only the plugin's own files.
                parts = name.split("/", 1)
                if len(parts) == 2 and parts[1] in PLUGIN_FILES:
                    members[parts[1]] = name
            missing = [f for f in ("main.py", "plugin.json", "dist/index.js") if f not in members]
            if missing:
                raise DongleError("not a Switch2-Pico plugin package (missing %s)" % ", ".join(missing))
            for rel, name in members.items():
                path = os.path.join(tmp, rel)
                os.makedirs(os.path.dirname(path), exist_ok=True)
                with z.open(name) as src, open(path, "wb") as out:
                    shutil.copyfileobj(src, out)
            # Decky leaves plugin folders read-only: writable again first
            # (as other self-updating plugins do).
            for root, dirs, files in os.walk(dest):
                for n in [root] + [os.path.join(root, f) for f in files]:
                    try:
                        os.chmod(n, os.stat(n).st_mode | 0o200)
                    except OSError:
                        pass
            # Everything extracted: now swap each file in place.
            for rel in members:
                target = os.path.join(dest, rel)
                os.makedirs(os.path.dirname(target), exist_ok=True)
                shutil.copyfile(os.path.join(tmp, rel), target + ".new")
                os.replace(target + ".new", target)

    async def _enable_decky_options(self):
        """The plugin is here, so the dongle's configuration page shows the
        options that need it ("Show additional Decky options")."""
        if self.decky_enabled:
            return
        s = await self._json("GET", "/api/settings")
        if "decky_options" not in s:
            self.decky_enabled = True   # firmware without them
            return
        if not s["decky_options"]:
            await self._json("POST", "/api/settings", b"decky_options=1")
            decky.logger.info("turned on the dongle's Decky options")
        self.decky_enabled = True

    # ---- Background: button events ------------------------------------------
    async def _events(self):
        """Presses of a button mapped to the Quick Access menu, counted by the
        dongle (GET /api/events), as "qam" events to the frontend, which opens
        or closes the menu."""
        seen = None   # the dongle's count at the last poll
        while True:
            try:
                await asyncio.sleep(EVENTS_S)
                if self.updating:
                    continue
                try:
                    status, data = await self._request("GET", "/api/events", timeout_ms=500)
                except DongleError:
                    seen = None
                    await asyncio.sleep(1.0)   # unplugged, restarting or in use
                    continue
                if status != 200:
                    seen = None
                    await asyncio.sleep(30.0)  # firmware without events
                    continue
                n = json.loads(data).get("qam", 0)
                # The first poll (and after a restart) only sets the baseline.
                if seen is not None and n != seen:
                    await decky.emit("qam")
                seen = n
            except asyncio.CancelledError:
                raise
            except Exception as e:
                decky.logger.warning("events: %s", e)
                await asyncio.sleep(1.0)

    # ---- Background: notifications -----------------------------------------
    async def _monitor(self):
        """Profile switches and new firmware or plugin versions, as "notify"
        events the frontend shows as toasts (Steam itself shows controller
        connects and low batteries)."""
        running = None   # (profile, USB mode) the dongle runs
        while True:
            try:
                await asyncio.sleep(POLL_S)
                if self.updating:
                    continue
                st = await self.get_status()
                if not st["ok"]:
                    self.decky_enabled = False
                    continue
                s = st["status"]
                await self._enable_decky_options()
                # Profile switched (C + Home on the controller, or here).
                cur = (s.get("profile"), s.get("usb_mode"))
                if running and cur != running:
                    await decky.emit("notify", "profile", cur[0] or "", cur[1] or "")
                running = cur
                if self.prefs.get("check_updates") and time.time() - self.latest_at > UPDATE_CHECK_S:
                    up = await self.check_update(force=True)
                    if up.get("available") and up["latest"] != self.notified_version:
                        self.notified_version = up["latest"]
                        await decky.emit("notify", "update", up["latest"], "")
                    pu = await self.check_plugin_update(force=True)
                    if pu.get("available") and pu["latest"] != self.plugin_notified:
                        self.plugin_notified = pu["latest"]
                        await decky.emit("notify", "plugin", pu["latest"], "")
            except asyncio.CancelledError:
                raise
            except Exception as e:
                decky.logger.warning("monitor: %s", e)
