# Switch2-Pico

Firmware that turns a **Raspberry Pi Pico 2 W** into a wireless USB dongle for
the **Nintendo Switch 2 Pro Controller**. The controller pairs to the Pico over
Bluetooth LE, and the Pico shows up over USB as a wired
**Nintendo Switch (1) Pro Controller**. That means it works anywhere a Pro
Controller works: Steam (Windows, macOS, Linux, Steam Deck), SDL games,
emulators, Chrome's Gamepad API, Linux's `hid-nintendo` driver, and a
Switch 1 console.

```
Switch 2 Pro Controller  ──BLE──▶  Pico 2 W  ──USB──▶  PC / Mac / Switch 1
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
| Amiibo (NFC) | Reads NTAG215 tags through the controller and presents them through the emulated Pro Controller's NFC chip. Game writes to amiibo go to a cached copy only (see limitations). The last tag can be downloaded as a `.bin` dump from the config page. |
| Player LEDs | Follow the player number the host assigns. |
| Battery | Reported to the host. |
| NSO GameCube controller | Basic support: buttons, sticks, analog triggers acting as L/R past a threshold, and rumble through built-in vibration presets. |
| Configuration page | Served by the Pico itself over Wi-Fi, so it works from any phone or computer with no drivers. |

## Hardware

* Raspberry Pi Pico 2 W (RP2350 + CYW43439). A Pico W (RP2040) build also
  works: configure with `-DPICO_BOARD=pico_w`.
* A USB cable from the Pico to the host.

## Installing

1. Get `switch2_pico.uf2`, either from the GitHub Actions artifacts or by
   [building it](#building).
2. Hold **BOOTSEL** on the Pico while plugging it in. A drive named `RP2350` appears.
3. Copy the `.uf2` file onto that drive. The Pico reboots into the firmware.

To update later, use **Firmware update mode** on the configuration page, or
repeat step 2.

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

### On a PC

Steam, SDL and the Linux kernel driver all treat the dongle as a wired Pro
Controller, with gyro and rumble. In Steam, enable *Switch Controller Support*
in Settings → Controller.

### On a Switch 1 console

Turn on **System Settings → Controllers and Sensors → Pro Controller Wired
Communication**, then plug the dongle into the dock or the console.

## Configuration page

Turn the configuration Wi-Fi on in one of three ways:

* press the Pico's **BOOTSEL** button briefly
* hold **C + Home** on the controller for 3 seconds
* plug in the dongle while no controller is paired (it starts automatically)

Then join the Wi-Fi network **`Switch2-Pico-XXXX`** (default password
`switch2pico`) and open **http://192.168.4.1**. Most phones open the page by
themselves. The Wi-Fi turns itself off after 10 minutes without page activity,
or immediately with **Turn Wi-Fi off now**, so it doesn't compete with the
controller's Bluetooth during play.

On the page you can:

* see the live input, battery, report rate and connection details
* remap every button, including GL, GR and C
* set stick deadzones, gyro sensitivity and calibration, and rumble strength and frequency mode
* scan amiibo and download the last tag
* toggle USB behaviour: report rate, LED following, wake-on-controller, attach only while connected
* change the Wi-Fi name and password, reboot, enter firmware update mode, factory reset
* read the firmware log (useful for bug reports)

## What is verified

I wrote this from the public protocol research listed under
[Credits](#credits). I couldn't test it with real hardware.

| Part | Confidence |
| --- | --- |
| Builds for the Pico 2 W; unit tests for the rumble codec, protocol parsing, mapping and NFC MCU emulation | ✅ done in CI |
| BLE connect, GATT layout, command framing, input report 0x05, stick calibration | High: matches two independent working implementations (trevlars' Linux bridge, SDL) |
| Nintendo pairing (`0x15` commands) and reconnect via the host address in adverts | High: documented with captures by ndeadly |
| Switch 1 Pro Controller USB protocol (handshake, subcommands, SPI calibration) | High: well documented and implemented by many projects |
| IMU axis mapping and units | Medium: derived from SDL's Switch 1 and Switch 2 drivers. Use the gyro settings if an axis feels wrong. |
| HD rumble **amplitude** | Medium-high |
| HD rumble **frequency** translation | **Low.** The Switch 2 frequency encoding hasn't been published. It is anchored on the console's idle frame (verified bit-exact in tests) with a configurable scale, and a *Fixed* fallback mode is available. |
| Amiibo reading over the Switch 2 controller | **Low–medium.** The NFC command set is only partly documented. The reader searches its buffer for the tag's UID so the exact header layout doesn't matter, and every step is logged. |
| Amiibo through USB to games and emulators | Medium. Uses the documented Switch 1 MCU protocol. Over USB it needs the extra `0x31` report in the HID descriptor (on by default; a genuine Pro Controller lacks it). If a console misbehaves, turn off *Declare NFC report on USB*. |

If something doesn't work, the log on the configuration page usually shows
where it stopped. Please include it in bug reports.

## Known limitations

* **Amiibo writes are not written to the physical tag.** Games that update an
  amiibo (Smash, Zelda…) succeed, but only the dongle's cached copy changes.
  You can download that copy from the configuration page. The Switch 2 write
  command format isn't known well enough to risk corrupting real tags.
* One controller per dongle.
* Joy-Con 2 are not supported yet.
* The Switch 1 report carries three IMU samples per report. The controller
  provides one per BLE report, so the latest sample is repeated.
* The connection interval requested is 7.5 ms (the Bluetooth minimum). The
  console itself uses 5 ms, which standard controllers don't allow.

## Building

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

## Code map

| File | Purpose |
| --- | --- |
| `src/main.c` | Main loop; connects the modules; hotkeys, BOOTSEL, USB suspend handling |
| `src/s2_link.c` | BTstack BLE central: scan, connect, GATT discovery, init and pairing commands, input, rumble, LEDs, NFC reading |
| `src/s2_proto.c` | Switch 2 protocol: adverts, command framing, report and calibration parsing |
| `src/procon.c` | Emulated Switch 1 Pro Controller: USB handshake, subcommands, SPI flash, input reports |
| `src/usb_hid.c`, `src/usb_descriptors.c` | Custom TinyUSB HID class driver and the genuine Pro Controller descriptors |
| `src/hd_rumble.c` | Switch 1 HD rumble decoder and Switch 2 encoder |
| `src/mapping.c` | Button remapping, stick calibration and deadzones, IMU conversion |
| `src/mcu_nfc.c`, `src/amiibo.c` | Switch 1 NFC/MCU emulation and the shared tag store |
| `src/settings.c` | Settings persisted in flash |
| `src/web/*`, `web/index.html` | Wi-Fi access point, DHCP and DNS, HTTP server, JSON API, page |

## Credits

Protocol knowledge comes from these projects. No code was copied from them.

* [ndeadly/switch2_controller_research](https://github.com/ndeadly/switch2_controller_research): Switch 2 GATT table, commands, pairing, reports
* [trevlars/switch2-controllers-linux](https://github.com/trevlars/switch2-controllers-linux): working Linux BLE bridge
* [libsdl-org/SDL](https://github.com/libsdl-org/SDL): Switch 2 USB driver (calibration, IMU, rumble frame layout) and the Switch 1 driver
* [TommyWabg/Switch2Connect](https://github.com/TommyWabg/switch2-controllers-windows10-gyro): IMU scale measurements, rumble captures
* [safijari/openpuck](https://github.com/safijari/openpuck) and [SundayMoments/DS5_Bridge](https://github.com/SundayMoments/DS5_Bridge): the dongle concept and Switch Pro output mode
* [dekuNukem/Nintendo_Switch_Reverse_Engineering](https://github.com/dekuNukem/Nintendo_Switch_Reverse_Engineering), CTCaer's jc_toolkit, [mart1nro/joycontrol](https://github.com/mart1nro/joycontrol) (Poohl's fork): Switch 1 protocol, HD rumble, NFC MCU

## License

MIT
