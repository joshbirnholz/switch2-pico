#!/bin/sh
# Build the nRF52840 firmware and convert it to a drag-and-drop .uf2.
#
# Requires arduino-cli with the Adafruit nRF52 core installed:
#   arduino-cli config add board_manager.additional_urls \
#       https://adafruit.github.io/arduino-board-index/package_adafruit_index.json
#   arduino-cli core update-index && arduino-cli core install adafruit:nrf52
#
# Usage: nrf52/build.sh [FQBN]   (default adafruit:nrf52:feather52840, which
#        also suits Pro Micro nRF52840 / nice!nano boards)
set -e
cd "$(dirname "$0")/.."

FQBN="${1:-adafruit:nrf52:feather52840}"
OUT=build-nrf

# --wrap: the BOS descriptor is the core's (main_nrf.cpp), and the saved log
# (log_journal_nrf.cpp) takes the flash completion
# events for its own writes before they reach InternalFS.
arduino-cli compile -b "$FQBN" --warnings default --library core --output-dir "$OUT" \
    --build-property "compiler.c.elf.extra_flags=-Wl,--wrap=flash_nrf5x_event_cb -Wl,--wrap=tud_descriptor_bos_cb" nrf52/switch2_nrf

# Find uf2conv.py in the installed core (arduino-cli data dir or sketchbook).
UF2CONV=$(find "$(arduino-cli config get directories.data 2>/dev/null || echo "$HOME/.arduino15")" \
               "$HOME/Arduino/hardware" -name uf2conv.py -path '*nrf52*' 2>/dev/null | head -n 1)
if [ -z "$UF2CONV" ]; then
    echo "uf2conv.py not found; the .hex is in $OUT" >&2
    exit 1
fi
python3 "$UF2CONV" -f 0xADA52840 -c -o "$OUT/switch2_nrf52840.uf2" "$OUT/switch2_nrf.ino.hex"
echo "Firmware: $OUT/switch2_nrf52840.uf2"

# The same firmware with the S140 v6.1.1 SoftDevice in front of it: the file
# to copy onto the board's drive, since it also works on boards without the
# SoftDevice. The configuration page's Update keeps using the smaller file
# above. The SoftDevice comes from the core's bootloader images.
CORE_DIR=$(dirname "$UF2CONV")/../..
SD_HEX=$(find "$CORE_DIR" -path '*bootloader*' -name '*feather_nrf52840_express*s140_6.1.1.hex' 2>/dev/null | sort | tail -n 1)
[ -n "$SD_HEX" ] || SD_HEX=$(find "$CORE_DIR" -path '*bootloader*' -name '*nrf52840*s140_6.1.1.hex' 2>/dev/null | sort | tail -n 1)
if [ -z "$SD_HEX" ]; then
    echo "S140 v6.1.1 .hex not found in the core; only the application .uf2 was made" >&2
    exit 1
fi
python3 tools/add_softdevice_uf2.py "$SD_HEX" "$OUT/switch2_nrf52840.uf2" "$OUT/switch2_nrf52840_full.uf2"
