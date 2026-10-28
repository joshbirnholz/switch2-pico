# Agent handoff — current status, 2026-10-27

This section supersedes all status, branch, test-count, release and next-step
claims in the historical notes below. Read `CLAUDE.md`, this section,
`research/audio/REGRESSION.md`, and the latest sections of
`research/headset-audio.md` before continuing.

## Branch and readiness

- Review branch: `review/controller-fixes-research-20261027`.
- Based on `claude/project-thread-woan6n`, including commit `3eb5754`
  (headset microphone input, audio self-tests and research notes), which
  had not been pushed when this handoff was prepared.
- This is a review/handoff branch, not a production release. Do not infer
  release readiness from version numbers. No merge, release, firmware
  installation or new hardware test was performed in this handoff session.
- Microphone support now defaults on when headset audio is enabled;
  older notes saying it defaults off are historical.

## Latest changes included

1. GameCube trigger calibration: both neutral bytes must differ from the
   erased-flash sentinel `0xFF`; zero is accepted. If either byte is invalid,
   both existing defaults are preserved. The helper lives in `s2_proto.c`
   and is used by `s2_link.c`. Regression cases cover erased/partially erased,
   ordinary and zero-valued pairs.
2. Existing local headset fixes are included: background USB playback or
   capture with no headset should not start Bluetooth audio or replace normal
   input reports. `pico/headset_audio_pico.c` gates audio on jack presence;
   `mic_task()` gates host-requested audio-in on headset presence. README and
   jack-state regression tests describe this behavior. These latest changes
   still need end-to-end hardware testing, particularly insertion/removal
   while the host keeps playback/capture open.
3. `research/IMG_9142.JPG`, `IMG_9143.JPG`, and `IMG_9144.JPG` are included
   as supplied research images; this session did not interpret them.

## Linux driver comparison and remaining work

The original archive URL was unavailable:
https://ratatoskr.run/linux-input/2026/07/17260590/t
A later revision quoted in review was inspected instead:
https://marc.info/?l=linux-input&m=179009046906714&w=4

- Linux scales GameCube trigger travel to raw 232; our output implementations
  use 255 (`x360.c`, `ds5.c`, `sinput.c`, `gc_adapter.c`). Verify the maximum
  in BLE captures before changing scaling. A shared normalization helper
  would avoid inconsistent output modes. No scaling change has been made.
- Linux sends Joy-Con grip-button enable `0x08/0x02`; we do not. Verify its
  need and effect over BLE before changing initialization.
- Firmware/DSP version query `0x10/0x01` could improve diagnostics; it has
  not been implemented.
- The stick calibration layout and user magic agree with our implementation.
- Our response parser names byte 5 `ack`, while the Linux header calls it
  length; our code does not consume that field. Review framing against actual
  BLE responses before broader parser/error-handling changes.

## Validation and evidence

- `make -C test run`: PASS on 2026-10-27, 478 core checks, 35 USB lifecycle
  checks, UAC2 mic/speaker variants, jack state, mic session/API and Python
  audio preflight/Decky tests.
- Firmware checks on 2026-10-27: `cmake --build build/mic-default -j2`
  and `cmake --build build/uac2-speaker -j2` pass; exact embedded-web checks
  pass for both `.bin` images (UF2 uses block framing and cannot be checked
  by this raw-byte script). The mic build reports an existing URL-array
  NUL-terminator warning in `usb_ms_desc.c`. `git diff --check` passes.
- Hardware findings from earlier work are recorded in `research/headset-audio.md`,
  including the 2026-10-23 startup, clock and BLE reconnect validation. They
  do not establish hardware validation of the newest changes above.
- Raw captures/flash dumps in `research/audio/data/` (about 546 MB), build
  outputs in `build/`, Python environments and caches remain intentionally
  gitignored. They are not available from a GitHub clone. Committed research
  notes summarize results; historical references to those local paths and
  SteamOS `/tmp` files do not promise their availability to another agent.
- The Steam Controller/freeze inquiry remains PAUSED at the user's request.
  Do not resume it as part of this handoff. Do not assume permission for
  recording, playback or remote-device changes from historical commands.

## Suggested next steps

Review the diff and the regression checklist, then test headset insertion,
removal and reconnect while USB capture/playback remain open. Separately,
measure GameCube BLE trigger limits before changing scaling. Keep release
and deployment decisions separate from this review branch.

---

## Historical working notes (superseded where inconsistent)

# Handoff: headset mic with motion (as of 2026-10-22)

Read this first, then `CLAUDE.md` and the last two sections of
`research/headset-audio.md` ("Tested on hardware (2026-10-21, morning)" and
"Motion while the mic is on"). That file holds the full protocol notes.

## Update from the resumed session (2026-10-22)

- SteamOS SSH works with `ssh -F /dev/null deck@steamos.local` (the laptop's
  system SSH config has a permissions error). Network commands need sandbox
  escalation. Test helpers and both firmware images are in `/tmp/s2log/`.
- Tested `7f96ea3`: Josh reported gyro directions initially matched, then
  went out of sync. Captured the failure in
  `research/audio/data/handoff-20261021/motion/dongle-test-20261022.txt`.
- Found and corrected the quaternion interpretation: fields are ratios to
  an omitted component; frame 0/1/2/3 omits w/x/y/z, with the three fields
  in cyclic order. Set the omitted component to 1 and normalize all four.
  See the new "Quaternion correction (2026-10-22)" section of
  `research/headset-audio.md`; it supersedes the quaternion/frame claims below.
- Local changes are not committed. Host tests pass 464; all 11,429 nonempty
  captured blocks decode. New mic + dump firmware:
  `build/motion4/switch2_pico.uf2`. Installed successfully on SteamOS;
  hardware retest PASSED: Josh reports gyro icons match controller movement.
  All 8,490 nonempty blocks in the second capture decode, all four frames.
  Raw-dump polling coincided with audio underruns. Without polling, a full
  30 s window had 1,500 headphone frames, 0 missing/underruns and 0 busy
  writes; mic 1,501 frames, 0 bad/dropped, one USB-ring underflow. Next:
  speech level/stereo listening. See the newest research notes.
- Test services `s2play`, `s2rec`, `s2motion` were stopped after the failure.
  Use distinct unit names (or `--collect`) for subsequent tests. The failed
  test's recording is `/tmp/s2log/mic.wav`, motion dump
  `/tmp/s2log/motion-test-20261022.txt` on SteamOS.

## Goal

Headphone audio and the headset mic of a Switch 2 Pro Controller, through the
switch2-pico dongle (Pico 2 W), as a USB audio device, in as many USB modes as
possible. Headphone output shipped in **0.17.0** (plugin 0.6.0). The current
milestone is **the mic in release builds**. Today the mic is a build option
(`-DS2_HEADSET_MIC=ON`, default OFF), because motion stopped while it was open.

## Where things stand

- **Released:** main (`claude/dazzling-wright-gxo6vj`) = 28df2a3, firmware
  0.17.0, plugin 0.6.0. CI published both, and the auto-updater offers them.
- **Working branch** `claude/project-thread-woan6n` (pushed, not merged), two
  commits on top of main:
  - eeaeb23: the first motion decoder (one layout).
  - 7f96ea3: all three layouts, frames mapped back, gyro over ≥10 ms, and the
    debug dump (`-DS2_MOTION_DUMP=1`, GET /api/motion).
- **Tests:** `make -C test` passes 437.
- **The dongle** (on Josh's SteamOS PC) runs the official 0.17.0 release. It
  was put back after testing.

## What was found (2026-10-21 evening)

Audio-in notifications (the mic) replace the input report, so they carry their
own motion: a compact block from byte 65. It's the same block as input report
0x09's from byte 14, the report the console uses. The block is decoded:

- Byte 0 is the length, then a little-endian bit stream:
  - bits 0-11: tick (ms)
  - bits 12-16: time since the previous block
  - bits 25-27 together with the length select the layout
- Each layout holds an **orientation quaternion** (offset binary,
  w = +sqrt(1-|v|²)) and the **accelerometer** (the same units as report 0x05):

  | Layout | Frame bits | Quaternion | Accelerometer |
  |---|---|---|---|
  | 40 B, code 7 | 34-35 | 3×21 from 37 | 3×14 from 100 |
  | 40 B, code 6 | 34-35 | 3×23 from 37 | 3×22 from 106, 8 frac |
  | 30 B, code 6 | 32-33 | 3×31 from 35 | 3×32 from 128, 16 frac |

- **Gyro** = the rotation between two blocks over the tick difference. It
  includes the sensor bias, so the existing gyro calibration still applies.
  The scale on this controller is about 14.3 LSB/dps, the dongle's range B.
  Blocks only 2-4 ms apart give a noisy gyro, so use one at least 10 ms older.
- **Frames:** near a half turn the controller re-expresses the quaternion.
  Frame 1 stores (y,z,x) of frame 0, and frame 2 stores (z,−y,x). Frame 2 is
  less certain: one block had |v| = 1.09.
- **Which layouts arrive** depends on the interval. At the laptop's 15 ms only
  the code-7 layout came. At the dongle's 7.5 ms all three come mixed, about
  40/22/36%. The first hardware test only decoded the code-7 layout, and Steam's
  gyro view lagged and jumped; that is why 7f96ea3 exists.

Code: `s2_parse_motion()` and `s2_motion_gyro()` in `core/src/s2_proto.c`,
`on_audio_in_notification()` in `core/src/s2_link.c` (history of 4 blocks;
gyro holds when a block gives none, and reads still after 250 ms of undecodable
blocks), and the 30 s log line "mic motion, 30 s: N blocks decoded, M other".

## Next steps

1. **Hardware test of 7f96ea3.** Install
   `build/switch2_pico-motion3-mic.uf2`: mic on, motion dump on, reports
   0.17.0. Josh reconnects the controller with headphones plugged in. Open the
   mic remotely (below). Josh runs Steam → Settings → Controller → Test Device
   Inputs and moves the controller, including upside down. Expected: the log
   shows ~3000 decoded and ~0 other per 30 s, and the gyro view follows
   smoothly. If frame 2 misbehaves upside down, dump blocks (/api/motion) while
   he flips it and fix the frame-2 mapping. Afterwards put 0.17.0 back:
   `research/audio/data/handoff-20261021/rel-0.17.0-pico2w.uf2`.
2. **Earbuds test.** Josh's new earbuds with an inline mic arrived. Check:
   - the one-ear audio (his old cable was loose)
   - the mic level (currently +12 dB, `MIC_GAIN 4` in `pico/headset_mic_pico.c`)
3. **Mic into releases:**
   - make S2_HEADSET_MIC default ON (or a runtime setting)
   - remove or keep the S2_MOTION_DUMP debug code (it's `#if`-guarded)
   - update the README Headphones section
   - bump the version, then merge to main the way 0.17.0 was (fast-forward
     main, CI publishes)
   - mind CLAUDE.md's UI-text rules
4. Deferred (Josh said not now): switching audio output when the headset is
   plugged in or out, UAC2 jack detection, other USB modes.

## How to work with the hardware

- Josh can run commands with `!` at the start of a message (no space before
  it, or it arrives as text). For anything timed or long, running it
  yourself is usually easier.
- **SteamOS PC:** `ssh deck@steamos.local` (key auth). It was offline when
  this was written.
  - No root.
  - Install firmware through the dongle's update API: copy
    `fw_push.py`, `api_get.py`, `dump.py` (in
    `research/audio/data/handoff-20261021/`) together with
    `research/audio/dongle_log.py` and `research/audio/dongle_bootloader.py`
    into `/tmp/s2log/` on the deck. Then run `python3 -I fw_push.py FILE.uf2`.
    Afterwards Josh must reconnect the controller.
  - Read the log with `python3 -I dongle_log.py` (one-shot). The `--follow`
    mode crashes with EBUSY when anything else uses the interface.
  - Background processes over SSH die when the session ends. Use
    `systemd-run --user --unit=NAME …` (stop them with `systemctl --user stop`).
  - **Opening the mic remotely:**
    - write a silent WAV to `/tmp/s2log/silence.wav` (48 kHz stereo s16)
    - `systemd-run --user --unit=s2play pw-play --target alsa_output.usb-Switch2-Pico_Switch2-Pico_SInput_000000000001-02.analog-stereo /tmp/s2log/silence.wav`
    - `systemd-run --user --unit=s2rec pw-record --target alsa_loopback_device.alsa_input.usb-Switch2-Pico_Switch2-Pico_SInput_000000000001-02.mono-fallback /tmp/s2log/mic.wav`

    The mic needs the stream to the controller as well as a recording, and a
    headset plugged in.
  - Plugin zips: Josh installs them himself (Decky → Developer → Install from
    ZIP). The plugin folders are root-owned.
- **This laptop** (Fedora, MacBook): the dongle can be flashed with
  `handoff-20261021/flash.sh FILE.uf2` when it's plugged in here.
- **Build:**
  - `cmake -S . -B <dir> -DPICO_BOARD=pico2_w -DS2_HEADSET_MIC=ON [-DCMAKE_C_FLAGS="-mcpu=cortex-m33 -mthumb -march=armv8-m.main+fp+dsp -mfloat-abi=softfp -mcmse -DS2_MOTION_DUMP=1"]`,
    then `cmake --build <dir>`. The version comes from `VERSION` at
    configure time, so re-run cmake after a bump.
  - Node and pnpm aren't installed: plugin builds come from CI artifacts.
  - `gh` isn't installed: use the GitHub API with the token from
    `git credential fill`.
- **Laptop ↔ controller research scripts:** `research/audio/` (BlueZ raw ATT,
  `.venv/bin/python`). Run them under `research/audio/wifi-off.sh` (the
  MacBook's BT drops LE links on 2.4 GHz Wi-Fi).
  - A controller paired with the dongle won't accept the laptop. Pairing mode
    (hold sync) works without pairing, but the controller turned itself off
    when audio started.
  - With `--pair` (`motion_capture.py`) it works, after which Josh re-pairs it
    with the dongle (sync, with the dongle plugged in).
- **Data:** in `research/audio/data/` (gitignored):
  - `motion-20261021-*.raw` (laptop captures)
  - `handoff-20261021/motion/dongle-dump-20261021.txt` (9302 raw blocks from
    the dongle, all three layouts) with the analysis scripts next to it (`dd.py`
    loads it; `dec.py`/`full.py` decode it; `gt.py`/`search.py` match the
    layouts).

## Rules that still apply

- Only read-type controller commands. Never send:
  - flash write/erase (0x02/0x02, 0x02/0x03, 0x02/0x05)
  - 0x03/0x07-09, or 0x15/0x03 outside normal pairing
  - 0x0D/* (firmware update)
  - 0x06/0x02 or 0x06/0x03
  - 0x18/0x02 without a payload (it crashed the controller)

  Never write to service 00c5af5d, and never let the OS pair or bond (SMP).
- The flash dump is Nintendo's firmware: never commit or upload it.
- Nothing may permanently change the controller or upset the computer's
  Bluetooth or audio. Ask before installing system packages. No sudo.
- At the office, experiments must be silent (memory: quiet-office-tests).
- UI text: future tense for things that will happen. After a dongle restart or
  update the controller has to be reconnected.
- Commits: `git -c user.name=Claude -c user.email=noreply@anthropic.com commit`,
  ending with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Use they/them for Josh.


Latest test services: `s2play4`, `s2rec4`, `s2motion4` stopped after the
successful gyro retest. Silent full-duplex check without dump polling completed;
`s2play-clean` and `s2rec-clean` are stopped too.


Stereo test passed (Josh heard the quiet left/right tones in the correct
ears). Normal speech clipped with MIC_GAIN=4: speech-interval RMS -18.8
dBFS, 1,098 near-rail samples. Changed MIC_GAIN to 1; installed
`build/switch2_pico-motion4-unity-mic.uf2` for a repeat speech check.
Dongle restarted; Josh needs to reconnect the controller. All
speech-test streams have been stopped. Keep raw recordings local.


Unity-gain speech repeat completed: peak -11.03 dBFS, no clipping; active
speech about -29.29 dBFS RMS. `s2play-unity` and `s2rec-unity` stopped.
Current dongle: corrected motion + MIC_GAIN=1 test build. Gyro and stereo
hardware checks passed. Before enabling/releasing the mic, investigate mic
PCM-ring underruns (9 in a full 30 s window; Opus frames 0 bad/dropped,
headphone frames 0 missing). Tests remain 464 passing. No commits/releases.


Underrun investigation: exact-zero runs in unity WAV confirm real gaps
(11–29 ms), not just startup counters. Added arrival/decode max-gap,
inserted-silence-packet and overflow counters to existing 30 s mic log.
Diagnostic firmware `build/switch2_pico-mic-timing.uf2` is installed;
reconnect required, then run silent streams without raw motion polling
for two full 30 s windows. MIC_TARGET still 30 ms; hypothesis is BLE/
decode jitter exceeds available buffering. No buffer fix chosen yet.


## Latest interruption / host recovery (2026-10-22, about 23:23 EDT)

User returned after usage limit and reported Steam frozen. SteamOS SSH was
responsive; test streams had all stopped. Steam main thread waited in
`anon_pipe_read`, with a child shell waiting on PID 323523:
`wpctl settings monitor.alsa.enable-external-volume-control`, launched at
22:48:29 (just after USB firmware restart errors). Verified its exact argv
and sent SIGTERM to only this read-only query. Shell/helper exited and
Steam main thread resumed running immediately. Await user confirmation that
UI responds. No Steam/audio service restart or PC reboot performed.
PipeWire still lists other sound cards; dongle disappeared from its device
list after controller idled off. Do not assume PipeWire recovered.

Timing baseline DID run via direct ALSA, since PipeWire's dongle nodes were
missing and `wpctl status` hung. Direct ALSA commands work:
`aplay -q -D hw:CARD=SInput,DEV=0 /tmp/s2log/silence.wav` and
`arecord -q -D hw:CARD=SInput,DEV=0 -f S16_LE -r 48000 -c 1 -d 100 FILE.wav`.
Complete 30 s windows had frame-arrival/decode gaps about 48/47 ms with
1 underrun, then 45/45 ms with 2 underruns. A later partial window caught
105/104 ms and 7 underruns. No bad/dropped Opus frames, headphones no
missing frames. All these services stopped. Local baseline recording:
`/tmp/s2-mic-timing-alsa.wav`; remote `/tmp/s2log/mic-timing-alsa.wav`.

Changed MIC_RING_LEN from 4096 to 8192 and MIC_TARGET from 1440 (30 ms) to
5760 (120 ms), to bridge measured jitter (+90 ms nominal mic buffering).
Build in `build/motion4/switch2_pico.uf2` succeeds; 464 host tests pass.
**This larger-buffer build is NOT installed yet.** Current installed dongle
is `switch2_pico-mic-timing.uf2` (old 30 ms buffer, corrected motion, unity
gain). Controller idled off after 15 min; dongle is scanning/config-only.
Next: confirm host recovery, then install/test buffer change when ready.


Host recovery continued: user said UI still frozen. Steam had immediately
spawned another identical `wpctl settings monitor.alsa.enable-external-volume-control`
query (PID 337380, parent shell 337379), and blocked on it. User authorized
restarting WirePlumber and asked whether dongle use or testing caused this.
Restarted WirePlumber at 23:27:30: service active, but fresh `wpctl` calls
still hung, so this alone did not recover the host. Completing audio
recovery by restarting user PipeWire, pipewire-pulse, and WirePlumber.
Use `timeout -k 1 N` for wpctl: plain timeout may leave stuck clients alive.

Causal evidence: host kernel/SteamOS audio errors at 22:48:28 coincide
with the diagnostic firmware USB restart; Steam's first stuck wpctl query
started 22:48:29. This implicates re-enumeration during testing as a trigger
for this incident, but does NOT establish that ordinary playback is safe,
or whether firmware USB teardown, host audio handling, or their interaction
is the underlying defect. No watchdog reset in dongle log; controller later
idled off normally. Need treat host hot-unplug/restart robustness as an open
release blocker alongside microphone underruns.


Full user audio-stack restart at 23:29 restored fresh wpctl queries and
HDMI devices. User confirmed Steam UI returned, but Steam Controller
input needed manual controller power-off/reconnect. User noticed the stall
after reconnecting a Steam Controller; controller.txt places that event
at 23:15:35, after the first stuck audio query at 22:48:29. Correlation is
not full causation. Recovery tests must verify controller input too.

User now reports no HDMI audio. Read-only checks at 23:34: HDMI sink is
selected/default, volume 1.00, not muted; monitor present and ELD valid.
No sink-inputs/playback streams existed at that instant, sink SUSPENDED.
Asked which application and requested playback while watching routing.
Do not claim audio restored until actual user playback succeeds.

Local safety changes added (NOT flashed): platform_audio_active for USB
speaker/mic alt settings, API 409 guards for firmware writes/reboot/USB
settings changes while audio open; postpone automatic config-only USB
switch on controller loss while audio open; do not idle-disconnect a
controller used for audio; defer internal reboot/reconnect until streams
close. Build underway/passed compile steps; regression tests still needed.
Firmware test remains paused while host audio is being recovered.


## Prevention work and new baseline (2026-10-22, ~23:48 EDT)

User clarified the objective is preventing dongle-triggered audio loss and
Steam freezes, not further host recovery. Stopped recovery diagnostics.
User unplugged dongle, rebooted SteamOS and confirmed HDMI UI sounds and
Steam Controller input BOTH work. They then plugged dongle back in with
Switch 2 controller OFF. Host baseline: kernel
6.18.50-valve2-1-neptune-618-gc7289a96b14d, pipewire 1.6.8-1.101,
wireplumber 0.5.14-1.7, steam-jupiter-stable 1.0.0.85-9.2.

Firmware guards now implemented and built with mic ON and OFF. While USB
audio is open: block firmware writing and reboot/bootloader/factory/reconnect
API actions (409), reject USB-changing settings before saving, defer internal
reboot/reconnect, retain USB audio endpoints across BLE loss, and count audio
as activity for idle sleep. USB bus-reset callback clears stale codec stream
flags. Decky backend reports an explicit refused restart as failure.

New tracked research tools: `research/audio/audio_preflight.py`,
`research/audio/fw_push.py`. Preflight is read-only, refuses any non-closed
ALSA dongle PCM handle (including capture/prepared/suspended) and stalled
wpctl queries. Updater checks handles before each write/restart and health
before beginning and before restart. Bootloader helper also checks handles.
Do not use the old gitignored fw_push.py again. Copy new helper dependencies
together. No routing/service changes are performed to make checks pass.

Tests: 464 core assertions, 35 USB lifecycle assertions, 12 preflight tests,
1 Decky restart-refusal test all pass. New regression procedure:
`research/audio/REGRESSION.md` (requires both audible HDMI sounds and other
controller input after every USB transition; stop on first failure).

Artifacts:
- `build/switch2_pico-audio-lifecycle-mic.uf2` (mic ON, unity gain, 120 ms
  buffer, debug dump compiled but not polled), SHA256
  e755af71e39d30cfe455a2ff9b05014d0747e7dc4e9b7f24dfc6f12e70ef7780.
- `build/switch2_pico-audio-lifecycle-speaker.uf2` (mic OFF) also built.
- Main `build/motion4` is restored to MIC=ON.

Staged guarded updater/diagnostics/rollback in `/tmp/s2log/` on new boot.
Preflight passed: config-only dongle, no BLE controller, no open audio
handles, immediate wpctl response. Guarded firmware installation is in
progress (image CRC 0xc004d9e4). Next: verify audio_active field and host
health after update; user checks HDMI sounds and Steam Controller input
before powering Switch 2 controller on. Nothing committed/merged/released.


Guarded firmware installation completed. User confirmed HDMI sounds and
Steam Controller input survived both the quiescent update and normal Switch
2 controller connection. Current installed SHA256 is e755af71... (full hash
above), firmware still reports 0.17.0 plus new `audio_active` status field.

Hardware refusal probes PASS with playback only, mic only, and verified
full duplex: begin/chunk/end, USB-changing settings, reconnect and reboot
all 409; USB identity and settings unchanged; host settings queries healthy.
Factory and bootloader rejection are unit-tested only (avoid destructive
hardware probes). Host audio_preflight also correctly refuses open PCM.
User confirmed HDMI sounds/input still work with full duplex running.

Two probe corrections: same-value usb_interval is correctly a no-op (200),
so probe now requests a different valid interval; raw ALSA source node was
not linked by PipeWire policy, so recorder now uses the application-facing
`alsa_loopback_device.alsa_input.usb-Switch2-Pico_Switch2-Pico_SInput_000000000001-02.mono-fallback`.
Initial unlinked capture/full-duplex attempt is NOT counted. Both ALSA
pcm0p and pcm0c RUNNING were verified before counting the valid checks.

Long full-duplex run began 2026-10-22 23:55:21 EDT:
`s2life-play` (pw-play --raw /dev/zero, named headphone sink) and
`s2life-rec` (pw-record --channels 1, virtual mic source,
`/tmp/s2log/lifecycle-mic.wav`), each RuntimeMaxSec=1200. They are still
running; stop these units when finished. No raw motion polling. Mic clean
30 s windows show 0 under, 0 silent, 0 bad/dropped/overflow; headphones
have occasional single missing frames, which is a remaining quality issue
and must not be reported as a clean audio pass. Host has not stalled.
Need complete 10 min, save logs, then BLE loss/reconnect retention and
quiescent disconnect/reconnect with audible HDMI/input checkpoints.

Tests now 464 core +35 lifecycle assertions, 14 Python preflight/probe tests,
1 Decky refusal test. All pass. Both mic-ON and mic-OFF firmware builds
pass. No hardware-affecting code changes since guarded installation.


2026-10-23 follow-up: full duplex exceeded ten minutes (23:55:21 to
00:06 EDT); user confirmed HDMI Steam sounds and Valve Steam Controller
navigation still work. Steady-state mic counters were clean; occasional
headphone missing frames remain a quality failure.

BLE loss with both ALSA streams RUNNING retained USB address [1,9],
usb_mounted=true, audio_active=true, usb_config_only=false, link=scanning
beyond the disconnect grace period. User confirmed HDMI sound/input.
BLE reconnect returned link=ready on the SAME USB address [1,9]; user
again confirmed HDMI sound/input. Snapshots are /tmp/s2log/before-ble-loss.json,
after-ble-loss.json, after-ble-return.json on SteamOS. Both bounded test
services remain active; recorder expires about 00:14:47 and playback about
00:15:21 EDT. A Valve Steam Controller poweroff/reconnect checkpoint is
now pending, leaving Switch2 and both streams active.


2026-10-23 00:12 EDT: STOPPED regression sequence after FAILED Valve Steam
Controller reconnect checkpoint. User reports no navigation or touchpad
haptics initially after reconnect; Home still opened its menu and restored
haptics/navigation. Everything working again by report. Do not mark this
a pass or infer proven same root cause as prior persistent audio failure.
Controller log: disconnect 00:10:05, reconnect 00:10:19, mapping/context
activations 00:10:59–00:11:01. Kernel journal for event window has only new
virtual Xbox input device, no USB removal/reset. Dongle stayed [1,9], ready.
After recovery wpctl queries responsive, no stuck wpctl helper; Steam main
thread not stuck in anon_pipe_read. Cannot reconstruct transient blocking
state retrospectively. Stopped s2life-rec and s2life-play at 00:12:27; both
ALSA handles subsequently closed, audio_active=false. No Steam/audio service
restart, host settings changes, or firmware changes. Logs saved locally in
research/audio/data/handoff-20261021/reconnect-20261023/ (no recording copied).
Quiescent enumeration tests remain unperformed. Further work should isolate
Steam Controller reconnect behavior with/without dongle audio before claiming
prevention complete; do not immediately repeat failing hardware sequence.


Mic-only test completed: `/tmp/s2log/mic-only-buffered.wav`, mono PCM16
48 kHz, 97.365 s. Playback ALSA closed, capture RUNNING. Two complete
steady-state 30 s windows: 0 dropped/bad frames, 0 underruns, 0 silent USB
packets, 0 overflow. Largest complete-frame/decode gap in these windows
45.271/50.654 ms. WAV has zero runs >=5 ms only at startup: 0.000–0.011 s
and 0.013–1.053 s, then none. Peak -32.107 dBFS, no rail samples, ambient
only (not a spoken level/intelligibility test). Startup window has 1 under
and 1040 silent packets; exclude startup from steady-state claims. Initial
brief nonzero data before startup silence remains to inspect on reopen.
Stopped s2mic-only; no firmware modification or update during this test.
Freeze investigation remains PAUSED by explicit user request. Mic buffer
now has measured steady-state continuity in both full duplex and capture
only. Remaining: controlled speech/listening, measure latency (120 ms
nominal target), startup/reopen behavior, optional BLE session reset review.


### USB microphone integration (2026-10-23)

User reiterated primary goal: a mic input audio interface on the dongle.
Changed CMake S2_HEADSET_MIC default from OFF to ${S2_HEADSET_AUDIO}; normal
Pico2W audio builds now include capture, with explicit OFF opt-out. Updated
README with input selection, independent HDMI output, format and startup/
latency limits. No release/version bump/commit.
Fresh Release build `build/mic-default/switch2_pico.uf2` succeeds WITHOUT
explicit S2_HEADSET_MIC or S2_MOTION_DUMP. Uses local SDK/Opus and picotool
from motion4 build. Cache confirms audio=ON, mic=ON, no motion dump. Existing
464 core +35 lifecycle assertions, 14 preflight/probe tests, 1 Decky test
pass; git diff --check passes. New default-build artifact is NOT installed:
installed guarded test build already exposes and captures this interface;
updating would require manual controller reconnect while user is away.

Read installed raw USB descriptors from sysfs: AudioControl interface2;
playback interface3 alt1 EP0x05 adaptive OUT max196; microphone interface4
alt0 no endpoints, alt1 EP0x86 asynchronous IN max98. Mic format descriptor
is TypeI, one channel, two bytes/sample, 16 bits, fixed 48000 Hz. This
confirms actual host-visible capture interface, alongside working recording
tests. It is restricted to Pico2W SInput audio mode, as planned; no other
mode support added. Freeze investigation remains paused.


### UAC2 connector reporting (2026-10-23, in progress)

User explicitly authorized UAC2 jack reporting. New portable
core/src/usb_audio2.c/.h supplies UAC2 descriptors (217 B mic-on/125 B
speaker-only), read-only clock CUR/RANGE/validity, connector CUR, six-byte
interrupt messages. Playback adaptive OUT0x05, capture async IN0x86 retained;
separate clocks5/6, AC interrupt IN0x87. Board queues notifications from main
loop with busy/claim handling. Audio bcdDevice adds0x10 to distinguish old
cached UAC1 layout. Status API adds headset_jack bit0 headphone/bit1 mic.
Audio-in parser clears old headset bit before applying current jack bits
(fixes sticky presence after unplug during recording).

Initial installed image SHA256 5719cce656703c74d6f68d4baff8a648e0e2bdcf03d3deee43af25d8e73b18ad,
CRC0xf66bc8b4. Linux creates Headphone - Output Jack and Mic - Input Jack.
User connected headset: both on, USB [1,11]. User unplugged: both off;
PipeWire EnumRoute analog-input-mic and analog-output-headphones available=no,
same USB [1,11]. Replug: headphone=yes, mic=no until recording starts.
Thus read-only audio-in characteristic polling was inadequate for idle mic
availability (initial apparent success was from host capability probes).
Removed that new transport polling code entirely. Current fix uses existing
notification-mode switch briefly on headset insertion, observes one audio-in
jack report, then restores normal reports if host is not recording. Bounded
to1500ms on no response. No USB audio client/stream is opened for this probe.
Second image is being installed now, CRC0x5eef44be; user will need reconnect.

First image full duplex after recording began: steady 30s window 1500 frames,
0 missing headphone frames, 0 mic dropped/bad/under/silent/overflow, max
arrival/decode gaps71.871/79.498ms. Startup late-frame catch-up logged2249
frames (old inactive audio_due), not steady-state BLE loss. Streams stopped.
Artifacts remote /tmp/s2log/uac2-*.txt/.json; do not upload recordings.

Tests cover descriptors/controls for mic-on and speaker-only, independent
jack states, invalid controls, clock ranges, exact notification bytes; actual
link-state/probe unit checks cover no overlapping mode switches, probe end
after first report, timeout, real recording. Existing suite passes. Both
firmware variants build; rerunning speaker build for updated probe. No release.


UAC2 follow-up: second installed image SHA256
09e324d7dfdfa7f7169e744a3784bcb8958e08e1bd9dd12722dc42d0dfe330d0
(CRC0x5eef44be), USB [1,13]. Bounded idle notification probe WORKS: after
user unplugged/replugged with both ALSA endpoints CLOSED, PipeWire route
availability changed no→yes for BOTH headphone and mic. No USB re-enumeration.
Probe: headset insertion49.887s, mic-notification mode49.947→50.022s (~75ms).
The probe does not open a USB stream. Normal input is restored after one
valid audio-in report. All new and existing tests pass; mic-OFF build also
succeeds. There are no final changes to s2_transport.h or BTstack transport.

Active-stream unplug/replug also passed connector-state checks: same USB
[1,13] retained. On removal96.316s, host closed playback and capture in
~25ms; both jack controls off. Replug117.641s triggered brief probe, both
routes became available, capture automatically reopened117.766s. Existing
silent playback moved to HDMI on removal and did not automatically return;
this is host policy. Started a new named playback stream targeting headphones
for final steady-state duplex check. No audible speaker tones used.
Artifacts /tmp/s2log/uac2-v2-*-pw.json, uac2-active-*-pw.json,
uac2-probe-v2-log.txt, uac2-active-replugged-log.txt, uac2-usb-descriptors.txt.
Actual settings UI hiding not observed; verified underlying route availability.
Headphones-without-mic hardware test and Windows/macOS testing remain undone.


UAC2 completion checkpoint (2026-10-23): final full-duplex window
150–180s firmware uptime: headphones1500 sent/0 missing, 0 under/skip/over;
mic1500 frames/0 dropped/0 bad/0 under/0 silent/0 over, max arrival/decode
gaps60.042/61.864ms. Both ALSA directions verified RUNNING before counting
this window. User headset unplug/replug during recording correctly changed
jack states and resumed mic capture without USB re-enumeration. Final WAV
94.944s, mono48kHzPCM16, zero rail samples, peak-0.440dBFS (handling included;
not speech/headroom validation). No audio recording transferred.
Stopped s2jack-rec and s2jack-play-final. Final preflight: all dongle PCM
handles CLOSED, audio_active=false, host queries responsive; USB [1,13],
controller ready, headset remains plugged. No host audio/service/config
changes. No Valve Steam Controller reconnect tests (investigation paused).
Mic-on and mic-OFF firmware builds pass; full host suite passes and diff
whitespace clean. Current installed SHA25609e324d7... as above; no version
bump, commit, merge, or release. Local evidence directory:
research/audio/data/handoff-20261021/uac2-jack-20261023/ (gitignored).

Implemented/verified: USB Audio2 connector controls and interrupt updates;
SteamOS ALSA jack controls and PipeWire availability; idle and active headset
unplug/replug; same USB identity retained across jack changes. Unverified:
actual Steam settings UI hiding; headphone-only TRS hardware; Windows/macOS;
long-term stability. Previous startup/latency/quality and paused host-stall
limitations still apply. USB configuration-only enumeration on BLE loss when
no audio is open remains existing behavior; jack changes alone never request
USB reconnect. Generic ALSA policy also advertises an unknown S/PDIF route;
firmware has only the analog playback and capture streams.

Standards/host implementation references used for this change:
https://github.com/torvalds/linux/blob/master/sound/usb/mixer.c
https://github.com/torvalds/linux/blob/master/include/linux/usb/audio-v2.h
https://learn.microsoft.com/en-us/windows-hardware/drivers/audio/usb-2-0-audio-drivers
Local SDK TinyUSB src/class/audio/audio.h and src/device/usbd.h supply
matching UAC2 descriptor/control layouts.


### Mic test popups (2026-10-23)

User paused the validation checklist to request a five-second microphone
test in BOTH Decky and web config, matching research/IMG_9142.JPG–9144.JPG.
Inspected all three images. Implemented ready/live meter → Begin Recording
→ preparing (~600ms) → exactly5s recording → automatic headphone playback
with a playback meter → done. Cancel/Close/Escape cleanup and errors.

Architecture: dongle-local test, shared API /api/mic-test (GET status; POST
do=open|record|cancel with session=). No browser microphone permission,
voice transfer, or host routing/service changes. Preview opens controller mic
when popup opens, recording starts only on Begin. ~26KB compressed CELT RAM
buffer, erased on completion/cancel/error. Replay decoded on core1, duplicated
to stereo, half amplitude (-6dB) through existing encoder; ~300ms drain for
controller output buffer. Refuses another audio app, requires connected
headset mic/SInput, auto-aborts headset loss or5s without UI heartbeat.
Platform audio guard includes test-active; settings writes blocked during
test. The self-test exercises headset/BLE/codec playback, not host USB capture.

New core/src/mic_test.c/.h (supported implementation only when MIC=ON,
stubs otherwise); board decoder/encoder hooks; /api/settings mic_test
capability flag. New decky-plugin/src/mic-test.tsx + validated Python
mic_test RPC. Web native dialog in Headphones section, Decky Microphone
section. UI styled after supplied images; responsive and accessible meters.
Actual mic test has not yet been heard/verified on hardware: user has been
asked to reconnect and try the web popup, answer pending. Latest read-only
status confirms controller READY, headset_jack=3, audio_active=false,
/api/mic-test session0/idle. Do NOT start a recording without the user's
Begin action.

Validation: existing464 core/35 lifecycle assertions + UAC2/jack tests pass.
New portable test checks exact5s capture boundary, frame order/playback,
lease cleanup, unplug/USB conflict/no-frame errors, stale-session isolation.
New actual HTTP/WebUSB handler tests and three Decky RPC unit tests pass.
Mic-ON and mic-OFF Release builds succeed; Python compile and whitespace
check clean. Plugin TypeScript/build pass using @decky/ui4.12.1 (lockfile
version; npm initially pulled4.16, which removed pre-existing SideMenu export;
restored pinned UI without changing manifests). Local node_modules ignored.

Headless Chromium web tests (test/test_mic_ui.mjs) pass ready/record/playback
meters, Escape/cancel, late-open cleanup, errors, mobile fit. Mocks only;
no hardware voice recording. Run with PLAYWRIGHT_MODULE=/tmp/s2-ui-test/
node_modules/playwright/index.mjs, PLAYWRIGHT_BROWSERS_PATH=/tmp/s2-browsers.
Browser process needs sandbox escalation. Render screenshots in
/tmp/s2-mic-test-ui/{ready,recording,playback,mobile}.png (visually inspected
ready; screenshot matches reference layout).

Installed firmware build/mic-default/switch2_pico.uf2 SHA256
6ee1a156c1d0224692719ed108ed0a6a7345ccd27673951a380fa21b1f725eb1
CRC0x57008dd6, version still0.17.0. Host rebooted since previous session:
/tmp/s2log was absent; recreated helpers from TRACKED guarded scripts plus
api_get.py. Updater verified idle/healthy before installation. Current
image staged remotely /tmp/s2log/mic-test.uf2.

Built Decky ZIP decky-plugin/out/Switch2-Pico.zip, SHA256
de178fc61aef7b9cde8947fb263fc005b34a4cb9ce878a0f8385db10a72aaaca,
copied to SteamOS /tmp/s2log/Switch2-Pico-mic-test.zip. Plugin installation
is for user to do per existing handoff; NOT installed automatically. No
version bump/commit/merge/release. Reload web config to see firmware's
new button. Freeze investigation remains paused. No test services or
recorders were started in this turn, no recordings exported.


User reconnected headset/controller but needed help opening the new test.
Set up a temporary ZIP-only HTTP server on SteamOS localhost127.0.0.1:8765,
systemd user unit s2mic-plugin-package, RuntimeMaxSec1800. ZIP URL verified
SHA256de178fc6... matching built artifact. Served directory only contains
Switch2-Pico.zip, no recordings/logs. User instructions pending answer:
Decky Settings→Developer→Install Plugin from URL→
http://127.0.0.1:8765/Switch2-Pico.zip; then Switch2-Pico→Microphone→
Test Microphone. User installs the plugin, not agent. Once popup is open,
user will need Begin Recording to run the5s real test. Do not run a separate
automated mic recording behind that UI. Server expires automatically30min
from start; recreate if needed, stop when installation complete.


User confirmed mic-test Decky ZIP installed successfully. Explicit future
preference: COPY PLUGIN ZIP TO THEIR STEAMOS DOWNLOADS FOLDER, so they can
choose it in the installer. Do not set up an HTTP installation server next
time. User still installs the ZIP themself. Stopped s2mic-plugin-package
now that installation is complete. Real popup recording/playback test result
has not yet been reported; installation success is not audio test success.


### Mic test correction: native styling and provisional jack status

User clarified screenshot reference was FUNCTIONAL ONLY, not visual. Decky
should use standard Decky popups/components; web popup should match existing
page. Replaced copied dark/cyan card/icons/footer with native ModalRoot,
DialogButton and ProgressBar; normal title/body styles. Web dialog now uses
existing card/theme variables, normal buttons, page accent and labeled meter.
This preference overrides earlier instructions to match screenshot styling.

User reported test wrongly refused their connected Pro2 headset/SInput.
Read-only status confirmed ready Pro2/SInput/audio inactive, but cached
headset_jack=1 (headphones only). Log showed a brief controller audio/mic
startup then stop. The old test required jack=3 before opening and stopped
on initial headphone-only metadata. Fixed validation: connected headphone
jack can open preview; actual mic presence checked after preparing/settling,
up to3s, before recording starts. Loss of physical jack still stops test
immediately. Test does not record before Begin.

Jack observation now debounces candidate type for300ms after500ms warm-up.
Insertion probe holds a bounded silent controller audio stream so microphone
detection can settle even when USB endpoints are idle; max2s insertion
probe. Previously probe ended on first audio-in packet and could cache a
provisional headphones-only status. Existing confirmed mic status is kept
through transient startup metadata. New regression cases cover provisional
5→7 jack states, stable TRS, idle probing, delayed mic in preparing, actual
no-mic failure, and allowing API open when physical headset exists but mic
classification is not yet confirmed. Full tests pass.

Mic-on/off firmware builds pass; Decky typecheck/build pass; browser tests
pass after restyling, visually inspected ready popup matches page theme.
New firmware SHA25657098d5aa909b80bc2289455a3069714da899f11b3b70e005ddb3f44ff46c8d4
CRC0x67a8442f, updater currently running. Plugin ZIP SHA256
95b063d9d67c3f4d8cb588ed60a05ab3bf805905b6c23eb04bf3f033448e8c4f
copied as USER REQUESTED to /home/deck/Downloads/Switch2-Pico-mic-test.zip.
No install server. User installs ZIP via file picker, then reconnects Switch2
controller after firmware update and retries. Hardware success not yet
confirmed. No recordings initiated by agent. No commit/release/version bump.


### Mic test wording and automatic dismissal (2026-10-23)

USER CONFIRMED the corrected microphone test WORKS IN DECKY. Web popup has
not been hardware-tested by user. New request: match Switch screenshot TEXT
closely (not styling), automatically dismiss on completion, remove visible
Microphone level/Playback level labels and all elapsed/remaining time lines
in BOTH clients. Implemented exact ready instructions, Recording... /
Speak toward the mic. (Recording for five seconds.), Now playing back... /
Make sure you can hear the recorded voice audio. Native Decky/page styling
retained. Live meter retained with ARIA labels only. Both clients dismiss
on done before rendering a final state. Updated browser regression test
covers auto-dismiss and absence of visible label/time nodes. Passes.
Decky typecheck/build passes; new ZIP SHA256
9526c815636ec95bc19de258e5c75827441800c7ff2eac42302180fbf2901764
copied to /home/deck/Downloads/Switch2-Pico-mic-test.zip. User installs.

CAUGHT EXISTING WEB EMBEDDING BUG: weak const zero web_ui_size initializer
in same TU as router was constant-folded by Release compiler; linker then
discarded strong embedded data. Prior web changes were present in generated
C but NOT in firmware; prior firmware hashes did not change on HTML edits.
Informed user candidly. Moved weak defaults to core/src/web_ui_stub.c,
router uses extern declarations; added stub to CMake. Arduino core library
will pick up stub for non-web boards. New test/check_embedded_web.py checks
exact source-page bytes exist in final firmware binary; CI Pico build now
runs this check. Both mic-on and speaker-only final binaries pass; symbols
web_ui_data and web_ui_size now retained. Full existing/new host suite passes.
An initial same-image update in this turn completed before bug was found;
then installed corrected image (currently updater running):
UF2 SHA2564b80c251e03e7060650bed0afca7ef5484fdc67d8bcb64f42d1eac058fa6c55d
image965120bytes CRC0x79460880. Embedded HTML207776bytes SHA256
330b33d18ec96d31278bc2bd6eaacc6967cdf893767ce8df4051c624ec472cfe.
Next verify GET / served-body length/hash after update, then tell user new
ZIP in Downloads, controller needs reconnect, web page reload. No audio
engine changes this turn, no recordings started, no release/version bump.


Completion: corrected image installation finished. Read GET / from actual
dongle over WebUSB: status200,207776bytes, SHA256330b33d18ec96d31278bc2bd6eaacc6967cdf893767ce8df4051c624ec472cfe, EXACT source match. Verified served body contains new screenshot wording
and done→closeMicTest callback, no time/visible-level-label nodes. Web
embedding fix is confirmed on hardware, not merely in source/build output.
Updated Decky ZIP already in Downloads. User needs install replacementZIP
and reconnect Switch2 after dongle update. No new audio test requested or
started. Previous user-confirmed Decky audio success still applies; this
turn changes presentation/completion behavior only (plus web embedding fix).


User installed latestZIP but reported unchanged UI. Compared hashes on
SteamOS: DownloadsZIP9526c815... correct; ZIP JS and installed plugin
/home/deck/homebrew/plugins/Switch2-Pico/dist/index.js BOTH SHA256
ce0860c819993a3cc5fe40ff38da61ff64ca21257809b7027e06e12ee19c518d.
Both contain new ready/playback text and lack old visible labels/final
close-screen wording. Thus no file-copy or packaging mismatch.
Read-only CEF CDP inspection atlocalhost8080 (SharedJSContext) found
DeckyPluginLoader available. Used its existing importPlugin method to
reload ONLY Switch2-Pico, preserving actual installed version/loadType:
await importPlugin(p.name,p.version,p.loadType,true,10000). Completed with
loadedSwitch2-Pico/contentcomponentContent. No Steam/audio restart.
Likely stale frontend or already-open modal; user asked to close/reopen
test, no reinstallneeded. Exact reinstall-refresh root cause not proved
(performance resource list showed boot URL but may be buffer-limited).
No version bumps or code/package changes this turn. Helpers in
/tmp/s2log/cdp-plugin*.py. User confirmation after refresh still pending.


User confirmed refreshed popup LOOKS GOOD, requested final addition: an
audio symbol to left of meter as in screenshot, while retaining current
native styling. Added small themed22px line SVG microphone for ready/input
and speaker for playback in BOTH clients, left-aligned with live meter.
No visible labels/timers returned, auto-dismiss unchanged. Decky typecheck/
build and browser tests pass (new checks for left-of-meter positioning and
input/output symbol switch). Screenshot inspected. Embedded final binary
exact-page verification passes. No audio-engine changes.

New ZIP SHA256136f86788a9b19445dc6c398e677511664aab8807e3278d5e4805729f732caac
copied to /home/deck/Downloads/Switch2-Pico-mic-test.zip for USER install.
Firmware SHA256f4a029157b15b4533a4782641bb8b455cd69a9e6191cf8eb3920a9831fb8a4cf
image965888bytes CRC0x1acedf07, guarded updater currently running. Expected
HTML208463bytes SHA256191eeb211632f630888914d84021311f7251d0d1d3d4bd7ec0d6200b9c870976.
Plugin still0.6.0; Decky may need targeted frontend refresh after user
installs newZIP (prior turn documents safe existing importPlugin method).
No install server, no automatic plugin installation. Need controller manual
reconnect after firmware restart. No new recordings started.

Meter-symbol firmware installation finished; updated plugin ZIP is in Downloads. Browser and build validation passed. No audio test session is active or started by agent.


### Resumed mic validation: USB speech and Switch comparison

User resumed original mic validation after popup work; freeze investigation
remains paused. Explicitly ready for agreed USB speech recording. Recorded
via named PipeWire application-facing loopback, verified ALSA captureRUNNING
(card nowSInput/card0; do not assume oldcard2). Bounded s2usb-speech25s
completed: /tmp/s2log/usb-speech.wav, mono48kPCM16,25.173333s; peak-18.7477dBFS,
0 rail samples,62 active100ms windows RMS-32.5632dBFS, exactlyone>=5ms zero
run at0–1.055s startup, no later gaps. JSONusb-speech-measurements.json,
logusb-speech-log.txt remain onhost. No voice transferred.

Played derived stereo half-amplitude file usb-speech-headphones.wav directly
to plughw:CARD=SInput,DEV=0 withaplay (no HDMI fallback), boundedunit
s2usb-speech-play35s. User confirmed playback worked and voice sounds good,
but reported a lot of background noise. They compared sameheadset onactual
Switch2 and reported MUCH cleaner audio. This is a quality issue to investigate,
not a proven headset limitation. User suggested console-side noise reduction.
Code inspection: MIC_GAIN1, direct CELT decode toUSB ring, no noise reduction
or AGC. Plugin localtest also replays raw decoded mono throughstereo encoder.

Official Nintendo support confirms Microphone Noise Suppression toggle in
System Settings→Audio→Audio Input; input volume is a separate setting:
https://en-americas-support.nintendo.com/app/answers/detail/a_id/68202/p/1095/c/235
Asked user to repeat Switchmic test with noise suppressionOFF, sameheadset
selected and volumeunchanged, compare to dongle. Answer pending. Do not
attribute noise discrepancy conclusively toDSP until this controlled check.
No denoiser implemented or firmwarechange in this turn. Optional lightweight
noise reduction is a possible follow-up, but ifSwitch remainscleaner with
noise suppressionOFF, inspect controller setup/gain/otherprocessing first.
Also looked at SpeexDSPpreprocessor officialdocs: supports noise suppression,
AGC,VAD. Not downloaded/integrated/benchmarked; noalgorithm choice made.


Controlled Switch comparison CONFIRMED by user: Switch2 Microphone Noise
Suppression wasON. WithOFF, backgroundnoise sounds similar to dongle/PC.
WithON, quiet room levelmeter falls to0; withOFF, the sameambientnoise
shows lowmeterlevel. This supports missing console-side noise suppression
as the explanation for subjective noise difference, not a headset-quality
problem or demonstrated raw USB decode defect. User observation is not a
bitwise/gain-matched capturecomparison. No DSP added. If optionalnoise
suppression is added later, route processed audio consistently to USB capture
and self-test capture/playback/levelmeter; zeroing only meter or gate alone
is not equivalent to actual noise reduction during speech. Rawlevel/gain
currentlyunity; noaudio updates this turn. Continue startup/reopen/BLE
reconnect validation next, latency/hardwarecompat remain outstanding.
Controller may currently be connected to Switch2 for comparison; ask user
to reconnect to dongle withheadset before further USBtests.


### USB mic reopen regression (2026-10-23, in progress)

User reconnected to dongle to resume startup/BLE checks. Host USB1/25,
SInput is now ALSAcard0. No active test/PCM before probe. Ran5 bounded
direct hw:CARD=SInput,DEV=0 arecord captures (3s each,0.7s gaps), ambient
only, no voice playback. Script /tmp/s2log/mic-reopen.py, systemdunit
s2mic-reopen finished. Baseline /tmp/s2log/mic-reopen-before/ contains
trial1..5.wav andmeasurements.json (renamed before new firmware).
Trial1 initialzero1.039s. Trials2–5 contain2.000–2.042ms nonzero OLD audio
thenstartupzero0.558–1.037s. No rail samples or later>=5ms zero gaps.
This reproduces earlier brief stale data on USB capture reopen.

Code cause: set_mic_alt left mic_running=true onalt0; SOF refilled two
1ms hardware IN buffers with oldPCM and nextalt1 reused them. Changed
pico/usb_audio.c set_mic_alt: stop mic SOF first on every change; request
PCMflush BEFORE rearming; onalt0 clear both IN buffers; onalt1 reactivate
allocated endpoint and reinitialize both buffercontrol/nextindex before SOF
resumes. SDK iso_activate reuses allocated DPRAM (verified in dcd source).
Mic-on build and exact embedded-page check pass; whitespace clean. No
plugin/audio gain/DSP changes. Guarded updater currently installing image
965888bytes CRC0x2a939110, UF2SHA256
728c75ecd67d6c51e4bbafce786b95666be4e3daa647ed5ca3fd6ae715ee1caa.
After update user must reconnect, rerun same5-open probe. Do NOT call fix
validated until hardware recordings start with uninterrupted zero silence.
Then testBLE loss/return with directALSA capture held open (PipeWire policy
otherwise closes capture on jackunavailable); watch reconnect startup pop
because current decoder flush/skip only occurs on USB open, not new BLE
session. No BLE reconnect test performed yet in this resumed validation.
Recordings stay on host; no new audio exported. Freeze investigationpaused.


Post-fix5-open probe PASSED: currentinstalled728c75ec... USB1/27. Allfive
3s captures have uninterrupted exact-zero startup fromsample0 to liveaudio,
no old2ms packets, no rail samples or later>=5ms zero gaps. Startup timings
0.542,0.362,0.547,1.040,1.041s fromactualcapture samples. Files now
/tmp/s2log/mic-reopen/; baseline preservedmic-reopen-before/. Saved tested
UF2asbuild/switch2_pico-mic-reopen-fixed.uf2.

Started bounded directALSA capture s2mic-ble (180s arecord,185s service)
/tmp/s2log/mic-ble.wav to keep USB capture open regardless of PipeWire jack
policy. ALSA hw:CARD=SInput,DEV=0,mono48kPCM16, ownerpid56378, trigger
monotonic5752.760434056s, beforestatus1/27mounted/active/ready. User asked
to power off onlySwitch2 withheadsetleftin; answerpending. Then verify
sameUSB/ALSA RUNNING duringBLEloss, askreconnect, saveaudio/logs, stop
unitbeforeautomatic180sexpiry. Watch for full-scale startup pop or stale
decoder state because open USB stream currently doesn't reset decoder on
newBLEsession; do not assume reconnect clean from transportretention alone.


### Reconnect test exposed clock regression in first reopen fix

DirectALSA s2mic-ble kept sameUSB1/27 and ALSA RUNNING acrossoff/on; user
powered off then reconnected, no Valve SteamController manipulation.
Stopped capture manually at78.500333 recordedseconds. No clipping/pop:
overallpeak-29.8275dBFS; first1s afterresumepeak-38.5548dBFS, no rails.
Zero runs startup0–0.698s andoffinterval20.208792–50.378792 recordedtime.
HOWEVER recordedoff interval30.17s vsfirmwarearrival gap45.404s and
USB ringoverflow447105samples per30s afterreturn indicate timing failure,
so not a clean reconnect pass. All evidence /tmp/s2log/mic-ble*.json/txt/wav.

Timed comparison on first reset-basedfix: arecord8s took8.0739wallsecs
firstopen,11.8316wallsecs nextopen. A pw-record attempt recorded0samples
(unlinked/unavailable; NOT a capture-rate pass). Metadata
/tmp/s2log/mic-rate-reset/measurements.json, sourceaudiofiles. Initial
mic-rate.py had indentationerror; corrected locally/remote andsecondrun
producedthese actualresults. User informed firstfix introduced/interacted
with USB timing problem despite removingstalesamples.

Revised pico/usb_audio.c fix: RESTORE original continuously active endpoint
(bank selector untouched acrossalt0/1, activateonlyonce). Onalt0 overwrite
both queuedDPRAM PCM payloads withzeros without touching controlregisters;
mic_fill produces48sample zeros whilealt0 instead of draining PCM ring.
RequestPCMflush before arming firstopen. This eliminates staleaudio while
preserving endpointstate/clock that was stable in earlier longruns. Built
cleanly, exactembeddedpage checkpasses. Guarded updater installing revised
image965888bytes CRC0xc39a3d31, UF2SHA256
758911e87223b06f526261ab56b8c3812bb32021314dadc7775fb04340b0abbd.
DO NOT use earlier728c75ec reset-basedimage as final; revert to original
pre-fix f4a02915 if this revised queued-silence fix fails. Needuserreconnect
then measure BOTH startup samples and wallclock sample rate, not just WAV
length. Currenthandset may needreconnect after update. Freezeinvestigation
paused, micvalidation continues. No agentinitiatedvoice recording/playback;
ambientUSB probes only.


### Revised queued-silence fix still fails capture clock (Oct 23)

User reconnected. USB 1/29, ready, headset_jack=3, mic-test idle.
Bounded s2mic-revised ran five reopen captures then two timed ALSA captures
and one timed PipeWire capture. All five start with uninterrupted silence
(no stale 2ms packet), but 3s recordings take ~4.44s after first open.
Direct ALSA 8s takes 11.8291/11.8328 wall seconds; PipeWire 10.0035s
contains only 6.752s audio. Thus revised 758911e8 image FAILS timing too.
Firmware logs corroborate ring overflow (198428,370648,212741 samples per
report), not just app bookkeeping. No clipping or >=5ms later zero gaps.
Evidence /tmp/s2log/mic-reopen/ and mic-rate/, s2mic-revised journal.
Do not attribute slowdown exclusively to first reset-based fix without
comparison: now restoring preserved pre-reopen mic-test-symbols.uf2
(f4a029157b15b4533a4782641bb8b455cd69a9e6191cf8eb3920a9831fb8a4cf,
CRC0x1acedf07) via guarded updater. This keeps latest approved UI.
Source pico/usb_audio.c still contains revised queued-silence experiment;
installed earlier image will differ. Next: user reconnect, timed ALSA/PW
comparison on pre-reopen firmware FIRST. No BLE cycle until clock fixed.
Freeze investigation remains paused. User enabled --approve-for-me;
current automatic reviewer handles SSH sandbox requests. Physical
controller reconnection remains manual.


### Controlled comparison: pre-reopen firmware clock is healthy

After user reconnected, USB1/31 ready with mic-test idle.
s2mic-baseline-rate on preserved f4a02915 image: direct ALSA 8s captures
take8.02634 and8.02782 wall seconds; PipeWire10.00143s contains9.93067s
audio. Evidence preserved /tmp/s2log/mic-rate-baseline/. Therefore reopen
experiments are implicated; baseline timing passes both clients.
Narrowed experiment: mic_fill ALWAYS calls headset_mic_read as baseline
did, preserving closed-state packet-size/ring controller progression;
only zero the resulting payload when mic_alt!=1. Keep pending DPRAM
payload zero on alt0 and original continuous endpoint/bank control.
The prior conditional skipped headset_mic_read entirely while closed.
Exact causal timing mechanism is still unproven; do not claim root cause.
Built+embedded web checked, diff check clean. Guarded update installing
mic-reopen-silenced-payload.uf2 SHA256
9e9b23df7ee21f28f82fc20a044a94a7135238e9d84eacac7689d02fcd35da96
CRC0x86a735c4. Needs manual reconnect, then startup AND clock validation.
Current /tmp/s2log/mic-reopen still queued-silence results; preserve/rename
before rerun. Baseline fallback mic-test-symbols.uf2 retained.


### Payload-only experiment fails after repeated opens; bank race fix pending

User reconnected,USB1/33. s2mic-payload-check firstfive3s captures start
with clean silence0.558–0.578s and takeabout3.04s each afterfirst. However
subsequent8s captures take11.231/11.833 wallseconds; PipeWire10.002s yields
6.752s audio. This suggests transition into persistent bad bank state,
not simply fixedCPU overload. Results preserved mic-reopen-silenced-payload/
and mic-rate-silenced-payload/. Do NOT call9e9b23df validated.

Inspected USB driver: mic_fill read bankcontrol32 before PCM preparation,
then rewrites BOTH banks twice. Host may complete sibling IN bank during
preparation, and stale fullword stores can restore its AVAIL/FULL bits.
SDK usb_dpram.h explicitly documents8/16/32bit accesses. Changed ONLY
mic refill control to volatile16bit stores to selectedbank, preserving
ISO offset inbank1 and12cycle AVAIL delay; siblingbank untouched. Speaker
path unchanged. Rootcause hypothesis requires hardware confirmation.
New built+embeddedpagechecked UF2SHA
78d6ea50b2927c5162b6336e7760a6996482489b8ec157609702f6d5d065c573
CRC0x632fda76; guarded updater installing mic-reopen-bank-local.uf2.
Need manual reconnect then same5-open+timedALSA/PW probe, followed by
BLEretention ONLYifratepasses. Baseline f4a02915 remains availablefallback.


### Bank-local USB mic control passes startup and rate checks

Userreconnected,USB1/35. s2mic-bank-check on78d6ea50 image:
5opens all start with clean uninterrupted silence0.557,0.571,0.572,0.576,
0.567s; no clipping or later>=5ms zero gaps. Subsequent8s ALSA recordings
take8.0250 and8.02725wallseconds;10.0035s PipeWire produces9.94133saudio.
Results /tmp/s2log/mic-reopen/ andmic-rate/. This passes tests that prior
fullwordrefill variants failed, supporting siblingbank controlrace.
MicON+micOFF builds, embeddedweb anddiff checks pass.

Started s2mic-bank-ble, bounded240s directALSA capture,RuntimeMax245;
/tmp/s2log/mic-ble-bank.wav, owner86610, trigger9538.083941345 monotonic.
Before USB1/35 ready/active; captureRUNNING. Saved before.json/log.
Pendinguserpoweroff onlySwitch2controller, leaveheadset/dongle/Valvealone.
Thenverify sameUSB andcaptureRUNNING, askreconnect, measureWAVwalltime,
ringoverflow/startuppop/gaps. Do not mark BLE audio validated yet.


### 2026-10-23: USB mic startup, clock and BLE reconnect validated

This supersedes the failed reopen experiments above. Installed firmware:
`78d6ea50b2927c5162b6336e7760a6996482489b8ec157609702f6d5d065c573`
(UF2); a matching copy is `build/switch2_pico-mic-bank-validated.uf2`.
Do not use the earlier file named `switch2_pico-mic-reopen-fixed.uf2`:
that reset-based experiment failed timing despite passing startup checks.

The final change keeps the mic endpoint continuously armed, silences queued
payloads on close, and silences closed-state refills while preserving the
packet-size controller. Crucially, mic refills write only the selected
16-bit buffer-control bank. Whole-word writes previously risked restoring
stale AVAIL/FULL bits for the sibling bank if hardware completed it during
PCM preparation. Speaker control writes were not changed. The bank-local
fix passed the same checks that the preceding experiments failed.

Measured on SteamOS with the user's headset:

- Five successive USB capture opens began with uninterrupted silence
  (0.557–0.576 seconds), with no stale initial packets, clipping, or later
  exact-zero gaps of at least 5 ms.
- Two eight-second direct ALSA captures took 8.0250 and 8.0272 seconds.
  PipeWire captured 9.9413 seconds in 10.0035 seconds elapsed.
- Direct ALSA capture held open across controller power-off/reconnection
  retained USB address 1/35 and the same RUNNING capture process, PID86610.
  The final recording contains 155.375875 seconds in 155.391863 seconds
  elapsed. Silence was limited to startup (1.019 seconds) and controller
  loss (37.888 seconds), closely matching the firmware's 37.920-second
  mic-arrival gap. No other >=5ms zero gaps and no clipped samples.
- First second after audio resumed peaked at -41.24 dBFS (RMS -57.77 dBFS),
  with no large reconnect spike. This is waveform evidence, not a fresh
  listening test. Overall peak was -14.03 dBFS.
- The complete 30-second post-reconnect window decoded1500 frames with
  zero dropped/bad frames, underruns, silent packets or ring overflows.
  Ring stayed near its120ms target. Controller-silence periods and startup
  are expected; a missing headphone frame in an earlier window does not
  establish dropout-free headphone playback.

Stopped the bounded recording normally; dongle audio handles are closed,
link ready, and host audio queries respond. No playback, host audio/Steam
restart or Valve Steam Controller manipulation. Raw WAVs remain on SteamOS.
Numeric results and firmware log copied to
`research/audio/data/mic-bank-validation-20261023/` (local ignored evidence).
Microphone-on/off builds, exact embedded-web check, existing host suite
(464 core cases plus lifecycle/UAC2/jack/mic/API/Python tests) and whitespace
check pass. No commit, merge or release performed.

Remaining limits: end-to-end input latency has not been measured; only this
SteamOS/headset combination has been validated. Noise suppression is not
implemented; user previously confirmed raw input sounds like the Switch2
with its noise suppression disabled. The Steam Controller/freeze inquiry
remains PAUSED at the user's request, and these mic results do not resolve
it. Decky/web mic-test UI remains as approved; plugin ZIP is unchanged.


### User authorized commit and auto-update publication

Committed mic work as3eb57546a448dae39b6be44185b490bd52a00ca4
("Enable headset microphone input and add audio self-tests"). Firmware
version0.18.0 (VERSION + version.h), plugin0.7.0. Reconfigured/rebuilt
PicoRelease+exactembeddedpage check and plugin typecheck/build pass.
Authenticated GitHub default branch is claude/dazzling-wright-gxo6vj,
NOT a branch literallynamedmain. Pushed HEAD there by fastforward from
28df2a3; includes two existing mic-motion commits and newmiccommit.
Git identity absent locally; commit used authenticated GitHub user's name
and public canonical noreply email via command-only git -c (no configedit).
HANDOFF and user screenshots intentionally stay untracked/local. Numeric
audioevidence remainsignored; no rawrecordingspublished.
Auto-update workflow39986334583 started; monitor release/pages success
and manifests firmware0.18.0/plugin0.7.0. No host hardwareupdate performed
for versionbump; dongle still validated0.17.0 sourceimage78d6ea50.


### 2026-10-25: controller-only disconnect investigation

User reports intermittent drops with no headset/cable; Decky dongle reboot last night helped temporarily. Retained log captured two 0x08 Bluetooth supervision timeouts, second at stable -60 to -66 dBm; no definitive cause from timeout alone. Found Steam background capture open on dongle despite no audio use. It caused headphone silence traffic (~1500 frames/30s) and mic mode replacing normal input, even with empty jack.

Implemented unreleased fix: Pico audio activation requires physical headset presence (test tone override preserved); core mic_task also gates host request on per-link headset. USB capture remains open and returns silence with empty jack. Added regression checks for no-headset capture, unplug, reinsertion; README documents behavior. Preserved preexisting unrelated GameCube trigger calibration changes.

Installed build/disconnect-fix/switch2_pico.uf2, reports 0.18.0, SHA256 2fea4337be8033491bd771f3685f664f8c2d510d68ff1de2d4bede6506a3008b (local candidate differs from published 0.18.0). Guarded updater used. Only dongle card temporarily switched off; original output:iec958-stereo+input:mono-fallback restored and verified. First staging transfer interrupted by delayed config-only USB reenumeration after closing capture; retry succeeded. No host service restart.

478 core assertions + existing lifecycle/UAC2/jack/mic/API/Python suites pass; mic-on/off builds and embedded-web checks pass. User reconnected. Six-minute hardware monitor: 26,001 input reports, READY/USB mounted throughout, Steam capture open throughout, zero timeouts, no Bluetooth audio activation in complete firmware log. USB capture silence as intended, host audio query responds. All test processes finished. Evidence research/audio/data/disconnect-20261025/. Long-term intermittent timeout resolution remains unproven; headset regression only unit-tested, no new physical headset/audio recording test. No commit/push/publication. Prior freeze inquiry remains paused.
