# Install a UF2 on a running switch2-pico dongle through its update API
# (/api/fw/*, as decky-plugin/main.py does). fw_push.py FILE.uf2
import fcntl, json, os, struct, sys, time, zlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from audio_preflight import require_idle, require_healthy_audio
from dongle_bootloader import USBDEVFS_CLAIMINTERFACE, USBDEVFS_RELEASEINTERFACE, bulk, find
CHUNK = int(os.environ.get("CHUNK", "4096"))

def request(method, path, body=b""):
    for attempt in range(50):
        bus, dev, itf, eps = find()
        fd = os.open(f"/dev/bus/usb/{bus:03d}/{dev:03d}", os.O_RDWR)
        try:
            try:
                fcntl.ioctl(fd, USBDEVFS_CLAIMINTERFACE, struct.pack("I", itf))
            except OSError as e:
                if e.errno == 16: time.sleep(0.1); continue   # the plugin has it
                raise
            payload = f"{method} {path}\n".encode() + body
            msg = b"S2PQ" + struct.pack("<I", len(payload)) + payload
            bulk(fd, eps["out"], msg, len(msg), 10000)
            got, need = b"", 12
            while len(got) < need:
                got += bulk(fd, eps["in"], None, 4096, 10000)
                if need == 12 and len(got) >= 12:
                    need = 12 + struct.unpack_from("<I", got, 8)[0]
            return struct.unpack_from("<H", got, 4)[0], got[12:need]
        finally:
            try: fcntl.ioctl(fd, USBDEVFS_RELEASEINTERFACE, struct.pack("I", itf))
            except OSError: pass
            os.close(fd)
    raise SystemExit("interface busy")

def uf2_image(data, family, base, max_len):
    blocks, end = [], 0
    for o in range(0, len(data) - 511, 512):
        m0, m1, flags, addr, length = struct.unpack_from("<IIIII", data, o)
        if m0 != 0x0A324655 or m1 != 0x9E5D5157 or flags & 1: continue
        if flags & 0x2000 and struct.unpack_from("<I", data, o + 28)[0] != family: continue
        assert length <= 476 and addr >= base and addr + length - base <= max_len, "doesn't fit"
        blocks.append((addr - base, o + 32, length)); end = max(end, addr + length - base)
    img = bytearray(b"\xff" * end)
    for off, src, length in blocks: img[off:off + length] = data[src:src + length]
    return bytes(img)

def post(path, body=b""):
    require_idle()  # recheck before every write and the restart
    st, r = request("POST", path, body)
    if st != 200: raise SystemExit(f"{path}: {st} {r[:200]!r}")

def main():
    require_idle()
    require_healthy_audio()
    st, r = request("GET", "/api/fw/info"); info = json.loads(r)
    img = uf2_image(open(sys.argv[1], "rb").read(), info["family"], info["base"], info["max"])
    crc = zlib.crc32(img) & 0xFFFFFFFF
    print("image", len(img), "bytes, crc", hex(crc), flush=True)
    post(f"/api/fw/begin?size={len(img)}&crc={crc}")
    for off in range(0, len(img), CHUNK):
        post(f"/api/fw/chunk?off={off}", img[off:off + CHUNK])
    post("/api/fw/end")
    require_healthy_audio()
    print("written; requesting restart", flush=True)
    try: post("/api/action?do=reboot")
    except OSError: pass  # the USB interface can disappear after the request
    print("Restart requested; reconnect the controller after the dongle returns.", flush=True)


if __name__ == "__main__":
    main()
