# Switch2-Pico for Decky Loader

Quick controls for the Switch2-Pico dongle from the Steam Deck's Quick Access
menu (Decky Loader), on a Steam Deck or any SteamOS / Linux machine with Decky.

* Status of the connected controller (battery icons, what the dongle emulates).
* **Pair a controller**.
* **Profile** for the connected controller type (pop-up list with the
  profile's button and emulated controller).
* **Extra buttons** of the connected controller (C, Capture, GL / GR, the
  Joy-Con 2 SL / SR): pick what each sends from a list with button icons.
* **Remap from the controller** (C + GL / GR or SL / SR) where available.
* **Mouse Mode** for Joy-Con 2 (Xbox 360 profiles): on / off, speed, scrolling.
* **Paired controllers**: forget one or all.
* **Only on USB while a controller is connected** (the dongle's setting).
* **Reboot dongle**.
* **Firmware updates**: checks GitHub on start and once a day, and installs
  the update over USB.
* **Plugin updates**: checks for a newer version of itself the same way and
  installs it through Decky's own installer (as Decky's store does), which
  reloads the plugin.
* Notifications: a profile switch (C + Home), a firmware or plugin update
  available (Steam itself shows controller connects and low batteries).

## How it talks to the dongle

Over USB, through the dongle's configuration interface ("Switch2-Pico
Config", the one the configuration page uses with WebUSB) and the same API
(`core/src/web_api.c`). The backend (`main.py`) uses Linux usbdevfs directly,
so it needs no extra Python packages; it runs as root (Decky flag `_root`)
for access to `/dev/bus/usb`. The interface is claimed only during each
request, so the configuration page in a browser can still be used (but not
at the same moment).

Firmware updates use `latest.json` and the `.uf2` files CI publishes to
GitHub Pages (`https://joshbirnholz.github.io/switch2-pico/fw/`), the same
ones the configuration page's Update button uses. CI also builds this plugin
and publishes it at `…/decky/` (`latest.json` with its version and SHA-256,
and `Switch2-Pico.zip`); the plugin compares that version with its own
(`package.json`), so bump `version` there for a release. Both are published
from the default branch.

## Install

1. Build (or take `Switch2-Pico.zip` from a release):
   ```sh
   pnpm install
   pnpm build
   ./package.sh          # -> out/Switch2-Pico.zip
   ```
2. On the Deck: Decky settings → enable **Developer mode**, then Developer →
   **Install Plugin from ZIP File** and pick `Switch2-Pico.zip`.

## Icons

Button icons are Kenney's [Input Prompts](https://kenney.nl/assets/input-prompts)
(CC0), embedded by `scripts/gen_icons.py` into `src/kenney.ts`:

```sh
python3 scripts/gen_icons.py /path/to/kenney_input-prompts
```
