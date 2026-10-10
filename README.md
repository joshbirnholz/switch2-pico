# Switch2-Pico

Firmware that turns a **Raspberry Pi Pico 2 W** or an **nRF52840 board**
(Pro Micro nRF52840 / nice!nano, Adafruit Feather nRF52840, ...) into a
wireless USB dongle for the **Nintendo Switch 2 Pro Controller** (and the
Nintendo GameCube Controller and **Joy-Con 2**, alone or as a pair). The
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
| Pairing | Hold **Sync** on the controller. Uses Nintendo's own key-exchange commands; standard Bluetooth pairing would make the controller disconnect. The dongle remembers up to 8 controllers. |
| Reconnect / wake | After pairing, pressing any button reconnects. While the PC sleeps, the controller is let go so it can sleep too; a button press can wake the PC (USB remote wakeup; on Linux see [Waking the PC](#on-a-pc)). |
| Buttons and sticks | Uses the controller's own calibration, with a configurable radial deadzone. |
| GL / GR / C | Each can be mapped to any Pro Controller button (all other buttons can be remapped too), on the configuration page or from the controller (hold C + GL/GR and press a button). Defaults: GL → left stick click, GR → right stick click, C → unassigned. |
| Gyro and accelerometer | Converted to the Switch 1 axis layout and units. The gyro scale is auto-detected, can be calibrated, and sensitivity is adjustable. |
| HD rumble | The host's Switch 1 HD rumble frames, including the packed multi-sample formats, are decoded and re-encoded for the Switch 2 actuators, left and right separately. |
| Player LEDs | Follow the player number the host assigns. |
| Battery | Reported to the host. |
| USB modes | **Switch Pro Controller** (default; Switch consoles and PCs), **DualSense Edge** (GL, GR and C become its back paddles and Fn button, so Steam Input can map them), **DualSense**, **Xbox 360 controller**, **GameCube adapter**, or **SInput** (GL, GR, C and Capture as buttons of their own in Steam Input, with the Switch 2 Pro Controller's symbols, plus gyro and rumble). Each mode has its own button map. |
| Nintendo GameCube Controller | Basic support: buttons, sticks, analog triggers acting as L/R past a threshold, and rumble on its motor (strength by switching it on and off every 12 ms, as SDL does). |
| Joy-Con 2 | A **Joy-Con 2 (L)** and a **Joy-Con 2 (R)** connected together are one controller, **Joy-Con 2 (L/R)**. A single one is held sideways (SL / SR are its shoulders). Each of the three has its own profiles. **Untested on hardware** (see [Joy-Con 2](#joy-con-2)). |
| Mouse | Mouse Mode: a Joy-Con 2's optical sensor as a USB mouse next to the controller (a profile option, Xbox 360 and SInput modes). |
| Configuration page | Over **WebUSB** in Chrome/Edge with the dongle plugged in, or (Pico 2 W only) over the dongle's own Wi-Fi from any phone or computer. |

## Hardware

Two boards are supported; they share all of the controller logic.

| | Raspberry Pi Pico 2 W | nRF52840 (Pro Micro / nice!nano, Feather, ...) |
| --- | --- | --- |
| Bluetooth | CYW43439 (BTstack), shares its radio with Wi-Fi | Nordic SoftDevice S140, Bluetooth only |
| Configuration | WebUSB or Wi-Fi page | WebUSB |
| Buttons on the board | BOOTSEL: tap = Sync (pairing window), hold 1–5 s = Wi-Fi page, hold 5 s = forget all controllers | none (use the configuration page) |
| Firmware file | `switch2_pico.uf2` | `switch2_nrf52840.uf2` |

A Pico W (RP2040) build also works: configure with `-DPICO_BOARD=pico_w`.
The nRF52840 build needs a board with the Adafruit nRF52 UF2 bootloader and
S140 v6.1.1 SoftDevice. That is the bootloader Pro Micro nRF52840 and
nice!nano boards ship with, the same as for openpuck.

## Installing

Download the `.uf2` for your board from the
[**Latest build** release](https://github.com/joshbirnholz/switch2-pico/releases/tag/latest)
(rebuilt on every push; each CI run also keeps it as an artifact), or
[build it](#building). Both boards' files are there; the configuration page's **Update**
offers the one for the connected board. Then:

* **Pico 2 W:** hold **BOOTSEL** while plugging it in; a drive named `RP2350`
  appears. Copy the `.uf2` file onto it.
* **nRF52840:** double-tap reset (on a Pro Micro without a reset button,
  short RST to GND twice quickly); a drive such as `NICENANO` or
  `FTHR840BOOT` appears. Copy the `…-nrf52840.uf2` file onto it.

The board reboots into the firmware.

**Updating:** the configuration page's **Tools → Firmware** section shows the
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

1. Plug in the dongle. Its LED blinks because no controller is paired yet.
   **Pico:** also press **BOOTSEL** once (the dongle's own Sync button): the
   LED blinks faster while it accepts a new controller (60 s). The
   configuration page's **Pair a controller** does the same.
2. Hold the small **Sync** button on the controller until the player lights sweep.
3. The dongle connects, pairs, and the controller gives a short "click"
   rumble. The LED stays on.
4. Next time, just press any button on the controller.

**Several controllers.** The dongle remembers up to 8 paired controllers;
any of them can connect by pressing a button, one at a time (while one is
connected, the others wait). To add one, hold Sync on it (on the Pico, press
BOOTSEL first). Pairing a ninth forgets the oldest pairing; pairing a known
controller again makes it the newest. The configuration page lists the paired
controllers (**Device → Paired controllers**) with a **Forget** button for
each and **Forget all**; holding the Pico's BOOTSEL button for 5 seconds also
forgets all. A forgotten controller no longer reconnects by itself (it still
remembers the dongle, but the dongle ignores it); hold Sync on it to pair it
again.

### Joy-Con 2

Pair each Joy-Con 2 like any controller (hold its Sync button), one after the
other: the pairing window closes once the first is paired, and the first
stays connected while the window is opened again for the second (on the Pico, a Joy-Con 2 kept this way is only let go if another
kind of controller shows up in pairing mode). Only one
controller connects at a time, with one exception: a **Joy-Con 2 (L)** and a
**Joy-Con 2 (R)** connect together. Once one Joy-Con 2 is connected, the
dongle keeps looking for the other side and connects it when it wakes up;
nothing else connects meanwhile (not a second (L) or (R), not a Pro or
GameCube Controller).

* **Both connected: Joy-Con 2 (L/R).** One controller with the (L)'s left
  stick and buttons and the (R)'s; motion comes from the (R). The battery
  shown is the lower one. Rumble goes to each side's own motor. Each
  Joy-Con's SL and SR are inputs of their own (four in all), mapped
  separately.
* **One on its own.** By default it is its half of the Joy-Con 2 (L/R)
  (the other half's buttons and stick just aren't there until it connects).
  With **Device → Paired controllers → Allow a single Joy-Con 2 on its own**
  on, it is its own controller instead, **Joy-Con 2 (L)** or **(R)**, held
  sideways with the rail on top, with its own profiles (their tabs show only
  then): its stick is the left stick, turned to match; the four buttons
  under the thumb act as A / B / X / Y by where they end up (an (L): Down =
  A, Left = B, Right = X, Up = Y; an (R): X = A, A = B, Y = X, B = Y); SL /
  SR are L / R. Motion isn't turned (the axes are as the Joy-Con reports
  them). Then, when a Joy-Con 2 connects and the dongle last ran a pair whose
  other half is paired too, it starts as the pair and waits for the other
  side; if that doesn't connect within 30 seconds, the one that did is used
  on its own. A pair stays a pair if one side drops; the dongle reconnects
  it when it wakes up.
* **SL / SR** of a pair can be remapped from the controller like the Pro
  Controller's GL / GR (**Remap SL/SR from the controller** on its Buttons
  tab): hold C and SL or SR of either Joy-Con, then press another button;
  only the SL or SR held changes.
* Profiles: a pair's are on buttons and switch with **C + Home**, like the
  Pro Controller's. A single Joy-Con 2 has no shortcut: its profiles are
  numbered 1–8, and the one in use is chosen on the configuration page.

**Mouse Mode.** On the Joy-Con 2 tabs, a profile's **Mouse** tab has one
switch, Mouse Mode, and the pointer speed. With it on, each Joy-Con 2 laid on
its side on a surface is its own USB mouse, as on a Switch 2 (the dongle adds
two mice, the (L)'s and the (R)'s; the sensor reports how far the surface
is). As on a Switch 2, a Joy-Con's sensor only runs while it is tilted
within about 55 degrees of lying on its side (it costs battery), so it
becomes a mouse a moment after it is put down. Its shoulder button clicks, its trigger right-clicks, its stick click
middle-clicks and its stick scrolls: up / down scroll up / down, and left /
right scroll sideways (with **Scroll sideways with left / right** off, left
scrolls up and right down, as on a Switch 2; **Invert up / down scrolling**
and **Invert left / right scrolling** turn either the other way); picked up,
they are the controller's again. In Xbox 360 and SInput profiles of the
Joy-Con 2 types the dongle always has the two USB mice, so turning Mouse
Mode on or off (and every other option and mapping) takes effect right
away; only switching to a profile with another emulated controller restarts
the dongle.

**Xbox 360 and SInput modes only.** Mouse Mode works only in profiles that
emulate an Xbox 360 or an SInput controller (no kernel driver claims either
device as a whole); elsewhere the setting is kept but does nothing (and the
Joy-Con's buttons all stay the controller's). In the Switch Pro and
DualSense modes, Linux's driver for that controller (`hid-nintendo`,
`hid-playstation`) claims every interface of the device, the mouse's too,
fails on it, and the mouse gets no driver; the dongle can't avoid that, as
the USB ID belongs to the whole device.

**Pairing only on request (Pico, on by default).** The Pico ignores
controllers in pairing mode unless its pairing window is open: a short
BOOTSEL press or **Controller → Pair a controller** opens it for 60 seconds
or until a controller is paired (press again to close it); a connected
controller is let go meanwhile. So
pairing your controller with a Switch 2 or a PC nearby doesn't get it grabbed
by the dongle. Turn it off under **Controller** to pair any time (the nRF52840
has no button and always works that way).

| Pico LED | Meaning |
| --- | --- |
| very fast blink (5 Hz) | pairing window open: hold Sync on the controller |
| fast blink | no controller paired yet |
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
**Device → Power and radio**.

**Remapping GL/GR from the controller.** Hold **C** and **GL** (or **GR**) and
press another button: GL (GR) now sends what that button sends, and the
controller ticks. Do the same again to clear it. While C + GL/GR are held,
nothing reaches the host. Turn it off on the configuration page if you use C
as a regular button. It is off in DualSense Edge and SInput modes, where GL,
GR and C are the paddles and extra buttons that the host's software (e.g.
Steam Input) remaps.

**Steam quick access menu on C.** On the configuration page, map C (or any
button) to *Home+A (Steam quick access)*. A tap then sends Home, then Home + A,
which opens Steam's quick access menu. It fires when the button is released
without another button pressed meanwhile, so C still works as the remap
modifier above. While the shortcut plays (about 0.16 s) the host sees only
those buttons: everything else released, sticks centered. Don't use it on a Switch, where Home + A launches the selected
game.

**Steam screenshot on Capture.** Likewise, *Home+R (Steam screenshot)* sends
Home, then Home + R, Steam's screenshot shortcut (Guide + RB / PS + R1 in the
other modes). Map Capture (or any button) to it.

### USB modes (DualSense Edge, DualSense, Xbox 360, GameCube adapter, SInput)

Under **Mode** (on each controller tab) the dongle can present itself as:

| Mode | Use it for | GL / GR / C |
| --- | --- | --- |
| Switch Pro Controller (default) | Switch consoles, PCs | mapped to existing buttons |
| DualSense Edge | PCs; Steam Input gets the extra buttons | back paddles and Fn buttons |
| DualSense | PCs and games that expect a PlayStation pad | mapped to existing buttons |
| Xbox 360 controller | anything that only speaks XInput | mapped to existing buttons |
| GameCube adapter | Switch and Wii U games that take GameCube controllers (Smash), Dolphin / Slippi, Steam | mapped to existing buttons |
| SInput | PCs (Steam, SDL games); Steam Input gets every extra button, with Nintendo symbols | GL, GR and C (with their symbols), plus SL (L) / SR (R) |

**Xbox 360 mode** presents an XInput controller ("Switch2-Pico Xbox 360
Controller", 1209:0001, the pid.codes test ID) rather than Microsoft's own
ID (045E:028E, before 0.16.0): Windows' Xbox 360 driver takes a device with
Microsoft's ID as a whole, so the configuration page got "access denied"
and Joy-Con 2 Mouse Mode had no mice. With this ID the dongle's Microsoft
OS 2.0 descriptors give that driver only the controller (compatible ID
XUSB10) and WinUSB the configuration interface; the mice get Windows' mouse
driver. XInput games see an Xbox 360 controller as before. Linux's xpad
driver knows the pid.codes vendor ID. Software that recognises an Xbox 360
controller by Microsoft's USB ID won't, and Steam may show it as a generic
XInput controller. **Untested on Windows.**

**SInput mode** presents the dongle as an SInput controller ("Switch2-Pico
SInput", 2E8A:10C6), Hand Held Legend's open USB protocol that SDL (and so
Steam) reads directly: the dongle tells the host which buttons, sticks,
triggers and sensors it has, so the four back paddles, Capture and C come
through as buttons of their own, with Nintendo face labels (B bottom, A
right), gyro, accelerometer, rumble and player number. Steam shows the
Switch 2 Pro Controller's GL, GR and C symbols for three of them, so a
Nintendo Switch 2 Pro Controller is mapped one to one. The other two paddles
(Steam's L4 / R4), named **SL (L)** and **SR (R)** on the page and in the
plugin after the Joy-Con 2 buttons they get by default, work but show no
symbol in Steam; SDL accepts the GL / GR pair only together with them.

The dongle lists C for Steam only when a button of the profile in use
sends it: with C mapped to the Quick Access Menu (Decky), for example, Steam
doesn't list a C button it would never see. (The other buttons are always
listed: Steam labels them by their place in the list, so leaving one out
would mislabel the ones after it.) SDL reads the list only when the
controller appears on USB, so when that changes, the dongle will leave USB
and come back half a second later (the Bluetooth controller stays
connected); Steam may then treat it as a controller with a different
layout, with its own Steam Input configuration. Only the controller's own
buttons count, not mappings stored for buttons it doesn't have. Defaults:

| Button | SInput |
| --- | --- |
| GL / GR (Pro) | GL / GR |
| SL / SR of the (L) | SL (L) / GL |
| SR / SL of the (R) | SR (R) / GR |
| Capture | Capture |
| Home | Home |
| C | C |

(Held upright, a Joy-Con 2 (L)'s SL is above its SR and a Joy-Con 2 (R)'s SR
above its SL.) There are no Steam shortcut outputs in this mode (Quick
Access Menu (Decky) and Capture do those jobs). Mouse Mode works in this
mode too. On Linux, Steam needs to read the device through hidraw: the
current `tools/99-switch2-pico.rules` allows it (the Decky plugin installs
such a rule on SteamOS). It needs an SDL / Steam recent enough to know
SInput.

**GameCube adapter mode** presents the dongle as Nintendo's Wii U / Switch
GameCube controller adapter (WUP-028) with the controller in port 1 (ports
2–4 empty). Made for the Nintendo GameCube Controller: its analog triggers come
through as analog L / R, Z (its ZR) as Z, Start (its Plus) as Start, the
C-stick as the C-stick. Sticks are scaled to a real GameCube stick's range
(128 ± 100). Other controllers work too: by default L / R give a full L / R
press and ZR is Z; remap under **Button mapping**. Rumble is on / off, as on
the real adapter. Notes:

* On a Switch, use it where a GameCube adapter works (e.g. Smash Bros.
  Ultimate). The dongle also has its configuration (WebUSB) interface, which
  a real adapter lacks.
* Dolphin / Slippi: Linux needs the current udev rule (above); Windows needs
  the WinUSB driver on the adapter, as with a real one (Zadig, "WUP-028",
  interface 0).
* Reports go out at the *Report interval* (default 8 ms = 125 Hz, like the
  real adapter; 4 ms for 250 Hz).

**Profiles.** A profile is a USB mode together with its button mapping, stick
settings (inner deadzone, full deflection, swap), trigger threshold and rumble
(on/off, strength). Each profile sits on a button: a face button (A, B, X, Y)
or a D-pad direction, so each controller type has up to 8. The Nintendo
Switch 2 Pro Controller and the Nintendo GameCube Controller have their own
profiles. One profile per controller type is **in use**, and it applies
whenever that controller connects. Several profiles can share a mode, for
example two Xbox 360 Controller profiles with different mappings.

The configuration page's **Controller** section has a tab per controller type
(Nintendo Switch 2 Pro Controller, Nintendo GameCube Controller, Joy-Con 2
(L/R), Joy-Con 2 (L), Joy-Con 2 (R)). It opens on the connected controller's
tab, marked with a green dot. Each tab shows its profiles as two diamonds of
tiles, the D-pad and the face buttons (laid out as on that controller), each
with the button's icon, the profile's name and its emulated controller. A
green check marks the profile in use. Tiles keep their size and place: a long
name is cut off with "…". Below the diamonds are the chosen button's
**Name**, **Emulated Controller** (its USB mode, or **None**), **Use this
profile**, **Duplicate…** and **Delete**. Then come its **Buttons**,
**Sticks & triggers**, **Rumble** and (Joy-Con 2 tabs) **Mouse** tabs, and
**Motion**, which is shared by every profile of every controller.

* Pick an emulated controller on an empty button to make a profile there.
* **None** or **Delete** empties the button. If that was the profile in use,
  another one takes over (Y, A, X, B, then the D-pad), and the page says which.
* **Duplicate…** asks for the button to copy to; a button that already has a
  profile asks before it is overwritten.
* Drag a profile onto another button to swap the two (or move it to an empty
  one).
* Changing a profile's emulated controller starts its mapping over from that
  mode's defaults.
* The GL / GR remap option shows only where it applies (not in DualSense Edge
  or SInput mode, whose back paddles are remapped by the host).

Edits stay unsaved until **Save** in the header; the header says when there
are unsaved changes.

New dongles start with one profile per mode, named after it, holding that
mode's mapping, on the buttons below. In use: Switch Pro for the Nintendo
Switch 2 Pro Controller, GameCube adapter for the Nintendo GameCube
Controller. Settings from earlier versions keep their profiles: each moves to
the button that selected it in the old Quick switch (one selected by two
buttons is copied to both), and the rest go to free buttons. Defaults differ
where it helps: a GameCube controller's analog L / R are the triggers (LT /
RT, L2 / R2) and Z the right bumper in the gamepad modes; a Pro Controller in
GameCube adapter mode uses ZL / ZR as L / R and R as Z.

**Mode per controller.** The dongle starts in the mode of the profile in use
for the controller that connected last. When a controller of the other type
connects and its profile's mode differs, the dongle restarts in that mode
first and the controller connects right after. A controller being paired is
paired first; then the dongle switches and the controller reconnects.

**Switching from the controller:** hold **C + Home** until you hear a click
(1.5 seconds; a "ba-thump", and all four of the controller's lights blink).
Then press the button of the profile you want; the controller thumps. A
profile in the same USB mode applies at once; one in another mode restarts the
dongle in that mode. Default profiles:

| Button | Profile |
| --- | --- |
| A | DualSense Edge (DualSense Edge Wireless Controller) |
| B | Xbox 360 (Xbox 360 Controller) |
| X | DualSense (DualSense Wireless Controller) |
| Y | Switch Pro (Nintendo Switch Pro Controller) |
| D-pad up | GameCube adapter (Nintendo GameCube Controller Adapter) |
| D-pad down | SInput (SInput Controller; new dongles) |
| D-pad left / right | empty |

Empty buttons are ignored. Press C + Home again, or wait 5 seconds without
pressing anything, to leave without a change. While choosing, the host sees no
buttons pressed. While C is held, Home doesn't reach the host, so the hold
doesn't open the host's home menu. The **Tools → Haptics test** plays these
effects, and the controller's built-in vibration samples.

A change of mode restarts the dongle, which then has that controller's USB
IDs; the configuration page still finds it (Linux: install the current
`tools/99-switch2-pico.rules`, which covers every mode's IDs). The default
mappings go by position (Nintendo A = Circle / Xbox B, B = Cross / Xbox A, and
so on). The C + GL/GR + button shortcut changes the profile in use and works
in every mode except DualSense Edge and SInput.

DualSense modes add these outputs to the map:

* **Touchpad click (left / center / right)**: a click plus a touch on that part
  of the pad, so Steam Input sees left and right touchpad clicks. Capture
  defaults to the center click.
* **Mic (mute)** button.
* **Left / right paddle** and **left / right Fn** (DualSense Edge only; the
  Edge defaults are GL → left paddle, GR → right paddle, C → right Fn).
* **PS + Cross (Steam quick access)**, the macro equivalent of Home+A (Xbox:
  Guide + A), and **PS + R1 (Steam screenshot)** (Xbox: Guide + RB).

The battery level (DualSense percentage, Switch Pro full / medium / low /
critical) is the controller's own level (0–9, what the console shows), with
its external-power and charging flags. The input report the dongle uses only
carries the battery voltage, so right after connecting and then once a minute
the dongle briefly subscribes to the controller-specific report (0x09 Pro,
0x0A GameCube), takes one and unsubscribes. If that isn't available, or input
stops while it is read, the dongle stays with an estimate from the voltage
(highest reading per 4 s, smoothed, settling over the first minute, then at
most 1 % per 20 s). The log shows `s2: controller battery level` when the
level changes and a `battery:` line once a minute.

Rumble is the two-motor kind these controllers have, played on the Switch 2's
HD rumble actuators like the motors of a real pad: the strong, heavy motor in
the left grip and the weak, light one in the right, each on its own side, so
games that signal left vs right (Fez's L2/R2 hints, for example) can be told
apart. **Tools → Haptics test → Left motor / Right motor** plays each one. The
DualSense's audio-driven haptics aren't available (the emulated DualSense has
no audio interface); games fall back to this rumble. Gyro is reported in the DualSense modes (the Xbox 360
controller has none). Player LEDs follow the host. Switch consoles need the
Switch Pro Controller mode.

### On a Switch 1 console

Turn on **System Settings → Controllers and Sensors → Pro Controller Wired
Communication**, then plug the dongle into the dock or the console.

## Configuration page

Until it finds a dongle, the page shows only how to connect. Then it has three sections: **Controller** (profiles, button mapping,
sticks, rumble, motion), **Device** (paired controllers, power
and radio, USB, Wi-Fi on the Pico 2 W, connection details) and **Tools**
(firmware, restart and reset, haptics test, log). The header always shows the
controller, its battery, the profile the dongle is running (its USB mode on
hover), and **Save**.

### Over USB (WebUSB)

With the dongle plugged in, open **https://joshbirnholz.github.io/switch2-pico/**
in Chrome or Edge (Chrome also offers this link in a notification when the
dongle is plugged in), click **Connect over USB** and pick the dongle ("Pro
Controller", "DualSense Edge Wireless Controller", "DualSense Wireless
Controller", "Switch2-Pico Xbox 360 Controller", "Switch2-Pico SInput" or
"Switch2-Pico (no controller)", depending on its USB mode; a real controller with those names isn't listed).
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
* The WebUSB interface is a second USB interface next to the controller, and
  is always on.
* While no controller is connected, the dongle shows up as a plain USB
  device, "Switch2-Pico (no controller)" (1209:0001, pid.codes' test ID),
  with only the configuration interface: games don't see a controller, but
  this page and the Decky plugin can still configure it. When a controller
  connects, the dongle leaves USB and comes back as the emulated controller;
  5 seconds after it disconnects, the other way round. While the PC sleeps
  it stays the controller, so a button press can still wake the PC.
  (Pico 2 W only for now: the nRF52840 always shows up as the emulated
  controller, whose configuration interface works without a controller
  too.)

### Over Wi-Fi

Turn the configuration Wi-Fi on in one of three ways:

* press the Pico's **BOOTSEL** button briefly (with *Pair only after
  pressing Sync* on, the default, hold it 1–5 seconds instead)
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
* toggle USB behaviour: report rate, LED following, wake-on-controller
* change the Wi-Fi name and password, reboot, enter firmware update mode, factory reset
* read the firmware log (useful for bug reports)

### Steam Deck / SteamOS: Decky plugin

`decky-plugin/` is a [Decky Loader](https://decky.xyz) plugin with quick
controls in the Quick Access menu: the profile, button mapping, Joy-Con 2
Mouse Mode, pairing and paired controllers, reboot and firmware updates. It talks to the dongle over USB like the
configuration page. See [decky-plugin/README.md](decky-plugin/README.md).

With the plugin, any button can open Steam's **Quick Access menu** (in any
mode and profile): map it to **Quick Access Menu (Decky)**. The dongle counts
taps of it (released without another button pressed meanwhile, so C still
works for C + Home and C + GL) and the plugin opens the menu, or closes it
when it's open. This
output only works in SteamOS Game Mode with the plugin running, so the
configuration page lists it only with **Device → Decky plugin → Show
additional Decky options** on; using the plugin turns that on for the
dongle. (Provisional: this feature may change or go away.)

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
**Device → Connection details → Dongle power** shows the USB and chip supply (with the lowest
seen since start), and the log records new lows (`power: … dropped to …`).
Try another port directly on the computer (not a hub) or another cable, and
don't charge the controller from the same hub. **Device → Power and radio →
Bluetooth transmit power** lowers the dongle's peak current; 0 dBm still reaches
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
| Joy-Con 2: connection, pairing, input, sideways use, pairs | **Untested.** Built from the published report layouts (the same report 0x05 as the Pro Controller, SL / SR bits, the Joy-Con-specific reports and rumble characteristics). |
| Joy-Con 2 Mouse Mode | ✅ Works on SteamOS in Xbox 360 mode. The sensor's position (report 0x05, offset 0x10) gives the movement and its distance field (0x16) whether it lies on a surface. |
| Two Bluetooth connections at once | **Untested** on both boards (nRF52840: two central links; Pico: BTstack with two connections on the CYW43439). |
| HD rumble **frequency** translation | **Low.** The Switch 2 frequency encoding hasn't been published. It is anchored on the console's idle frame (verified bit-exact in tests) with a scale and a *Fixed* fallback mode in the settings (no longer on the configuration page). |

If something doesn't work, the log on the configuration page usually shows
where it stopped. Please include it in bug reports.

## Known limitations

* **No amiibo.** The Switch doesn't use a Pro Controller's NFC reader over a
  wired USB connection, so the dongle doesn't read tags. It answers the
  NFC/IR chip setup commands like an idle controller.
* One controller connected at a time (up to 8 remembered), except a Joy-Con
  2 (L) together with a Joy-Con 2 (R).
* A single sideways Joy-Con 2's motion isn't turned to match how it's held.
* No drawing of the Joy-Con 2 on the configuration page yet.
* Going back to firmware older than 0.10.0 resets the settings (pairings
  included): the older firmware doesn't read the larger settings.
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
| `core/src/s2_link.c` | Switch 2 controller logic: which adverts to connect to (one controller, or a Joy-Con 2 pair on two links), init and Nintendo pairing commands, input, rumble pacing, LEDs, gyro calibration |
| `core/src/joycon.c` | Joy-Con 2: one controller from an (L) and (R), a single one turned sideways, the optical sensor as a mouse |
| `core/src/s2_transport.h` | Interface each board's Bluetooth stack implements |
| `core/src/s2_proto.c` | Switch 2 protocol: adverts, command framing, report and calibration parsing |
| `core/src/procon.c` | Emulated Switch 1 Pro Controller: USB handshake, subcommands, SPI flash, input reports |
| `core/src/usb_hid.c`, `usb_pro_desc.c` | TinyUSB HID class drivers (the controller, the Joy-Con 2 mouse) and the Pro Controller report descriptor |
| `core/src/hd_rumble.c` | Switch 1 HD rumble decoder and Switch 2 encoder |
| `core/src/mapping.c` | Button remapping, stick calibration and deadzones, IMU conversion |
| `core/src/mode_select.c` | Profile shortcut on the controller (C + Home, then a button; not on a single Joy-Con 2) |
| `core/src/profiles.c` | Profiles per controller type: defaults, checks |
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
