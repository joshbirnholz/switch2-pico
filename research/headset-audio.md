# Pro Controller 2 headset audio: research handoff

Goal: get audio out to, and mic audio in from, the Switch 2 Pro Controller's
3.5 mm jack over Bluetooth LE, then expose it from the switch2-pico dongle as
a USB audio device.

This file is the starting point for a Claude Code session on Josh's Fedora
laptop (2016 MacBook Pro). That session works directly with the controller
over the laptop's Bluetooth from Python, which makes each experiment a quick
edit and rerun instead of a firmware rebuild. What it learns is the
controller's protocol, so it carries over to the dongle unchanged. Keep this
file up to date as findings come in: it is the project's record of the work.

## Where things stand (October 2026)

Nobody outside Nintendo has made headset audio work over Bluetooth. The
framing is documented, but the codec isn't.

- On controller firmware 2.0+, the jack uses two ordinary GATT
  characteristics in the same service as the rest of the controller. It
  does not use LE Audio or isochronous channels.
- A command (`0x17/0x02`) configures and starts the stream.
- The mic frames look compressed, at roughly 10:1 if one 50-byte frame
  covers 5 ms. The published "ruled out" list (Opus, LC3, SBC, IMA/OKI
  ADPCM, Nintendo DSP-ADPCM, µ-law, raw PCM) was tested **only on idle
  captures with no mic plugged in**, so it proves little. Nobody has
  analyzed a capture with real mic audio in it.
- Playback: someone wrote 16-bit PCM to the output characteristic and only
  felt a buzz from the rumble actuator. They later traced that to their
  own pacing bugs (a 440 Hz tone sent at a tenth of the right rate came
  out near 46 Hz). Whether correctly paced PCM plays from the jack is
  still untested.
- The controller's flash reportedly holds a MediaTek `MT3616A0` DSP
  firmware blob, which points to a vendor codec.
- Over USB-C, the controller is a standard USB Audio Class 1.0 device
  (48 kHz 16-bit stereo out, mono mic in), so the jack already works when
  the controller is plugged into a computer by USB. That's useful for
  reference recordings.

Sources:
- switch2mac research: [PROTOCOL.md §10](https://github.com/Peterksharma/switch2mac/blob/main/research/PROTOCOL.md),
  [audio-investigation.md](https://github.com/Peterksharma/switch2mac/blob/main/research/audio-investigation.md),
  [capture-format-analysis.md](https://github.com/Peterksharma/switch2mac/blob/main/research/capture-format-analysis.md)
- [ndeadly/switch2_controller_research](https://github.com/ndeadly/switch2_controller_research)
  (`bluetooth_interface.md` for the GATT table, `commands.md`)
- [trevlars/switch2-controllers-linux](https://github.com/trevlars/switch2-controllers-linux),
  a working raw-L2CAP BLE implementation on Linux
- This repo: `core/src/s2_proto.[ch]` (UUIDs, framing) and `core/src/s2_link.c`
  (`init_run()` is the exact command sequence the dongle uses).

## Protocol cheat sheet

### GATT (all in service `ab7de9be-89fe-49ad-828f-118f09df7fd0`)

| Purpose | UUID | Notes |
|---|---|---|
| Input report 0x05 (notify) | `ab7de9be-89fe-49ad-828f-118f09df7fd2` | what the dongle normally uses |
| Command write | `649d4ac9-8eb7-4e6c-af44-1ea54fe5f005` | the controller ignores Write Commands where Write Request is allowed; use a write with response |
| Command response (notify) | `c765a961-d9d8-4d36-a20a-5315b111836a` | |
| Rumble, Pro | `cc483f51-9258-427d-a939-630c31f72b05` | write without response |
| **Audio out (host to jack)** | `cc483f51-9258-427d-a939-630c31f72b06` | write without response; format unknown |
| Pro input report 0x09 (notify) | `7492866c-ec3e-4619-8258-32755ffcc0f8` | byte 1 = power info |
| **Audio in (jack mic to host)** | `7492866c-ec3e-4619-8258-32755ffcc0f9` | read, notify; firmware 2.0+ |
| "Report rate?" descriptor | `679d5510-5a24-4dee-9557-95df80486ecb` | the console writes `85 00`; we don't |

The CCCD sits at value handle + 1. On the dongle, BTstack's CCCD lookup
never got an answer, so it writes there directly. BlueZ/bleak may be fine,
but if notifications don't start, that's the cause.

**Never let the OS pair or bond (SMP).** The controller drops the link and
powers off when SMP is attempted. Don't pair it in GNOME Settings or
`bluetoothctl pair`; just connect. Nintendo's own pairing runs over the
command characteristic (`0x15`, see `init_run()`).

### Advertisement

Manufacturer data, company `0x0553`. Offsets include the 2-byte company id:
5 = VID (`0x057E`), 7 = PID (`0x2069` Pro Controller 2), 0x0B = `0x81`
when it wants a console to wake up, and 0x0C..0x11 = the host it wants to
reconnect to (all zero in pairing mode). Hold the sync button to put it in
pairing mode.

### Commands

```
byte 0 cmd | 1 0x91 | 2 0x01 (Bluetooth) | 3 sub | 4 0x00 | 5 payload len | 6 0x00 | 7 0x00 | 8.. payload
```

The response echoes cmd at byte 0, `0x01` at byte 1 and sub at byte 3, with
data from byte 8.

The dongle's connect sequence (`s2_link.c` `init_run()`):
`0x07/0x01`, memory read `0x13000` (device info), `0x16/0x01`, then the
`0x15` pairing steps only when the controller isn't bonded to this host,
then `0x0A/0x02 03 00 00 00` (a click), `0x09/0x07` (player LEDs),
`0x0C/0x02 <mask> 00 00 00`, the stick calibration reads,
`0x0A/0x08 <20 bytes>`, `0x0C/0x04 <mask> 00 00 00`, and finally it
subscribes to the input report. The feature mask on a Pro is `0x2F`
(buttons, sticks, IMU, unk 0x08, rumble). switch2mac uses `0xA7`.

If the controller drops the link or ignores commands after a few seconds,
it probably wants the `0x15` app-layer pairing. Copy those steps from
`init_run()`.

Memory read `0x02/0x04`: payload `len 0x7E 0x00 0x00 addr(u32 LE)`. Use a
`len` up to `0x40`; switch2mac says `0x4F` works. The dongle reads
`0x13000` (info), `0x13080`/`0x130C0` (factory sticks) and
`0x1FC040`/`0x1FC080` (user calibration), so the flash is at least 2 MB.

Audio:
- `0x17/0x02` payload `80 BB 00 00 02 F0 00` means 48000 Hz (u32 LE), mode
  or channels `0x02`, and 240 samples per frame (u16 LE). It's ACKed with
  an empty payload. Try other values too, such as mode 0x01, 16 kHz, and
  other frame sizes, and watch what changes.
- `0x18/0x01` is unexplored. It's been seen returning
  `00 00 40 f0 00 00 60 00`.

### Audio-in notification (112 bytes; measured on macOS at about 30 Hz)

```
 0      sequence (+1 per packet)
 1      0x20
 2-4    buttons
 5-10   sticks (packed 12-bit)
13      jack state: 0x00 nothing, 0x05 headphones, 0x07 headset with mic;
        bit 3 = this packet carries an audio frame (alternates)
14      audio frame length (0x32 = 50)
15-64   audio frame; idle = f8 ff fe + 47 zero bytes
65      telemetry length (0x28, rarely 0x04)
66-105  motion telemetry (12-bit tick counter in 1.25 ms units). Not audio,
        even though it looks like it.
```

While subscribed to audio in, the normal input reports stop. The rate
measured on macOS reflects macOS's slow connection interval. Measure it
again at 7.5 ms.

## Setup on Fedora

1. **Unplug the switch2-pico dongle**, or it will grab the controller.
2. Check the Bluetooth adapter: `bluetoothctl show`, `btmgmt info`,
   `dmesg | grep -i blue`. The 2016 MacBook Pro's Broadcom Bluetooth (on
   UART) has patchy Linux support. If it doesn't come up or won't connect
   to BLE peripherals, use a USB Bluetooth 5 adapter (Realtek RTL8761B
   based ones work in Fedora).
3. Audio: `arecord -l` and `aplay -l` (or `pactl list short sources/sinks`).
   The internal Cirrus audio on this Mac often doesn't work on Linux, and
   it has no line in, so plan on a USB sound card with a mic input.
4. Python: create a venv in this folder (`python3 -m venv .venv`) with
   `bleak numpy scipy sounddevice soundfile`. Ask before installing system
   packages (`dnf`).
5. Connection interval: BlueZ normally picks about 30-50 ms. To match the
   dongle (7.5-15 ms), set
   `/sys/kernel/debug/bluetooth/hci0/conn_min_interval` and
   `conn_max_interval` to 6 and 12 as root before connecting. Record which
   interval each capture used. Raw HCI traffic can be watched with
   `sudo btmon -w capture.btsnoop`.

Keep scripts in `research/audio/`. Keep dumps and captures out of git
(`research/audio/data/`, gitignored). The flash dump is Nintendo's
firmware and must never be committed or uploaded anywhere.

## Plan

Work only with commands that read. Don't send unknown command IDs, other
memory subcommands (writes and erases are likely nearby), or anything that
looks like a firmware update. A bricked controller ends the project.

**Phase 1: connect and map.** Scan, connect without pairing, dump the whole
GATT table (every service, characteristic, handle, property and
descriptor), run the init sequence, and print input reports. Done when the
buttons show up live.

**Phase 2: dump the flash.** Read sequentially from 0 in 0x40-byte chunks.
Make the dump resumable, write as it goes, retry on timeouts, and log the
address ranges the controller refuses. Expect 15-30 minutes. Then:
- run `binwalk`/`strings`, and find `MT3616A0` and the DSP blob's
  boundaries and header;
- work out the DSP's architecture before disassembling it (check the
  header, entropy and instruction patterns; don't assume one);
- look for codec signatures: known constant tables (SBC/mSBC, LC3/LC3plus,
  CELT/Opus, G.722, ADPCM step tables, aptX, LDAC) and strings that name
  the codec, sample rate or frame size;
- also look at the main MCU firmware for the code that handles
  `0x17`/`0x18` and the audio characteristics' handles.

**Phase 3: audio commands, no cable needed.** Send `0x17/0x02` with the
known payload, subscribe to audio in, and log raw packets with timestamps
at the 7.5 ms interval. Note the rate and how often bit 3 of byte 13 is
set. Try `0x18/0x01` and variations of the `0x17` payload, and record what
the ACKs and the stream do.

**Phase 4: output lane, with a plain stereo aux cable** from the
controller's jack to the sound card's input (the controller should report
0x05, headphones). Pace writes to `…b06` with write-without-response and
watch for backpressure. Record the line in while playing:
- PCM16 LE mono and stereo, 240 samples per 5 ms, chunked to the MTU, with
  a 1 kHz tone;
- the same with small headers (sequence byte, length byte) in front;
- PCM at the other rates the `0x17` payload accepts;
- if Phase 3 has real mic frames, write them back to `…b06` exactly as
  they arrived. If the two directions use the same codec, recorded sound
  comes out of the jack.

Analyze the recordings automatically: tone frequency, level and
distortion. A clean 1 kHz result means the format is solved for output.

**Phase 5: mic lane.** With phone earbuds that have a mic (state 0x07),
play a stimulus from the laptop speaker into the earbud mic: silence,
1 kHz, 300 Hz, a chirp, then noise, with clear silences between them, and
capture audio-in frames during it. Later, with a TRRS splitter (PC
headphone out to the splitter's mic jack, splitter's headphone jack to the
sound card input), feed the stimulus electrically for clean, repeatable
captures. If the controller doesn't detect a "headset" on the splitter, a
2.2 kΩ resistor across the mic pin or a cheap lapel mic will fix it. To
get reference audio, plug the controller in by USB-C and record the same
stimulus through its USB audio device.

Analyze the frames: look at entropy and bit-level structure, compare
against the reference, check frame size against the configured
rate/frame/mode, and decode attempts guided by whatever Phase 2 turned up.

## Bringing it to the dongle (once the format is known)

- Transport (`pico/s2_ble_btstack.c`, and the nRF52 port in
  `nrf52/switch2_nrf/s2_ble_bluefruit.cpp`): discovery already walks the
  service. Add the `…b06` and `…0f9` characteristics, an audio-in
  subscription like `s2t_poll_power()`, and an `S2T_CHAR_AUDIO_OUT`
  write. BTstack runs one GATT request at a time per link.
- Link layer (`core/src/s2_link.c`): send `0x17/0x02` once the link is
  ready, and parse audio-in packets. Buttons and sticks are in those
  packets too, so input keeps working while subscribed.
- USB: a TinyUSB audio class interface (48 kHz, 16-bit; stereo out, mono
  in), plus the codec on the RP2350. Endpoints 0x81-0x84 are taken (see
  `pico/usb_descriptors.c`). The Decky plugin and the config page will
  want an on/off setting.
- Bandwidth isn't a concern: the compressed stream is tens of kbit/s, and
  the dongle already asks for a 7.5-15 ms interval.
- Follow `CLAUDE.md` for any UI text.
