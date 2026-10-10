# Switch2-Pico for Decky Loader

Quick controls for the Switch2-Pico dongle from the Steam Deck's Quick Access
menu (Decky Loader), on a Steam Deck or any SteamOS / Linux machine with Decky.

* At the top, one button with the connected controller (battery icons) and
  the profile in use (its button, name and emulated controller); it opens
  the profiles.
* **Profile** for the connected controller type: a pop-up list with each
  profile's button and emulated controller. Pick one to use it; the pencil
  edits one (name, emulated controller, button mapping, sticks, rumble,
  Mouse Mode, and Delete, which asks first); **Add a profile** makes a new
  one on a free button with that controller's defaults. Changes are saved
  right away.
* **Button mapping** for the connected controller: its extra buttons
  (Capture, C, GL / GR, the Joy-Con 2 SL / SR) in the panel, and **Show all**
  for every button; pick what each sends from a list with button icons.
  In Show all the changes apply together on leaving (**Save**, or Back on
  the controller); **Cancel** asks before discarding them.
* **Remap from the controller** (C + GL / GR or SL / SR) where available.
* **Quick Access menu from the controller**: a button mapped to **Quick
  Access Menu (Decky)** (here or on the configuration page) opens Steam's
  Quick Access menu, or closes it when it's open. The plugin asks the dongle
  for presses every 50 ms, and turns on the dongle's *Show additional Decky
  options* so the configuration page lists this output. Provisional.
* **Mouse Mode** for Joy-Con 2 (Xbox 360 and SInput profiles): on / off, speed, scrolling.
* **SInput mode**: on start the plugin adds a udev rule
  (`/etc/udev/rules.d/70-switch2-pico.rules`) so Steam can read the dongle
  through hidraw in SInput mode, unless a rule for its ID (2e8a:10c6) is
  already there.
* **Paired controllers**: pair a controller, forget one or all.
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
