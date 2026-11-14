"""tools/add_softdevice_uf2.py: the full nRF52840 .uf2 (S140 + application),
and the Decky plugin's uf2_image() skipping its SoftDevice part."""
import importlib.util
from pathlib import Path
import struct
import sys
import tempfile
import types
import unittest
from unittest.mock import Mock

ROOT = Path(__file__).resolve().parents[1]


def load(name, path, modules=None):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    saved = dict(sys.modules)
    sys.modules.update(modules or {})
    try:
        spec.loader.exec_module(mod)
    finally:
        sys.modules.clear()
        sys.modules.update(saved)
    return mod


tool = load("add_softdevice_uf2", ROOT / "tools" / "add_softdevice_uf2.py")


def hex_file(mem):
    """Intel HEX for {address: byte}, 16 bytes a line, with type-04 records."""
    lines, upper = [], None
    addrs = sorted(mem)
    i = 0
    while i < len(addrs):
        a = addrs[i]
        if a >> 16 != upper:
            upper = a >> 16
            rec = bytes([2, 0, 0, 4]) + upper.to_bytes(2, "big")
            lines.append(":" + (rec + bytes([-sum(rec) & 0xFF])).hex().upper())
        run = [mem[a]]
        while len(run) < 16 and i + len(run) < len(addrs) and addrs[i + len(run)] == a + len(run) and (a + len(run)) >> 16 == upper:
            run.append(mem[a + len(run)])
        rec = bytes([len(run), (a >> 8) & 0xFF, a & 0xFF, 0]) + bytes(run)
        lines.append(":" + (rec + bytes([-sum(rec) & 0xFF])).hex().upper())
        i += len(run)
    lines.append(":00000001FF")
    return "\n".join(lines) + "\n"


def fake_softdevice(end=0x25DE8, fwid=0xB6):
    mem = {a: 0xFF for a in range(0, 0x1000)}               # MBR (must be left out)
    mem.update({a: (a * 7) & 0xFF for a in range(0x1000, end)})
    for off, val in ((4, tool.SD_MAGIC), (8, 0x26000), (0xC, fwid)):
        for k, b in enumerate(val.to_bytes(4, "little")):
            mem[0x3000 + off + k] = b
    mem.update({a: 0xAA for a in range(0xF4000, 0xF4100)})  # bootloader (left out too)
    return mem


def app_uf2(path, start=0x26000, blocks=3):
    out = bytearray()
    for n in range(blocks):
        out += struct.pack("<8I", tool.UF2_MAGIC0, tool.UF2_MAGIC1, tool.FLAG_FAMILY, start + n * 256, 256, n, blocks, tool.FAMILY)
        out += bytes([n + 1]) * 256 + bytes(220) + struct.pack("<I", tool.UF2_MAGIC_END)
    Path(path).write_bytes(out)


class FullUf2Tests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.d = Path(self.dir.name)

    def tearDown(self):
        self.dir.cleanup()

    def run_tool(self, mem):
        (self.d / "sd.hex").write_text(hex_file(mem))
        app_uf2(self.d / "app.uf2")
        tool.main(["x", str(self.d / "sd.hex"), str(self.d / "app.uf2"), str(self.d / "full.uf2")])
        return (self.d / "full.uf2").read_bytes()

    def test_full_image_has_softdevice_then_app(self):
        data = self.run_tool(fake_softdevice())
        n = len(data) // 512
        addrs = []
        for i in range(n):
            m0, m1, flags, addr, size, no, num, fam = struct.unpack_from("<8I", data, i * 512)
            self.assertEqual((m0, m1, flags, no, num, fam), (tool.UF2_MAGIC0, tool.UF2_MAGIC1, tool.FLAG_FAMILY, i, n, tool.FAMILY))
            self.assertEqual(struct.unpack_from("<I", data, i * 512 + 508)[0], tool.UF2_MAGIC_END)
            addrs.append(addr)
        self.assertEqual(addrs[0], 0x1000)                   # no MBR
        self.assertEqual(max(addrs), 0x26000 + 2 * 256)       # no bootloader
        self.assertEqual(addrs[-3:], [0x26000, 0x26100, 0x26200])
        # The SoftDevice bytes are copied as they are.
        sd_bytes = b"".join(data[i * 512 + 32:i * 512 + 32 + 256] for i in range(n - 3))
        self.assertEqual(sd_bytes[0x10:0x14], bytes(((0x1010 + k) * 7) & 0xFF for k in range(4)))

    def test_refuses_incomplete_or_wrong_softdevice(self):
        with self.assertRaisesRegex(ValueError, "incomplete"):
            self.run_tool(fake_softdevice(end=0x4200))       # what a failed serial update leaves
        with self.assertRaisesRegex(ValueError, "not S140"):
            self.run_tool(fake_softdevice(fwid=0x100))
        with self.assertRaisesRegex(ValueError, "no SoftDevice"):
            self.run_tool({a: 0xFF for a in range(0x1000, 0x26000)})

    def test_decky_extracts_only_the_application(self):
        data = self.run_tool(fake_softdevice())
        decky = types.ModuleType("decky")
        decky.logger = Mock()
        backend = load("test_sd_decky_backend", ROOT / "decky-plugin" / "main.py", {"decky": decky})
        img = backend.uf2_image(data, tool.FAMILY, 0x26000, 0xC7000)
        self.assertEqual(img, bytes([1]) * 256 + bytes([2]) * 256 + bytes([3]) * 256)


if __name__ == "__main__":
    unittest.main()
