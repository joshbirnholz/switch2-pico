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
| Command write | `649d4ac9-8eb7-4e6c-af44-1ea54fe5f005` | match the property, like the dongle: Write Request when it has "write", Write Command otherwise. On HEW70006172650 it is write-without-response only, and a Write Request gets error 0x03 |
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

### Setup check results (2026-10-20, Fedora 44, kernel 7.1.3)

- Dongle: not plugged in (no `2e8a:` device in `lsusb`).
- Bluetooth: `hci0` is the internal BCM20703A2 on UART (`hci_uart_bcm`),
  BlueZ 5.87. It's powered, with LE, central and peripheral roles, and
  isn't rfkill-blocked. Two warnings at boot: `BCM: firmware Patch file
  not found ('brcm/BCM.hcd')`, so it runs on its ROM firmware (build
  0229), and `Failed to set baudrate`, so the UART stays at the default
  speed. HCI version 8 means Bluetooth 4.2, so the LE data length
  extension may work but there's no 2M PHY. `scan.py` saw 42 LE
  advertisers in 15 s, so scanning works. Connecting to the controller,
  and holding a 7.5 ms interval, are still untested. If either fails,
  switch to a USB adapter.
- Audio: the internal Cirrus codec works here (out-of-tree
  `snd_hda_codec_cs8409`; analog out and a 2-channel capture device), but
  it has no line in. The only other input is the AVerMedia webcam mic.
  Phases 4 and 5 still need a USB sound card with a line or mic input.
- Python 3.14.6; `.venv` has bleak 3.0.2, numpy 2.5.4, scipy 1.18.1,
  sounddevice, soundfile. `.venv/` is gitignored.
- First connection attempts (`phase1.py`, internal adapter, default
  interval) failed. Each `Connected=True` took 15-20 s to arrive and
  dropped 0.3 s later, before GATT discovery finished, so the GATT table
  was never read. This happened three times in two runs. No disconnect
  reason yet: that needs `btmon` (root).
- `btmon` (`data/connect1.btsnoop`) showed HCI reason 0x3E (Connection
  Failed to be Established): after CONNECT_IND the controller never
  answered in the first 6 connection events. Two causes so far:
  1. **KDE Connect** keeps a Classic (BR/EDR) inquiry running, and the
     chip can't run it alongside an LE connection. Connections only came
     up between inquiries. Quitting `kdeconnectd` stops it
     (`Discovering: no`). Keep it closed during experiments, or turn off
     its Bluetooth backend.
  2. Even with no inquiry or scanning running, 7 of 8 attempts still
     failed with 0x3E and one with 0x08 (supervision timeout, 420 ms),
     although the signal was strong (-42 dBm). One attempt got as far as
     MTU exchange and primary service discovery before it timed out.
     Suspect: Wi-Fi is on 2.4 GHz channel 11 (`ETC Admin`). This Mac's
     Wi-Fi and Bluetooth share an antenna, and the Bluetooth chip runs
     without its firmware patch, so the two may not be sharing radio time
     properly. Not yet tested.
- Tools: `btmon` and `strings` are installed. `binwalk` isn't (needed in
  Phase 2; `dnf install binwalk`). `/sys/kernel/debug/bluetooth` needs root.

### Phase 1 results (2026-10-20)

Scripts: `att.py` (raw L2CAP ATT client, no BlueZ GATT), `s2.py`
(protocol helpers), `phase1.py`, `wifi-off.sh` (runs a command with Wi-Fi
off and always turns it back on). Run with KDE Connect closed:
`research/audio/wifi-off.sh .venv/bin/python research/audio/phase1.py 3C:A9:AB:69:58:AF 30`.

- **Wi-Fi off fixes the link.** With Wi-Fi off it connected on the first
  try (0.3 s over raw ATT) and stayed up for the whole run. Wi-Fi on
  2.4 GHz is what broke it before. Wi-Fi is the laptop's only network, so
  each experiment runs offline through `wifi-off.sh`.
- **BlueZ's GATT client can't be used.** The controller never answers
  Read By Group Type for secondary services (0x2801). BlueZ waits 30 s
  and disconnects. Over raw ATT, primary services, characteristics and
  Find Information were all answered.
- Controller: serial `HEW70006172650`, PID `0x2069`, MTU 512 (it accepted
  ours). Connection interval 48.75 ms (BlueZ default; no root to change
  it). Input reports came at ~21/s, one per connection event.
- All connect-sequence commands were ACKed, with no 0x15 pairing. User
  stick calibration at `0x1FC040`/`0x1FC080` is all `ff` (none), and
  factory calibration is present.
- Full GATT table in `gatt-pro2.txt`. Beyond the cheat sheet:
  - Service `00c5af5d-1964-4e30-8f51-1956f96bd280` (0x0001-0x0007) has
    `…281` read, `…282` **write**, `…283` read. This is probably firmware
    update or a vendor service. **Never write to it.**
  - In the main service: `3dacbc7e-…-6f9809e8b379` (0x0016),
    `4147423d-…-d23e5df59f8d` (0x0018), `ab7de9be-…fdf` (0x002a) and
    `3dacbc7e-…-6f9809e8b380` (0x0032) are write-without-response.
    `506d9f7d-…` (0x001e) and `d3bd69d2-…` (0x0022) notify, and
    `ab7de9be-…fde` (0x0026) is read/notify.
  - Every notify characteristic has its CCCD (0x2902) at value + 1, then
    one more descriptor: `679d5510-…` ("report rate?") after the input,
    0x09, `…fde` and audio-in characteristics, and
    `b746df8c-f358-495b-9cd2-e3bbeda4f979` after command response,
    `506d9f7d` and `d3bd69d2`.
  - Reads of characteristic values all return 0x02 (Read Not Permitted),
    even ones marked "read". GAP name: "Nintendo Switch 2 Pro Controller",
    appearance `0x03c4` (gamepad).
- **Without the 0x15 pairing, buttons and sticks read as idle.** In the
  first full run Josh felt the click, saw LED 1, and then pressed buttons
  and moved the sticks. Yet in all 617 reports (from the btmon capture),
  bytes 4-7 never changed and the stick bytes moved by only 1-2 counts.
  The IMU bytes 0x2A-0x3B changed constantly, so the reports were live.
  The dongle always pairs a controller that's in pairing mode, so it has
  never taken this path.
- **With `--pair`, inputs work. Phase 1 is done.** All four 0x15 steps
  were ACKed (the 0x15/0x01 reply echoes the controller's own address).
  Then every button decoded correctly with the layout in `s2_proto.c`
  (face buttons, L/R/ZL/ZR, +/-, Home, Capture, C, D-pad), and both sticks
  moved through their range. 615 reports in 30 s, and the link stayed up.
  The controller now has this laptop (`DC:A9:04:75:01:F6`) as a host.
  Always use `--pair` from now on when the controller is in pairing mode.

### Phase 2 progress (2026-10-20)

- `dump_flash.py` reads only (memory read `0x02/0x04`, 0x40 bytes). It
  writes as it goes, resumes, and reconnects. It was tested against a fake
  controller (drops, lost replies, refusals) and then on the real one.
- **Reconnecting without Sync works.** Once paired (`phase1.py --pair`),
  a button press wakes the controller. It advertises with the laptop's
  address in the host field (`f6 01 75 04 a9 dc`), and the raw ATT connect
  goes straight through. Memory reads don't need pairing.
- 8 KB trial: 128 chunks in about 12 s with nothing refused, about 10.7
  reads/s (two connection events per read at 48.75 ms). The full 2 MB
  would take about 51 min. Offset 0 starts `01 00 64 aa 20 53 59 53`
  (" SYS"), followed by high-entropy data, so it's probably encrypted or
  compressed.
- **The faster interval didn't apply yet.** Writing the debugfs
  `conn_min_interval` got "Permission denied" even as root, with no
  lockdown (cause unknown, maybe SELinux). `MinConnectionInterval=6` and
  `MaxConnectionInterval=12` in `/etc/bluetooth/main.conf` [LE] reach the
  kernel: btmon shows Set Default System Configuration 6/12 accepted after
  a BlueZ restart. Yet the next LE Create Connection still asked for
  0x18-0x28 (30-50 ms). BlueZ doesn't know the device (no stored
  connection parameters were loaded), so the cause is still open. Phase 3
  needs the short interval, so this has to be solved before then.
- **Full dump done** (`data/flash.bin`, 2 MB, sha256 `d7cf70ff…213a507b`
  in `data/flash.sha256`). It took 53 min unattended with no drops and no
  refusals: the controller stayed connected the whole time without any
  button presses. 0x13000 matches the device-info read (serial, PID,
  colors). Rough map, by entropy per 64 KiB:
  - 0x000000-0x01FFFF: mixed, half erased; " SYS" header at 0.
  - 0x020000-0x04FFFF and 0x080000-0x0AFFFF: entropy 8.0, two regions of
    the same size, so probably A/B firmware slots, encrypted or
    compressed.
  - 0x040000, 0x070000, 0x0B0000, 0x0D0000: partly filled blocks between
    them.
  - 0x0E0000-0x16FFFF and 0x1B0000-0x1EFFFF: erased.
  - **0x175000-0x1A7AC8 (~203 KiB): the DSP image, apparently not
    encrypted.** At 0x175318 there's `MT3616A0 DSP    `, followed by
    test patterns (`aa55aa55 33cc33cc 5a5a5a5a c3c3c3c3`). There are
    repetitive, code-like byte patterns around 0x177000, and
    0x180000-0x19FFFF has entropy ~7.4.
  - 0x1F0000-0x1FFFFF: nearly erased (the user stick calibration at
    0x1FC040 is empty).
  - binwalk finds no known formats in the image or in the DSP blob.

### DSP image and codec (2026-10-20)

- **DSP header** (at 0x175000, 24 bytes, little-endian): `DSPH`, version
  1, payload size `0x32AB0`, `0x00030200` (maybe version 3.2.0),
  `0x10090000` (maybe the load address), and the CRC32 of the payload
  (`0xF1CA2A7A`, verified). The payload is stored plain, not encrypted.
  Extract it with `dd` into `data/dsp.bin` (gitignored).
- **Architecture: Tensilica Xtensa, windowed ABI** (`arch_guess.py`). The
  `entry a1,N` prologue (`36 x1 00`) appears ~3200× more often than chance
  (238 functions), and `retw.n` (`1d f0`) ~170×. No other candidate
  scores. This fits a Cadence HiFi DSP, as in MediaTek/Airoha audio SoCs.
- **The codec is Opus** (`find_tables.py`), from exact tables:
  - CELT `eband5ms` (int16) at 0x1A2FF8;
  - the CELT energy probability model (`e_prob_model`) at 0x1A5358;
  - a SILK NLSF codebook row at 0x1A5548;
  - Q15 complex FFT twiddles for 480/240/120/60 points around
    0x1A3276-0x1A4D0C, which match CELT's standard 48 kHz/960 mode.
  - Two more increasing tables are not identified yet: u8 `0 2 5 9 14 20
    27 …135` at 0x1A2909, and int16 `0 1 … 8 16 24 36 44 … 106 134` at
    0x1A2E16.
  - No SBC, ADPCM, G.722, MS-ADPCM or µ-law tables. LC3 isn't in the
    table list yet.
- **The idle mic frame `f8 ff fe` is the standard Opus silence packet.**
  TOC `0xF8` means config 31: CELT-only, fullband, 20 ms, mono, one frame.
  libopus 1.6 decodes it to 960 samples of digital silence, with or
  without the 47 zero bytes of padding. The old "Opus ruled out" result
  came from idle captures only, and silence can't tell codecs apart.
- Open questions: the `0x17/0x02` payload says 240 samples per frame
  (5 ms), but the idle packet is 20 ms. Do real mic frames use other TOCs
  (e.g. 0xE8, config 29 = CELT FB 5 ms)? And does the output lane take
  Opus too? Phase 4 can test that directly: encode a 1 kHz tone with
  libopus (CELT-only, FB, 5 ms and 20 ms) and write the packets to `…b06`.

### First audio stream test (2026-10-20, `audio_stream.py`, 48.75 ms interval)

- `0x17/0x02 80 bb 00 00 02 f0 00` was ACKed with an empty payload.
  `0x18/0x01` returns `00 00 40 f0 00 00 60 00`, the same as seen on the
  console.
- Subscribing to audio-in (value 0x002e, CCCD 0x002f) gave 112-byte
  notifications, one per connection event (20.4/s). Every other one
  carries a frame, and none were lost by sequence number. Byte 13 = 0x0f
  (headset with mic, plus the frame bit).
- **With a mic plugged in, every frame is a real Opus packet:** 1929 of
  1929 were distinct, all 50 bytes, all TOC `0xF8` (CELT FB 20 ms mono),
  so CBR at 20 kbit/s. libopus decoded every one without an error.
- **But only 1 in 5 frames gets through:** 10 frames/s where 20 ms frames
  need 50/s. The controller sends one notification per connection event,
  so it needs about 100 events/s (an interval of 10 ms or less). Decoded,
  the stream is a flat noise floor of about -75 dBFS with no speech. CELT
  predicts each frame's band energies from the previous frame, so with
  80% of frames missing the decoder can't track them. This doesn't show
  yet whether the codec is right.
- Output: writes without response were throttled to a few per connection
  event. Only format 1 (Opus CELT 20 ms, 50 bytes, bare, 50 packets/s)
  went out at real-time rate; format 2 took 31.7 s for 4 s of audio.
  Josh "thinks he heard something at 1 and 2, but isn't sure". No 1 kHz
  showed up acoustically in the mic stream either, but the mic stream
  can't be decoded at this interval anyway.
- **Why main.conf's interval didn't apply:** the kernel keeps an LE
  parameter entry per address, created with the defaults in effect at the
  first connection. bluetoothd's Load Connection Parameters only clears
  old entries when it loads more than one (`param_count > 1`), and it
  loaded none, so the entry with 30-50 ms survived the restart.
  `set_conn_params.py` (root) sends Load Connection Parameters with one
  entry for the controller (7.5-15 ms, 2 s timeout), which updates it.
  Without root it gets status 0x14 (Permission Denied), so the request
  itself is well formed.

### Second audio stream test (after `set_conn_params.py`, 7.5-15 ms)

- The fix took effect: 66 notifications/s (about a 15 ms interval, the top
  of the range), still one per connection event. 33 frames/s arrive, but
  no notification is lost by sequence number, so the controller drops a
  third of the frames itself. Next: pin the interval at 7.5 ms
  (`set_conn_params.py 6 6 0 200`) for about 133 events/s.
- Mic: the decoded audio is the noise floor (RMS ~6) except for two loud
  bursts, at 12-13 s and 19-23 s of the 20 s talking window (peak at full
  scale). Not clear yet whether that's the voice or artefacts from lost
  frames. Josh can listen to `data/audio-20261020-125720-full.wav`.
- Output (all formats now at real-time rate): **formats 1 and 2 (bare
  mono Opus, TOC 0xF8 20 ms and 0xE8 5 ms) gave a thump from the
  controller's body. Nothing came out of the headphones.** Formats 3-4
  (with a length byte first) and 5 (PCM16) got no reaction. So the
  controller seems to read the first byte as an Opus TOC. The thump is
  probably the rumble actuator, the same thing the earlier PCM test felt.
  Next: stereo Opus (TOC 0xFC/0xF4/0xEC), since USB audio is stereo out.

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
