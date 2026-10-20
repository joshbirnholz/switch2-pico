#!/usr/bin/env python3
"""Scan for BLE advertisements and flag Switch 2 controllers.

Nintendo's manufacturer data uses company id 0x0553. Offsets in
headset-audio.md include the 2-byte company id; bleak strips it, so
subtract 2 here.

Usage: .venv/bin/python research/audio/scan.py [seconds]
"""
import asyncio
import sys

from bleak import BleakScanner

NINTENDO = 0x0553
PIDS = {0x2069: "Pro Controller 2", 0x2066: "Joy-Con 2 (R)", 0x2067: "Joy-Con 2 (L)"}


async def main(seconds: float) -> None:
    seen = {}

    def on_adv(dev, adv):
        first = dev.address not in seen
        seen[dev.address] = (adv.rssi, adv.local_name)
        md = adv.manufacturer_data.get(NINTENDO)
        if md is None or not first:
            return
        vid = int.from_bytes(md[3:5], "little") if len(md) >= 5 else None
        pid = int.from_bytes(md[5:7], "little") if len(md) >= 7 else None
        wake = md[9] if len(md) > 9 else None
        host = md[10:16].hex(":") if len(md) >= 16 else None
        print(f"NINTENDO {dev.address} rssi={adv.rssi} "
              f"vid={vid:#06x} pid={pid:#06x} ({PIDS.get(pid, '?')}) "
              f"wake={wake:#04x} host={host} raw={md.hex()}", flush=True)

    async with BleakScanner(on_adv):
        await asyncio.sleep(seconds)

    print(f"\n{len(seen)} advertisers seen in {seconds:.0f} s")
    for addr, (rssi, name) in sorted(seen.items(), key=lambda kv: -kv[1][0])[:15]:
        print(f"  {addr} {rssi:4d} dBm {name or ''}")


if __name__ == "__main__":
    asyncio.run(main(float(sys.argv[1]) if len(sys.argv) > 1 else 15))
