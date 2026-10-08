# Switch2-Pico

Firmware that turns a **Raspberry Pi Pico 2 W** or an **nRF52840 board**
(Pro Micro nRF52840 / nice!nano, Adafruit Feather nRF52840, ...) into a
wireless USB dongle for the **Nintendo Switch 2 Pro Controller**. The
controller pairs to the dongle over Bluetooth LE, and the dongle shows up over
USB as a wired
**Nintendo Switch (1) Pro Controller**. That means it works anywhere a Pro
Controller works: Steam (Windows, macOS, Linux, Steam Deck), SDL games,
emulators, Chrome's Gamepad API, Linux's `hid-nintendo` driver, and a
Switch 1 console.

```
Switch 2 Pro Controller  ──BLE──▶  dongle    ──USB──▶  PC / Mac / Switch 1
     (proprietary GATT)            (this firmware)      sees "Pro Controller" 057E:2009
```

> Unofficial project, not affiliated with or endorsed by Nintendo.
> **Status: early, not yet tested on real hardware.** The firmware builds
> and its protocol logic has unit tests, but the Bluetooth and console paths
> have not been run against a real controller yet. See
> [What is verified](#what-is-verified) before relying on it.

## Features

| Feature | Notes |
| --- | --- |
| Pairing | Hold **Sync** on the controller. Uses Nintendo's own key-exchange commands; standard Bluetooth pairing would make the controller disconnect. The dongle remembers the controller. |
| Reconnect / wake | After pairing, pressing any button reconnects. While the PC sleeps, the controller is let go so it can sleep too; a button press can wake the PC (USB remote wakeup; on Linux see [Waking the PC](#on-a-pc)). |
| Buttons and sticks | Uses the controller's own calibration, with a configurable radial deadzone. |
| GL / GR / C | Each can be mapped to any Pro Controller button (all other buttons can be remapped too), on the configuration page or from the controller (hold C + GL/GR and press a button). Defaults: GL → left stick click, GR → right stick click, C → unassigned. |
| Gyro and accelerometer | Converted to the Switch 1 axis layout and units. The gyro scale is auto-detected, can be calibrated, and sensitivity is adjustable. |
| HD rumble | The host's Switch 1 HD rumble frames, including the packed multi-sample formats, are decoded and re-encoded for the Switch 2 actuators, left and right separately. |
| Player LEDs | Follow the player number the host assigns. |
| Battery | Reported to the host. |
| USB modes | **Switch Pro Controller** (default; Switch consoles and PCs), **DualSense Edge** (GL, GR and C become its back paddles and Fn button, so Steam Input can map them), **DualSense**, or **Xbox 360 controller**. Each mode has its own button map. |
| NSO GameCube controller | Basic support: buttons, sticks, analog triggers acting as L/R past a threshold, and rumble through built-in vibration presets. |
| Configuration page | Over **WebUSB** in Chrome/Edge with the dongle plugged in, or (Pico 2 W only) over the dongle's own Wi-Fi from any phone or computer. |

## Hardware

Two boards are supported; they share all of the controller logic.

| | Raspberry Pi Pico 2 W | nRF52840 (Pro Micro / nice!nano, Feather, ...) |
| --- | --- | --- |
| Bluetooth | CYW43439 (BTstack), shares its radio with Wi-Fi | Nordic SoftDevice S140, Bluetooth only |
| Configuration | WebUSB or Wi-Fi page | WebUSB |
| Buttons on the board | BOOTSEL: tap = Wi-Fi page, hold 5 s = forget controller | none (use the configuration page) |
| Firmware file | `switch2_pico.uf2` | `switch2_nrf52840.uf2` |

A Pico W (RP2040) build also works: configure with `-DPICO_BOARD=pico_w`.
The nRF52840 build needs a board with the Adafruit nRF52 UF2 bootloader and
S140 v6.1.1 SoftDevice. That is the bootloader Pro Micro nRF52840 and
nice!nano boards ship with, the same as for openpuck.

## Installing

Download the nRF52840 `.uf2` from the
[**Latest build** release](https://github.com/joshbirnholz/switch2-pico/releases/tag/latest)
(rebuilt on every push; each CI run also keeps it as an artifact), or
[build it](#building). The Pico 2 W isn't a focus at the moment: CI builds it
only on request (**Actions → Build → Run workflow → Also build the Pico 2 W
firmware**), and the configuration page doesn't offer it as an update. Then:

* **Pico 2 W:** hold **BOOTSEL** while plugging it in; a drive named `RP2350`
  appears. Copy the `.uf2` file onto it.
* **nRF52840:** double-tap reset (on a Pro Micro without a reset button,
  short RST to GND twice quickly); a drive such as `NICENANO` or
  `FTHR840BOOT` appears. Copy the `…-nrf52840.uf2` file onto it.

The board reboots into the firmware.

**Updating:** the configuration page's **Firmware update** section shows the
installed and latest version. **Update** downloads the latest build and sends
it straight to the dongle over the configuration connection (from v0.5.2):
the dongle stores it in spare flash, checks it (CRC-32 and vector table),
installs it at the next start and restarts on its own. No drive to pick. The
copy writes the image's first page last, so a power cut mid-install leaves the
board in its UF2 bootloader rather than running a half-written image; plug it
in and install the .uf2 by hand.

Older firmware, or a failed direct update, falls back to the drive method: the
dongle restarts as its USB drive and (in Chrome or Edge) the page writes the
file once you pick the `NICENANO` / `FTHR840BOOT` / `RP2350` drive. In other
browsers use *Save the .uf2 instead* and drag the file onto the drive.
**Firmware update mode** only restarts into the drive, for installing a file
by hand.

**Install a .uf2 file…** in the same section installs a file from your
computer (a local build, or an older release to go back to) the same way:
directly when the firmware supports it, otherwise through the drive. A file
for a different board is refused.

## Pairing and everyday use

1. Plug in the dongle. Its LED blinks fast because no controller is paired yet.
2. Hold the small **Sync** button on the controller until the player lights sweep.
3. The dongle connects, pairs, and the controller gives a short "click"
   rumble. The LED stays on.
4. Next time, just press any button on the controller.

To pair a different controller, hold Sync on the new one. It replaces the old
pairing. To forget the paired controller, hold the Pico's BOOTSEL button for
5 seconds, or use the configuration page.

| Pico LED | Meaning |
| --- | --- |
| fast blink | waiting for a controller in pairing mode |
| short flash every 2 s | waiting for the paired controller to wake up |
| flicker | connecting |
| solid | connected |
| double blink | configuration Wi-Fi is on |
| 1–4 quick blinks, pause, repeat | the last connection attempt failed (see [Troubleshooting](#troubleshooting)); shown for 60 s |

### On a PC

Steam, SDL and the Linux kernel driver all treat the dongle as a wired Pro
Controller, with gyro and rumble. In Steam, enable *Switch Controller Support*
in Settings → Controller.

**Waking the PC with the controller.** While the PC sleeps, a button press on
the controller asks the PC to wake up. The PC has to allow it:

* **Linux / SteamOS:** Linux only lets keyboards wake the PC by default.
  Install the rule from `tools/99-switch2-pico.rules` once (in SteamOS Desktop
  Mode, from Konsole) and replug the dongle:
  `sudo cp tools/99-switch2-pico.rules /etc/udev/rules.d/ && sudo udevadm control --reload-rules`.
  `cat /sys/bus/usb/devices/*/power/wakeup` should then list one more `enabled`.
* **Windows:** Device Manager → the dongle → Power Management → *Allow this
  device to wake the computer* (Windows may only offer this for keyboards and
  mice).

**Sleeping when unused.** After 15 minutes without a button press or stick
movement the dongle disconnects the controller so it goes to sleep and saves
its battery; press any button to reconnect (within the first 20 seconds
after it disconnects, the controller's own reconnection attempts are
ignored). Change the time or turn it off in the configuration page's
**Controller** section.

**Remapping GL/GR from the controller.** Hold **C** and **GL** (or **GR**) and
press another button: GL (GR) now sends what that button sends, and the
controller ticks. Do the same again to clear it. While C + GL/GR are held,
nothing reaches the host. Turn it off on the configuration page if you use C
as a regular button. It is off in DualSense Edge mode, where GL, GR and C are
the paddles and Fn button that the host's software (e.g. Steam Input) remaps.

**Steam quick access menu on C.** On the configuration page, map C (or any
button) to *Home+A (Steam quick access)*. A tap then sends Home, then Home + A,
which opens Steam's quick access menu. It fires when the button is released
without another button pressed meanwhile, so C still works as the remap
modifier above. While the shortcut plays (about 0.16 s) the host sees only
those buttons: everything else released, sticks centered. Don't use it on a Switch, where Home + A launches the selected
game.

### USB modes (DualSense Edge, DualSense, Xbox 360)

Under **USB and system → Current mode** the dongle can present itself as:

| Mode | Use it for | GL / GR / C |
| --- | --- | --- |
| Switch Pro Controller (default) | Switch consoles, PCs | mapped to existing buttons |
| DualSense Edge | PCs; Steam Input gets the extra buttons | back paddles and Fn buttons |
| DualSense | PCs and games that expect a PlayStation pad | mapped to existing buttons |
| Xbox 360 controller | anything that only speaks XInput | mapped to existing buttons |

**Switching from the controller:** hold **C + Home** for 1.5 seconds. The
controller gives a "ba-thump" and all four of its lights blink. Then press the
button for the mode you want; the controller thumps and the dongle restarts in
that mode. Defaults:

| Button | Mode |
| --- | --- |
| A | DualSense Edge |
| B | Xbox 360 controller |
| X | DualSense |
| Y | Switch Pro Controller |
| D-pad | – (empty) |

**USB and system → Mode shortcut** assigns a mode, or – (empty), to each face
button and D-pad direction. Empty buttons are ignored. Press C + Home
again, or wait 5 seconds without pressing anything, to leave without a change.
While choosing, the host sees no buttons pressed. While C is held, Home doesn't
reach the host, so the hold doesn't open the host's home menu (setting every
button to – turns the shortcut and this off, except on the Pico 2 W while
the Wi-Fi hotkey is on). The **Haptics test** section plays these effects, and
the controller's built-in vibration samples.

Changing the mode restarts the dongle, which then has that controller's USB
IDs; the configuration page still finds it (Linux: install the current
`tools/99-switch2-pico.rules`, which covers every mode's IDs). Each mode keeps its own button map,
edited in **Button mapping** (pick the mode under *Mapping for*). The defaults
go by position (Nintendo A = Circle / Xbox B, B = Cross / Xbox A, and so on),
and the C + GL/GR + button shortcut works in every mode except DualSense Edge.

DualSense modes add these outputs to the map:

* **Touchpad click (left / center / right)**: a click plus a touch on that part
  of the pad, so Steam Input sees left and right touchpad clicks. Capture
  defaults to the center click.
* **Mic (mute)** button.
* **Left / right paddle** and **left / right Fn** (DualSense Edge only; the
  Edge defaults are GL → left paddle, GR → right paddle, C → right Fn).
* **PS + Cross (Steam quick access)**, the macro equivalent of Home+A (Xbox:
  Guide + A).

The battery level (DualSense percentage, Switch Pro full / medium / low /
critical) comes from the controller's battery voltage through a typical
lithium-cell curve, smoothed over several seconds so rumble doesn't make it
jump, and it only moves down while discharging (up while charging).

Rumble is the two-motor kind these controllers have, played on the Switch 2's
HD rumble actuators like the motors of a real pad: the strong, heavy motor in
the left grip and the weak, light one in the right, each on its own side, so
games that signal left vs right (Fez's L2/R2 hints, for example) can be told
apart. **Haptics test → Left motor / Right motor** plays each one. The
DualSense's audio-driven haptics aren't available (the emulated DualSense has
no audio interface); games fall back to this rumble. Gyro is reported in the DualSense modes (the Xbox 360
controller has none). Player LEDs follow the host. Switch consoles need the
Switch Pro Controller mode.

### On a Switch 1 console

Turn on **System Settings → Controllers and Sensors → Pro Controller Wired
Communication**, then plug the dongle into the dock or the console.

## Configuration page

### Over USB (WebUSB)

With the dongle plugged in, open **https://joshbirnholz.github.io/switch2-pico/**
in Chrome or Edge (Chrome also offers this link in a notification when the
dongle is plugged in), click **Connect over USB** and pick the dongle ("Pro
Controller", "DualSense Edge Wireless Controller", "DualSense Wireless
Controller" or "Controller", depending on its USB mode; a real controller with
those names isn't listed).
You can also open `web/index.html` from this repository directly (download
it and double-click it; WebUSB works from a local file too).

The hosted page, together with the latest firmware it offers as updates, is
published by the `pages` job in `.github/workflows/build.yml`. On a fork,
enable it once under **Settings → Pages → Source: GitHub Actions**.

* **Linux:** allow your user to open the device first:
  `sudo cp tools/99-switch2-pico.rules /etc/udev/rules.d/ && sudo udevadm control --reload-rules`,
  then replug the dongle.
* **Windows:** no driver needed; the dongle tells Windows to use WinUSB for
  the configuration interface.
* The WebUSB interface adds a second USB interface next to the controller. If
  a Switch 1 console doesn't accept the dongle, turn off *WebUSB configuration
  interface* (via Wi-Fi if needed) so it matches a genuine Pro Controller
  exactly.

### Over Wi-Fi

Turn the configuration Wi-Fi on in one of three ways:

* press the Pico's **BOOTSEL** button briefly
* hold **C + Home** on the controller for 3 seconds (it enters USB mode
  selection after 1.5 s; keep holding, and the Wi-Fi toggle leaves it again)
* plug in the dongle while no controller is paired (it starts automatically)

(Pico 2 W only.) Then join the Wi-Fi network **`Switch2-Pico-XXXX`** (default password
`switch2pico`) and open **http://192.168.4.1**. Most phones open the page by
themselves. The Wi-Fi turns itself off after 10 minutes without page activity,
or immediately with **Turn Wi-Fi off now**, so it doesn't compete with the
controller's Bluetooth during play.

On the page you can:

* see the live input, battery, report rate and connection details
* remap every button, including GL, GR and C
* set stick deadzones, gyro sensitivity and calibration, and rumble strength and frequency mode
* toggle USB behaviour: report rate, LED following, wake-on-controller, attach only while connected
* change the Wi-Fi name and password, reboot, enter firmware update mode, factory reset
* read the firmware log (useful for bug reports)

## Troubleshooting

**The controller connects briefly, then turns off.** After a failed attempt the
Pico LED blinks a code for 60 seconds:

| Blinks | Failed at |
| --- | --- |
| 1 | establishing the Bluetooth link |
| 2 | reading the controller's services (GATT) |
| 3 | first commands / Nintendo pairing |
| 4 | controller initialisation |

The full log (configuration page → Log → Download) shows the exact step and
the Bluetooth error code. Please include it in bug reports.

**The dongle stops responding** (LED frozen, the controller drops and the
configuration page can't find it). If the firmware ever hangs, a watchdog
restarts it after about 8 seconds.

**Saved log (nRF52840).** The log is also written to flash about once a
second (and straight away when the controller disconnects), so it survives a
restart, a crash and even unplugging the dongle. The configuration page's log
shows the saved part first, then a `===== saved log from before this start is
above =====` line, then the current start. `boot: last reset:` tells why the
dongle last started: `power on` means it lost power (unplugged, or the USB
port's power dropped), `restart by the firmware` an update, mode change or
reboot, `unexpected restart` a crash or hang. `usb: host connection lost`
says whether USB power was still there (the computer reset the port) or
went away (port, cable or hub).

**The dongle restarts by itself (nRF52840) and the log says `power on (power
was off or dropped)`.** The board really lost power for a moment: the USB
port, hub or cable couldn't keep up, or the board's regulator browned out.
**Controller → Dongle power** shows the USB and chip supply (with the lowest
seen since start), and the log records new lows (`power: … dropped to …`).
Try another port directly on the computer (not a hub) or another cable, and
don't charge the controller from the same hub. **USB and system → Bluetooth
transmit power** lowers the dongle's peak current; 0 dBm still reaches
across a room.

**The Wi-Fi network shows up but http://192.168.4.1 doesn't load.** Make sure
your phone or computer stays on the `Switch2-Pico` network even though it has
no internet (phones may switch back to mobile data; turn mobile data off
briefly). WebUSB is the easier option on a computer.

## What is verified

I wrote this from the public protocol research listed under
[Credits](#credits). On an nRF52840 dongle it has been confirmed to pair,
reconnect, and work with a Switch 1 console and with Steam on SteamOS.

| Part | Confidence |
| --- | --- |
| Builds for both boards; unit tests for the rumble codec, protocol parsing and mapping | ✅ done in CI |
| BLE connect, GATT layout, command framing, input report 0x05, stick calibration | ✅ confirmed on hardware (nRF52840) |
| Nintendo pairing (`0x15` commands) and reconnect via the host address in adverts | ✅ confirmed on hardware (nRF52840) |
| Switch 1 Pro Controller USB protocol (handshake, subcommands, SPI calibration) | ✅ confirmed with a Switch 1 and with Linux/Steam |
| IMU axis mapping and units | Medium: derived from SDL's Switch 1 and Switch 2 drivers. Use the gyro settings if an axis feels wrong. |
| HD rumble **amplitude** | Medium-high |
| HD rumble **frequency** translation | **Low.** The Switch 2 frequency encoding hasn't been published. It is anchored on the console's idle frame (verified bit-exact in tests) with a configurable scale, and a *Fixed* fallback mode is available. |

If something doesn't work, the log on the configuration page usually shows
where it stopped. Please include it in bug reports.

## Known limitations

* **No amiibo.** The Switch doesn't use a Pro Controller's NFC reader over a
  wired USB connection, so the dongle doesn't read tags. It answers the
  NFC/IR chip setup commands like an idle controller.
* One controller per dongle.
* Joy-Con 2 are not supported yet.
* The Switch 1 report carries three IMU samples per report. The controller
  provides one per BLE report, so the latest sample is repeated.
* The connection interval requested is 7.5 ms (the Bluetooth minimum). The
  console itself uses 5 ms, which standard controllers don't allow.

## Building

### Pico 2 W

```sh
git clone --depth 1 -b 2.2.0 https://github.com/raspberrypi/pico-sdk
git -C pico-sdk submodule update --init --depth 1
export PICO_SDK_PATH=$PWD/pico-sdk

cmake -S . -B build -DPICO_BOARD=pico2_w
cmake --build build          # -> build/switch2_pico.uf2

make -C test                 # host unit tests
```

Requires `arm-none-eabi-gcc` and CMake ≥ 3.13. The SDK fetches and builds
`picotool` automatically. Debug output goes to UART0 (GP0 TX, 115200 baud),
and the same log is available on the configuration page.

### nRF52840

```sh
arduino-cli config add board_manager.additional_urls \
    https://adafruit.github.io/arduino-board-index/package_adafruit_index.json
arduino-cli core update-index && arduino-cli core install adafruit:nrf52
nrf52/build.sh               # -> build-nrf/switch2_nrf52840.uf2
```

The sketch is built for the `adafruit:nrf52:feather52840` board definition,
which also fits Pro Micro nRF52840 / nice!nano boards. The status LED is
driven on both `LED_BUILTIN` and pin 24 (P0.15, the nice!nano LED). Pass
`-DS2P_LED_PIN_A=...` style defines to change it.

## Code map

`core/src/` holds everything that doesn't depend on the board. It is used
as plain C by the Pico CMake build and as an Arduino library by the nRF52840
sketch.

| File | Purpose |
| --- | --- |
| `core/src/s2_link.c` | Switch 2 controller logic: which adverts to connect to, init and Nintendo pairing commands, input, rumble pacing, LEDs, gyro calibration |
| `core/src/s2_transport.h` | Interface each board's Bluetooth stack implements |
| `core/src/s2_proto.c` | Switch 2 protocol: adverts, command framing, report and calibration parsing |
| `core/src/procon.c` | Emulated Switch 1 Pro Controller: USB handshake, subcommands, SPI flash, input reports |
| `core/src/usb_hid.c`, `usb_pro_desc.c` | TinyUSB HID class driver and the Pro Controller report descriptor |
| `core/src/hd_rumble.c` | Switch 1 HD rumble decoder and Switch 2 encoder |
| `core/src/mapping.c` | Button remapping, stick calibration and deadzones, IMU conversion |
| `core/src/mode_select.c` | USB mode shortcut on the controller (C + Home, then a button) |
| `core/src/web_api.c`, `webusb.c` | Configuration API, served over HTTP or WebUSB |
| `core/src/app_core.c`, `settings.c` | Glue, USB suspend / wakeup, settings |
| `core/src/platform.h` | Board services (time, storage, reboot, firmware update) |
| `pico/fw_update_pico.c`, `nrf52/switch2_nrf/fw_update_nrf.cpp` | Firmware update without the UF2 drive: staging, check, install at boot |
| `pico/` | Pico: BTstack transport, USB descriptors, Wi-Fi page server, LED, BOOTSEL |
| `nrf52/switch2_nrf/` | nRF52840: Bluefruit transport, USB setup, LittleFS storage, LED |
| `web/index.html` | The configuration page (WebUSB or HTTP) |

## Credits

Protocol knowledge comes from these projects. No code was copied from them.

* [ndeadly/switch2_controller_research](https://github.com/ndeadly/switch2_controller_research): Switch 2 GATT table, commands, pairing, reports
* [trevlars/switch2-controllers-linux](https://github.com/trevlars/switch2-controllers-linux): working Linux BLE bridge
* [libsdl-org/SDL](https://github.com/libsdl-org/SDL): Switch 2 USB driver (calibration, IMU, rumble frame layout) and the Switch 1 driver
* [TommyWabg/Switch2Connect](https://github.com/TommyWabg/switch2-controllers-windows10-gyro): IMU scale measurements, rumble captures
* [safijari/openpuck](https://github.com/safijari/openpuck) and [SundayMoments/DS5_Bridge](https://github.com/SundayMoments/DS5_Bridge): the dongle concept and Switch Pro output mode
* [dekuNukem/Nintendo_Switch_Reverse_Engineering](https://github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering), CTCaer's jc_toolkit, [mart1nro/joycontrol](https://github.com/mart1nro/joycontrol) (Poohl's fork): Switch 1 protocol, HD rumble

## License

MIT
