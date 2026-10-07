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
| Reconnect / wake | After pairing, pressing any button reconnects. While the PC sleeps, the controller is let go so it can sleep too; a button press can wake the PC (USB remote wakeup). |
| Buttons and sticks | Uses the controller's own calibration, with a configurable radial deadzone. |
| GL / GR / C | Each can be mapped to any Pro Controller button (all other buttons can be remapped too). Defaults: GL → left stick click, GR → right stick click, C → unassigned. |
| Gyro and accelerometer | Converted to the Switch 1 axis layout and units. The gyro scale is auto-detected, can be calibrated, and sensitivity is adjustable. |
| HD rumble | The host's Switch 1 HD rumble frames, including the packed multi-sample formats, are decoded and re-encoded for the Switch 2 actuators, left and right separately. |
| Player LEDs | Follow the player number the host assigns. |
| Battery | Reported to the host. |
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

Get the `.uf2` for your board from the GitHub Actions artifacts (or
[build it](#building)), then:

* **Pico 2 W:** hold **BOOTSEL** while plugging it in; a drive named `RP2350`
  appears. Copy `switch2_pico.uf2` onto it.
* **nRF52840:** double-tap reset (on a Pro Micro without a reset button,
  short RST to GND twice quickly); a drive such as `NICENANO` or
  `FTHR840BOOT` appears. Copy `switch2_nrf52840.uf2` onto it.

The board reboots into the firmware. To update later, use **Firmware update
mode** on the configuration page, which reboots into the same drive.

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

### On a Switch 1 console

Turn on **System Settings → Controllers and Sensors → Pro Controller Wired
Communication**, then plug the dongle into the dock or the console.

## Configuration page

### Over USB (WebUSB)

With the dongle plugged in, open **https://joshbirnholz.github.io/switch2-pico/**
in Chrome or Edge (Chrome also offers this link in a notification when the
dongle is plugged in), click **Connect over USB** and pick "Pro Controller".
You can also open `web/index.html` from this repository directly (download
it and double-click it; WebUSB works from a local file too).

The hosted page is published by `.github/workflows/pages.yml`. On a fork,
enable it once under **Settings → Pages → Source: GitHub Actions** and run the
*Pages* workflow (GitHub Pages for a private repository needs a paid plan).

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
* hold **C + Home** on the controller for 3 seconds
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
| `core/src/web_api.c`, `webusb.c` | Configuration API, served over HTTP or WebUSB |
| `core/src/app_core.c`, `settings.c` | Glue, USB suspend / wakeup, settings |
| `core/src/platform.h` | Board services (time, storage, reboot) |
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
