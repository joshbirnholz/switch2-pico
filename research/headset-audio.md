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
  Josh "thinks [they] heard something at 1 and 2, but [isn't] sure". No 1 kHz
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

### Third audio stream test (7.5 ms, silent mic prompts, stereo output)

- **At 7.5 ms every mic frame arrives:** 132.5 notifications/s, 52.9
  frames/s, nothing lost; 22 s of mic gave 22.0 s of audio.
- **The codec is confirmed: Opus CELT-only, FB, 20 ms, mono, 50 bytes
  CBR (20 kbit/s).** The mic frames match libopus encoding faint white
  noise: second byte mostly 0x59/0x51/0x5a, the same entropy profile.
  Random bytes after an 0xF8 TOC decode ~300× louder and look nothing
  like them. And libopus at 20 kbit/s CBR encodes digital silence as
  `f8 ff fe` + zero bytes, exactly the controller's frame when nothing is
  plugged in.
- **But the mic isn't live.** Taps, rubbing and close whispering (prompted
  and timestamped) left no trace. The decoded level stays at ~-75 dBFS
  the whole time. The encoder is fed an input with no mic signal in it:
  muted, no gain, or no mic bias.
- Output: a thump from the controller's body at formats 1-4 (mono 20 ms,
  stereo 20 ms 50 B, stereo 20 ms 100 B, stereo 10 ms), none at 5 (stereo
  5 ms), and **nothing from the headphones** in any of them.
- Jack state on plugging in: byte 13 went 0x04, then 0x07.
- ndeadly's commands.md lists more of the 0x18 family: `0x18/0x01`
  (8-byte response, matches ours), `0x18/0x02` (no example),
  **`0x18/0x03`, seen from the console as `18 91 01 03 00 01 00 00 07`
  and echoed as `07`**, and `0x18/0x04` (no details). 0x07 is also the
  "headset with mic" jack state, so `0x18/0x03` may select the active
  audio paths, and we never sent it. Next run: `0x18/0x03 07` after the
  stream setup, plus `0x10/0x01` (firmware versions, read only).
- Also from commands.md, for safety: `0x02/0x02` (write block),
  `0x02/0x03` (erase sector) and `0x02/0x05` (memory write, allowed at
  >= 0x1F5000) write flash. `0x03/0x07-0x09` and `0x15/0x03` write
  pairing data. `0x0D/*` is firmware update. `0x06/0x02` and `0x06/0x03`
  shut down or reboot the controller. Never send any of these.

### Fourth test and probe (2026-10-20)

- `0x10/0x01`: firmware **2.1.4**, type 2 (Pro), Bluetooth patch 12.0.0,
  **DSP 0.2.3**.
- `0x18/0x03 07` is accepted and echoed (`07`), as on the console. It
  changed nothing: mic still dead, no output from the jack, and thumps at
  1-4 as before.
- `probe_audio.py` asked each feature bit with `0x0C/0x01`. Answers
  (bytes 4-11 of the data): 0x01 / 0x02 / 0x04 / 0x20 → `07` in their own
  slot, 0x10 (mouse) → `01`, 0x80 (magnetometer) → `01`, and **0x08 and
  0x40 → all zero**. So no hidden headset feature bit.
- Configurations that didn't wake the mic or the jack:
  - feature masks 0x3F and 0xAF, then back to 0x2F;
  - `0x18/0x03 07` resent right before `0x17/0x02`;
  - `0x17/0x02` mode 0x01 and 0x03, then 0x02 again.
  The thump came only once, at the first tone of the run, so it looks like
  a reaction to audio-out starting after a pause, not to the content.
- `arch_guess.py` on the other partly filled flash regions (0x000000,
  0x040000, 0x070000, 0x0B0000, 0x0D0000) finds no instruction set, so
  the main MCU firmware is encrypted everywhere. The command handlers
  can't be read statically.
### USB-C check, and the output plan (2026-10-20)

- Over USB-C the controller is `057e:2069` "Switch 2 Pro Controller", a
  UAC device (ALSA card "Controller"). Playback: 'PCM Playback'
  switch/volume (-39..+39 dB). Capture: 'Mic Capture' switch/volume, on
  and at maximum, **2 channels** (FL, FR), not mono.
- **Output works over USB** (Josh hears audio in the earbuds), so the jack
  and earbuds are fine for output. Over Bluetooth it's still silent, so
  output is a protocol problem.
- **The mic is dead over USB too:** 15 s of taps recorded from
  `hw:Controller` show only the noise floor (RMS 5.4, peak ~25), the same
  as the Bluetooth mic stream decoded (RMS ~6). So the Bluetooth mic
  stream was likely faithful all along. These earbuds' mic doesn't work
  with this jack (CTIA/OMTP wiring?) or doesn't work at all. Retest the
  mic with another headset.
- From ndeadly's bluetooth_interface.md:
  - Audio-in = "input report 0x09 with 0x34 bytes of headset audio
    inserted at 0x0D": [state][0x32][50 bytes]. The state alternates
    0x07/0x0F with a headset and 0x05/0x0D with headphones.
  - `…b06` (0x2c) is "Output Report (Headset Audio)", with no format.
  - `3dacbc7e-…-379` (0x16, Pro) is "HD rumble + command":
    [00][16 B rumble L][16 B rumble R][8 B command header][data].
  - `3dacbc7e-…-380` (0x32, firmware 2.0+) is "Unknown + Command",
    probably the audio counterpart of 0x16.
  - `4147423d` (0x18) is the chunked large-command/firmware-update report.
    Don't use it.
- Next: `output_sweep.py` writes the mic's own frame format (Opus CELT 20
  ms mono, 50 B) to 0x2c in 11 layouts built around the input block
  ([state][32][frame] with state 08/0F/0D/07, with a 00 or seq prefix,
  [00][frame], and a 112-byte copy of an audio-in report).

### Output sweep result (2026-10-20)

- **`[00][0F][32][50-byte Opus frame]` on 0x2c plays.** It's layout 6:
  Opus CELT FB 20 ms mono, 20 kbit/s CBR, one packet per 20 ms. Josh heard
  the 1 kHz tone **from the controller's body** (the HD rumble actuators
  acting as a speaker?), **not from the headphones in the jack**. So the
  controller decodes our Opus and plays it. The format of the frame is
  right, and the routing isn't.
- The leading `00` is required: layout 2, `[0F][32][frame]`, did nothing.
  So did 08/0D/07 without the 00, `[00][08][32]`, `[00][32]`,
  `[00][frame]`, the seq and 0x50|seq prefixes, and the 112-byte mirror.
- Next: `output_sweep.py --states`, the same packet with every state byte
  0x00-0x1F plus 0x2F/0x4F/0x8F. In audio-in this byte is a bitfield (bit 3
  = frame present, 0x07 headset, 0x05 headphones), so it may pick the
  destination.
- **The state byte isn't the trigger.** Full `--states` sweep at -20 dBFS
  (`sweep-20261020-135703`): a tone at step 1 or 2 only. Repeat at -40
  dBFS, stopped after step 3: the tone at step 2 (state 0x01). Nothing
  from the headphones in either. In all three runs, **per connection, the
  first stream with the leading 00 is silent, the second plays (from the
  controller's body), and none after that play**, whatever the state byte
  (0x0F in the first sweep, 0x01 since).
- Next: `output_sweep.py --timing`. A: one continuous 20 s stream with a
  seconds counter (warm-up, then a stop?). B: 2 s bursts, each after
  resending 0x17/0x02 (do the gaps end the playing state?).

### Measuring the jack (2026-10-20, at home, laptop on Ethernet)

- The MacBook's own jack input records digital zeros under Linux (even
  the internal mic; 'Capture Source' won't switch), so it can't be used.
  A C-Media USB adapter ("Cable Creation", `0d8c:0014`, mono mic in) works.
  Its Auto Gain Control must be off, with Mic Capture Volume ~30 (+18 dB);
  with AGC on, line level clips hard. The controller's jack goes to its
  input with a plain aux cable; the controller reports that as headphones
  (state 0x0D).
- `record.py` records with a wall-clock start time; `measure_tone.py` lines
  it up with a timestamped test log and measures the tone per step
  (verified exact on a synthetic -30 dBFS tone). The card's floor is about
  -84 dBFS.
- `--timing` at -30 dBFS (`timing1`): **nothing at the jack** in the
  continuous 20 s stream or the four bursts. Josh heard two thumps from
  the body and no tone. The only signals in the recording are thumps when
  the controller woke and connected, and ~3 s after it disconnected, so
  the jack's amplifier switches with the connection.
- **`0x18/0x02` with no payload crashes the controller:** no response,
  then a link supervision timeout 2 s later. It came back advertising for
  the laptop by itself. Don't send it (or `0x18/0x04`) without a known
  payload.
- **`0x18/0x03` values 00-1D** (route2): every value echoed; `0x18/0x01`
  unchanged (`00 00 40 f0 00 00 60 00`); **no tone at the jack for any of
  them** (measured 00-18; the recording ended before 19-1D).
- `jack_sweep.py` (jack1): [00][0F][32] around 20 ms mono, 5 ms stereo
  (state 0F/0D/07) and 5 ms mono: nothing at the jack. But at the 15 ms
  interval the 5 ms formats only went out at 75-110 packets/s, not 200,
  so they weren't a fair test. They need the 7.5 ms interval (root:
  `set_conn_params.py 6 6 0 200`; it doesn't survive a reboot).

### Sniffing the Switch 2 (2026-10-20)

- A spare Pro Micro nRF52840 (nice!nano clone, Adafruit UF2 bootloader
  0.6.0, S140 6.1.1) runs Nordic's nRF Sniffer 4.1.0 from Adafruit's
  `sniffer_nrf52840dongle_4.1.0.uf2`, which writes 0x1000-0x12000 over the
  SoftDevice. Restore: `s140_nrf52_6.1.1_softdevice.uf2`, then the app
  UF2. `dongle_bootloader.py` (sudo) puts a switch2-pico board into UF2
  mode over USB. The board shows up as `1915:522a` on `/dev/ttyACM0`; give
  the user access with `sudo setfacl -m u:$USER:rw /dev/ttyACM0`.
  `sniff.py` drives it with Nordic's SnifferAPI (copied to
  `data/nrf_sniffer/`, not in git).
- **Switch 2 console**: address `98:E2:55:CE:0F:9D`. Connection request to
  the controller: interval 15 ms, latency 0, timeout 2 s, all 37
  channels. The controller asks for `LENGTH_REQ` 251 B; the console then
  switches to **2M PHY** (`PHY_REQ 02 02`, instant 13) and asks for **MTU
  512**.
- **The console's first GATT steps** (in the clear):
  1. Read By Type for `00c5af5d-…-81` (the response carried handle 0x0002
     and `04 00 05 00 01 01 00`, not fully decoded yet).
  2. **Write Request `01 00` to handle 0x0005**, `00c5af5d-…-82`. The
     console does write to this service. It's still not ours to write.
  3. Read By Type for `00c5af5d-…-83` (the response was missed).
- **Then the link is encrypted, without any 0x15 exchange.** From about
  event 70 every payload is ciphertext. Controller→console packets are 74
  B (63-byte input report + headers + 4-byte MIC); console→controller ones
  are 52-53 and 134 B. That controller had been paired to this console
  before, so they already share a key. Without the key, the sniffer loses
  the encrypted link-control updates and only catches stray packets. A
  capture needs a first-time pairing (the console forgets the controller)
  so the key exchange happens in the clear.
- Capturing that pairing took a few tries. Hopping 37/38/39 with
  `scan()` defaults, the sniffer went silent as soon as it followed the
  controller in pairing mode (an ordinary device followed fine). With
  `scan(findScanRsp=True, findAux=True)` the next pairing was caught
  (`sniff-20261020-182504`).
- **First-time pairing, in the clear.** After the 00c5af5d exchange and
  MTU 512, the console subscribes to the CCCDs at 0x001b (cmd-rsp), 0x001f
  (cmd-rsp2) and 0x0023 (`d3bd69d2`). **It sends every command inside the
  HD rumble + command report** (0x0016, write-without-response, 41-58 B):
  `[00][16 B rumble L][16 B rumble R][8 B command header][data]`, with the
  rumble zeroed. Responses come back on 0x001a. In order:
  - `07/01`, `02/04` (read 0x13000), `10/01`, `16/01`;
  - `15/01 00 02 <98:E2:55:CE:0F:9D> <98:E2:55:CE:0F:9C>`: two host
    addresses, the console's and that minus one;
  - `15/04`, then `15/02`, then `15/03`.
  The controller's B1 (`5cf6ee79…5f10`) is the same one it gave the laptop.
- **LTK = reverse(A1 ⊕ B1)**, using the 16 bytes as sent in `15/04` and
  its response. For this pairing: `321d351468ed07c3c8949cceaaf760ce`
  (`data/ltk.txt`). It's verified by `15/02`: the controller answers
  B2 = AES-128(LTK, reverse(A2)), byte for byte as sent. (The dongle's
  stored `ltk` is A1 ⊕ B1 without the reversal; it never uses it.)
- Right after `15/03`, the console sends `LL_ENC_REQ` with Rand = 0 and
  EDIV = 0, and the controller starts encryption. **Verified offline**:
  SK = AES(LTK, SKDm‖SKDs), then AES-CCM with IV = IVm‖IVs, decrypts both
  START_ENC_RSPs to `06` with valid MICs, using the LTK most significant
  byte first (`321d…60ce`; the reversed order fails). So it's standard LE
  encryption with that key. On reconnection (console asleep, then Home)
  the console repeats 00c5af5d, the CCCDs, `07/01 02/04 10/01 16/01`, and
  goes straight to `ENC_REQ` (no 0x15).
- The nRF Sniffer didn't decrypt with the key given as a **legacy** LTK in
  either byte order. (Also: SnifferAPI sends an all-zero key with every
  `scan()`, so the key has to be resent after each.) Next: as an LE Secure
  Connections LTK (`sendSCLTK`), which fits Rand = EDIV = 0.
- With two sniffer boards next to the controller (`sniff-20261020-191314`),
  board B caught the whole reconnection including both `ENC_REQ` and
  `ENC_RSP`, with key A loaded as a **legacy** LTK, and still didn't
  decrypt. Offline, key A decrypts that session's START_ENC_RSPs (MIC OK).
  So the firmware won't use it as a legacy key in that order. Still
  untested with a complete capture: key A as an SC LTK, and key B as legacy.
- The console's ENC_REQ SKDm/IVm repeat between sessions in shifted pieces
  (`dca4a469…`, `07a23bee…`), so its "random" values aren't very random.

### Decrypted: how the Switch 2 sends headphone audio (2026-10-20)

**The nRF Sniffer decrypts with the key loaded as an LE Secure Connections
LTK** (`sendSCLTK`, key most significant byte first), provided the same
board catches both ENC_REQ and ENC_RSP. Boards right next to the
controller and laptop Wi-Fi off (it was on 2.4 GHz) made that work.
Capture: `sniff-20261020-191503-ttyACM1.pcap`, decoded offline with
`decode_pcap.py` (live decoding stripped the MIC twice; fixed offline).

After encryption the console sends, all inside the rumble + command
report (0x0016) unless noted:
1. `0a/02 03`, `09/07 01`, **`0c/02 27`** (mask 0x27, without 0x08);
2. memory reads 0x13080, 0x130C0, 0x1FC040 (0x40 each), 0x13040 (0x10),
   0x13100 (0x18);
3. **`11/03`** → `01 c0 03 00 00 e7 d0 1c 3b 79 22 a0 3a 0a e8 9c 42 58 a0
   0b 42 0a e8 9c 41 58 a0 0b 41`; then a read of 0x13060 (0x20);
4. a **Write Request `85 00` to descriptor 0x0010** ("report rate?" of
   input report 0x09) and **`01 00` to CCCD 0x000f**: the console reads
   **input report 0x09 (handle 0x000e, 63 B)**, not 0x05;
5. `0a/08 01 ff…35 00 46…`, **`11/01`** → `03 00 00 00`, `0c/04 27`,
   `10/01`, **`01/0c`** → `61 12 50 10`, `18/01`;
6. **`LL_CONNECTION_UPDATE_IND`: interval 4 (5 ms)**, latency 0, timeout
   2 s. That's below the spec's 7.5 ms minimum. Then periodic
   `CHANNEL_MAP_IND`s;
7. **`18/04 01`** (sent three times in a row), ACKed;
8. through the **audio + command report 0x0032**, as `[00][command]`:
   `18/03 07`, then **`17/02 80 bb 00 00 02 f0 00`** (same as ours).
   0x0032 is `[audio length][audio block][command]`: later writes carry a
   123-byte audio block (`7b | 00 02 78 …`) followed by `0a/02 00 00 00 00`.

**Audio out (0x002c):** each 20 ms Opus frame is **240 bytes, sent as two
halves 5 ms apart** (then 15 ms to the next frame):
- `[00][04][78]` + the first 120 bytes (starts with the TOC);
- `[00][02][78]` + the last 120 bytes.

The frames are **Opus CELT-only, fullband, 20 ms, stereo, CBR 240 B
(96 kbit/s), TOC 0xFC**. Silence is `fc ff fe` + 237 zeros, exactly what
libopus produces for digital silence at 96 kbit/s stereo CBR, so in silence
the second half is all zeros. That first made it look like filler, and the
first reading of this section took the halves for 120-byte frames. 2546
frames (04+02) decode with libopus without errors
(`data/console-audio-out.wav`). Some halves are sent twice.

Differences from our earlier attempts: stereo 240 B frames in two halves
(we sent mono 50 B whole), state bytes 0x04/0x02 (we sent 0x0F), the 0x0016/0x0032
command reports, `18/04 01`, `11/03`, `11/01`, `01/0c`, mask 0x27, input
report 0x09 with `85 00`, a 5 ms interval, and encryption.
`console_out.py` does all of that except the last two.
- The jack recording during these sessions (`console1.wav`, USB card)
  shows the console's audio at about -34 dBFS whenever it was connected,
  with a thump at each connect and disconnect.

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

### First audio from the jack (2026-10-20, `console_out.py`)

The console's sequence and format replayed from the laptop (unencrypted,
15 ms interval, re-paired with `--pair`): **the 1 kHz tone reached the
headphone jack**: -26 to -40 dBFS around 850-1000 Hz for about 0.3 s, then
0.2 s of silence, then a steady 1150 Hz at -39 dBFS that continues after
the stream stops. Its peaks fall on multiples of 50 Hz, which looks like
the decoder underrunning and looping its last 20 ms. So encryption and
the 5 ms interval aren't needed for the jack to play; delivery timing is
the remaining problem. The console sends 100 packets/s (frames plus
fillers) at a 5 ms interval; the laptop at 15 ms managed ~75-110
writes/s before. Next: the 7.5 ms interval, and `--no-filler` (50
packets/s).

### Headphone output solved (2026-10-20)

- With each 240-byte frame split into the `04` and `02` halves, the laptop
  plays **a clean tone from the headphone jack**. 440 Hz at -30 dBFS came
  out at **440.00 Hz, -19 dBFS, THD -71 dB, everything else -50 dB, no
  dropouts in 5.4 s**, starting and stopping with the stream. That was an
  unencrypted link from the laptop at 7.5 ms (`console_out.py
  --freq=440`, recording `console-out-20261020-192912.wav`).
- The runs before that were explained by the format:
  - 120-byte frames followed by 120 zero bytes in the `02` packet: the
    controller decoded half-zero 240-byte frames into a looping, wrong-
    pitch sound (steady 900/1150/560 Hz);
  - no `02` packets: silence, since the frames were never complete;
  - the "body playback" and thumps of the very first tests came from bare
    or `[00][0F]` packets.
- **Recipe** (`console_out.py`), as the console does it:
  1. commands inside the rumble + command report 0x0016:
     `07/01, 02/04 0x13000, 10/01, 16/01, [0x15 pairing],
     0a/02 03, 09/07 01, 0c/02 27, (memory reads), 11/03, 0a/08 …,
     11/01, 0c/04 27, 10/01, 01/0c, 18/01, 18/04 01`;
  2. `85 00` to 0x0010 and `01 00` to 0x000f (input report 0x09);
  3. `18/03 07`, then `17/02 80 bb 00 00 02 f0 00`, via the audio +
     command report 0x0032 as `[00][command]`;
  4. 240-byte frames in two halves on 0x002c, as above.
  Not yet known which of steps 1-3 are actually required.
- Next: find the minimal command set, retest the mic with a headset whose
  mic works (these earbuds' mic is dead on the controller even over USB),
  and port to the dongle (an Opus encoder on the RP2350; 96 kbit/s out is
  ~100 writes/s).

### Minimal command set (2026-10-20, `ablate.py`)

- Run with the fewest commands first, on a controller that had been
  asleep (`ablate-*` second run): **no commands → silent; `17/02 80 bb 00
  00 02 f0 00` alone → plays**, on the plain command characteristic 0x0014
  (as the dongle sends commands) or through the rumble report, at -24 dBFS
  for a -30 dBFS tone. The console's full sequence plays at **-19 dBFS**,
  so something in it adds 5 dB (`18/04 01` or `18/03 07`?).
- Not needed for sound: encryption, the 5 ms interval, the 0x0016/0x0032
  reports, input report 0x09 / `85 00`, `11/xx`, `01/0c`, mask 0x27, any
  pairing beyond what reconnecting needs.
- The first run (most commands first, quick reconnects) played in every
  variant, even "bare": a variant can inherit the audio state of the one
  before, so order and sleep matter in these tests.
- **`18/03` and `18/04` aren't volume controls** (`volume_sweep.py`,
  one connection, 1 s of 440 Hz per step, -30 dBFS in):
  - `18/03` 01-14 all -18.9 dBFS; `1f` -23.9 (about 5 dB down); `00`
    silent, but it was the first step, and the first step is silent in
    both sweeps (see below).
  - **`18/04`: `00` mutes, any non-zero value (01-1f) plays at -18.9.**
    After `18/04 00`, `18/03 07` doesn't bring the sound back; `18/04 01`
    does.
  - So there are no volume steps in either: scale the PCM before
    encoding (the console presumably does the same).
- **Start-up delay:** the jack comes up 0.4-0.5 s after the first frame
  (seen in every recording), so the first ~0.5 s of a stream is lost.
  That probably explains the odd single runs (`17/02` alone at -24,
  `18/04 01` then `17/02` at -50, the first sweep step silent).
- **For the dongle:** send `18/04 01`, `18/03 07`, `17/02 80 bb 00 00 02
  f0 00` (the console's order) on the command characteristic, then 240 B
  stereo Opus frames in two halves on 0x002c; `18/04 00` to mute.

### Console settings that matter (reported by Josh, 2026-10-20)

- **System Settings → Audio Output → "Switch 2 Pro Controller Audio
  Mode": Stable / Low Latency.** "Selecting Low Latency will reduce audio
  lag compared to Stable. However, the sound may be more likely to cut
  out." Every capture so far was in **Stable**. That may explain 20 ms
  frames despite `17/02` asking for 240 samples (5 ms), and the halves sent
  twice (redundancy). Next: capture Low Latency (frame length, `17/02`
  payload, duplicates).
- **The console's microphone test** lists the Pro Controller mic as an
  input when the earbuds-with-mic are plugged in (not with the aux cable,
  which it treats as headphones), **but records nothing**: the console
  can't get sound from this mic either. So the dead mic in our tests isn't
  a protocol problem. The same earbuds record fine plugged **straight into** a
  MacBook Air's headphone jack (Audacity, "External Microphone"). So the
  earbuds' mic works, and the failure sits between them and the
  controller's jack: no signal over Bluetooth, over USB, or in the console's
  own mic test. Either this headset doesn't suit the controller's jack (mic
  bias, as with Apple EarPods on some devices) or the controller's mic input
  is faulty. A different headset tells which. Next: sniff the mic test (the mic-enable commands, audio-in
  subscription) even if the signal stays silent, and try another headset
  or the TRRS splitter.

### Low Latency mode capture (2026-10-20, `sniff-20261020-202539-ttyACM1`)

- A fresh pairing was caught by board B. sniff.py derived the LTK
  (`a7a32c7e6824021fec170368deab91ce`) from 15/04 and loaded it before
  ENC_REQ, so this session was decrypted live.
- Same sequence as in Stable, except **`18/03 02` instead of `18/03 07`**
  (via 0x0032). **`18/03` is the Pro Controller audio mode: 07 = Stable,
  02 = Low Latency**, which is why its values didn't change the level.
  `17/02 80 bb 00 00 02 f0 00` is unchanged.
- Audio-out is the same format: 20 ms stereo CELT frames of 240 B in
  `04`/`02` halves, TOC 0xFC. The latency difference is probably the
  controller's buffering, set by 18/03.
- Not done yet: measure the latency in each mode from the laptop (18/03 02
  vs 07); sniff the console's mic test; the mono-frame test; Opus on the
  RP2350.

### The mic works: console mic test, decrypted (2026-10-20, `sniff-20261020-223146-ttyACM1`)

- With **Low Latency** mode set, the console's mic test **recorded and
  played back** the earbuds' mic. The same test earlier, in **Stable**,
  recorded nothing.
- Sequence on reconnection (key `a7a3…91ce` from the Low Latency pairing):
  the usual setup, then `18/04 01`, `18/03 02` and `17/02` (0x0032), then
  **unsubscribe input report 0x09 (`00 00` to 0x000f) and subscribe to
  audio-in (`01 00` to 0x002f)**. About 17 s later it switches back
  (0x002f off, 0x000f on).
- Audio-in: 112-byte notifications at about 183/s (5 ms interval); 50
  frames/s, each 50 B Opus CELT FB 20 ms mono, TOC 0xF8 (as before).
  **Byte 13 is now 0x17/0x1f/0x27/0x2f**; it was 0x07/0x0f in our Stable
  tests, so bits 4-5 are new. Decoded (`data/console-mic-test.wav`): noise
  floor around -71 dBFS, then **speech-like signal up to -18 dBFS at
  35-41 s**, which was the mic test's recording.
- Theory: **the mic is live only in Low Latency mode (`18/03 02`)**. Next:
  test 02 vs 07 from the laptop.
- **Mic frames are 100 B, split in two like the output:** every 20 ms,
  four notifications 5 ms apart: byte 13 `2f` (or `1f`) with 50 B starting
  with the TOC 0xF8, then `17` with the other 50 B, then two `27` with
  length 0. So **Opus CELT FB 20 ms mono, CBR 100 B (40 kbit/s)**; bit 3 of
  byte 13 marks the first half. Decoding only the first halves (as
  `audio_stream.py` did) gives distorted audio; joined, the mic test
  decodes properly (5483 frames, `data/console-mic-test.wav`; Josh
  listened: "perfectly clean").
  Earlier Stable-mode captures showed `07`/`0f` only; their second halves
  were never looked at.

### The mic from the laptop (2026-10-20, `mic_test.py`, `mic_ablate.py`)

- **The mic works from the laptop.** `mic_test.py --full` (the console's
  setup sequence, then `18/04 01`, `18/03`, `17/02`, input report 0x09 off,
  audio-in on) recorded Josh's voice cleanly: floor -71 dBFS, voice -31 to
  -40 dBFS, **in both Low Latency (02) and Stable (07)**. So the mode
  isn't what enables the mic. (The console's Stable-mode mic test that
  "recorded nothing" earlier is unexplained; maybe the same state as below.)
- 50 frames/s need **one notification per 5 ms slot** (frame halves plus
  two empties); the controller sends one per connection event. At 15 ms
  (64/s) only 32 frames/s arrive; at 7.5 ms (149/s) all 50 do.
- Byte 13 bits 4-7 look like the controller's mic queue depth: 0-3 in Low
  Latency, up to 8 in Stable.
- **Two runs straight after the console's mic-test session gave flat noise
  at about -45 dBFS** (all frames, no voice; mic_test.py without --full).
  The first `--full` run was clean, and since then **every variant is
  clean, even with no extra commands**, across disconnects, sleep and a
  re-pairing. So the noise was a controller state the console left behind
  (it turned the mic stream off after its test). Something in its setup
  sequence clears it, and the clean state then persists. Which command
  wasn't found (that would need the noisy state again).
- **For the dongle:** send the console's whole setup sequence on every
  connection (it works on the plain command characteristic), then
  `18/04 01`, `18/03 07|02`, `17/02`; to record, write `00 00` to 0x000f
  (if input report 0x09 was on) and `01 00` to 0x002f; join the 100 B frame
  halves. Use a 7.5 ms interval while the mic is on.

### Output latency and mono frames (2026-10-20, `latency_mono.py`)

- From the laptop, 60 ms 1 kHz bursts on a running stream, first frame
  written → sound at the jack (USB card on the same clock; includes the
  laptop's Bluetooth queue, 7.5 ms interval):
  - **Stable (`18/03 07`): 155 ms**, all 10 bursts identical;
  - **Low Latency (`18/03 02`): 68 ms**, all 10 identical.
  `18/03` switches mode mid-stream. Low Latency is the one to use for
  games (matches the queue depth seen on the mic: up to 8 vs 0-3).
- **Mono frames don't work:** 440 Hz sent as mono CELT (TOC 0xF8, 240 B
  CBR, same two-half packets) came out as 473 + 483 Hz at -22 dBFS, with
  more energy outside the tone than in it. The output decoder expects
  stereo; send stereo (identical channels are cheap to code).
