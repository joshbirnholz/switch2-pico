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
import shutil
import ssl
import struct
import subprocess
import tempfile
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


class Plugin:
    async def _main(self):
        self.dongle = Dongle()
        self.lock = asyncio.Lock()
        self.prefs_path = os.path.join(decky.DECKY_PLUGIN_SETTINGS_DIR, "prefs.json")
        self.prefs = {"check_updates": True}
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
        self.monitor = asyncio.get_event_loop().create_task(self._monitor())
        decky.logger.info("Switch2-Pico plugin started")

    async def _unload(self):
        self.monitor.cancel()

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
        self.prefs[key] = value
        try:
            os.makedirs(os.path.dirname(self.prefs_path), exist_ok=True)
            with open(self.prefs_path, "w") as f:
                json.dump(self.prefs, f)
        except OSError as e:
            decky.logger.warning("prefs not saved: %s", e)
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
        return {"ok": True, "installed": installed, "latest": latest, "available": vcmp(latest, installed) > 0}

    def _installed_version(self):
        # package.json next to this file is what was installed (and what an
        # update replaces); Decky's own idea of it as a fallback.
        try:
            with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "package.json")) as f:
                return json.load(f)["version"]
        except (OSError, ValueError, KeyError):
            return getattr(decky, "DECKY_PLUGIN_VERSION", "0")

    async def install_plugin_update(self):
        """Download the latest plugin, put its files in place and restart
        Decky (which reloads every plugin, this one included)."""
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
                ["systemctl", "restart", "plugin_loader"], start_new_session=True))
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
            # Everything extracted: now swap each file in place.
            for rel in members:
                target = os.path.join(dest, rel)
                os.makedirs(os.path.dirname(target), exist_ok=True)
                shutil.copyfile(os.path.join(tmp, rel), target + ".new")
                os.replace(target + ".new", target)

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
                    continue
                s = st["status"]
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
