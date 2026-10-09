#!/bin/sh
# Package the built plugin for Decky's "Install Plugin from ZIP File".
set -e
cd "$(dirname "$0")"
[ -f dist/index.js ] || { echo "run pnpm build first" >&2; exit 1; }
rm -rf out && mkdir -p out/Switch2-Pico/dist
cp dist/index.js out/Switch2-Pico/dist/
cp main.py plugin.json package.json LICENSE README.md out/Switch2-Pico/
(cd out && zip -qr Switch2-Pico.zip Switch2-Pico)
echo out/Switch2-Pico.zip
