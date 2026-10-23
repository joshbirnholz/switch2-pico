# USB audio lifecycle regression (2026-10-22)

This is a release blocker, not just a microphone quality check. Passing a
PCM/Opus counter check does not establish that Steam or other sound devices
survived a USB transition.

## Evidence and limits

At 22:48:28 a test-firmware restart removed Switch2-Pico while host audio
was active. ALSA/PipeWire logged device-loss errors. At 22:48:29 Steam
launched a `wpctl settings monitor.alsa.enable-external-volume-control`
query that never completed. Steam's main thread blocked waiting for it.
The Steam Controller reconnected at 23:15:35, after this query had stalled.
Restarting the whole user audio stack later restored the query and Steam
UI, but controller input needed a reconnect and UI sounds stayed silent.

This implicates the USB/audio transition as a trigger for this incident;
it does not identify the underlying host/firmware defect or rule out a
failure during normal use. `-19` device-loss messages alone do not prove
malformed USB audio. Physical unplug, power loss, or watchdog reset can
still remove an active device; firmware guards cannot prevent those.

## Preconditions

1. Start with the dongle unplugged and a clean SteamOS session. Verify
   HDMI Steam interface sounds and Steam Controller navigation by hand.
   Do not continue if either is already broken.
2. Record the firmware hash, host package/kernel versions and start time.
   Save kernel, WirePlumber, PipeWire, Steam controller and CEF logs locally.
   Do not upload recordings or unrelated user logs.
3. Use `audio_preflight.py` and the tracked `fw_push.py` from this directory
   (copy them with `dongle_bootloader.py` to the same directory on SteamOS).
   Run with `python3 -I`. The preflight refuses open dongle ALSA handles and
   stalled host audio queries; it never changes the computer's configuration.
   Recheck handles before each firmware write and before restarting.
4. Close all dongle playback/capture clients, including voice settings and
   volume panels that keep the mic open. Stop only our test services.
   Do not restart audio services to make a preflight pass: a failed preflight
   is a failed readiness check to investigate.
5. Stage the rollback image before starting. Install the guarded build
   only after preflight passes. Confirm `/api/status` includes `audio_active`
   before exercising any requests expected to be rejected.

## Test sequence

Use named, bounded test services. Playback is silent or restricted to the
headphones; do not generate speaker test tones. Keep raw `/api/motion`
polling off during audio stability checks.

| Case | Expected dongle behavior | Host acceptance |
|---|---|---|
| Idle, playback only, capture only, full duplex | `audio_active` matches open interfaces | Audio queries complete within 5 s; Steam UI/input work |
| Firmware begin/chunk/end while audio open | 409; no flash operation | Same USB device stays present; sound/input continue |
| Restart/bootloader/factory/USB reconnect while audio open | 409; no settings reset or USB removal | Same USB device stays present; sound/input continue |
| USB-changing settings while audio open | 409; settings unchanged | Same USB device stays present; sound/input continue |
| Controller loses BLE connection with audio open | Audio USB function stays present; gamepad releases inputs; mic becomes silent | No unrelated audio loss; Steam remains responsive |
| Controller returns while audio remains open | Resume input/audio on the existing USB device | No re-enumeration; Steam Controller still works too |
| Controller idle timeout with audio open | Controller remains awake | Audio and controls continue |
| Close audio after controller loss | Normal 5 s grace period, then configuration-only USB | Host handles removal; HDMI sounds and other controller still work |
| Reconnect controller with streams closed | Normal USB enumeration | Sound/input checks pass afterwards |
| Quiescent firmware update | Guarded updater completes, controller is reconnected manually | Sound/input checks pass afterwards |
| 10 min full duplex, no USB changes | No reset, no steady-state mic silence/overrun or missing headphone frames | Host remains responsive; UI sound/input checked at end |

Check HDMI interface sounds and Steam Controller navigation after **each**
USB transition, not just at the end. A successful HTTP reply, `wpctl status`,
or moving gyro icon is insufficient on its own. Record endpoint/frame
counters separately from host acceptance.

Stop the sequence at the first host stall, audio loss, input loss, watchdog
reset, or timed-out query. Save evidence before recovery. Do not repeatedly
disconnect/reconnect or force active-audio USB removal to try to recover.
The known active-device removal reproduction requires a separate controlled
session with user work saved; the guards have not proved that path safe.

## Current state

Local lifecycle guards and host preflight tests pass. On the guarded build,
hardware rejection checks passed with playback only, capture only and
full duplex. A ten-minute full-duplex run preserved HDMI sounds and Steam
Controller input; steady-state mic counters were clean, but occasional
missing headphone frames remain a quality failure. BLE disconnect/reconnect
with streams open retained the same USB address [1,9] and passed user
sound/input checkpoints. Quiescent controller-loss enumeration and the
separate Steam Controller reconnect checkpoint remain pending.
The microphone remains disabled by default. Do not merge/release on the
basis of the earlier gyro and stereo checks alone.


## Related upstream reports (context, not an attribution)

- https://github.com/ValveSoftware/steam-for-linux/issues/13619 reports
  Steam's main thread blocked on a non-returning wpctl settings query,
  with a different trigger (incomplete PipeWire configuration). This
  corroborates the freeze-propagation mechanism, not our USB trigger.
- https://github.com/ValveSoftware/steam-runtime/issues/853 reports client
  format-renegotiation crashes/deadlocks and a wedged host graph, involving
  the older bundled PipeWire client and Bluetooth headset profile changes.
  Its host PipeWire/WirePlumber versions resemble this PC's, but its trigger
  and client stack differ. It has not been established as this incident's
  root cause; no runtime or host configuration changes were made.


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


2026-10-23: At user request the freeze investigation is PAUSED. Separate
UAC2 jack detection work was implemented and installed. Idle and active
headset unplug/replug update ALSA jack controls and PipeWire route availability
without USB re-enumeration. This is NOT a resolution of the Steam Controller
reconnect stall. See latest HANDOFF/headset-audio notes and local evidence
research/audio/data/handoff-20261021/uac2-jack-20261023/.


## Microphone capture timing acceptance (2026-10-23)

Mic input is now enabled by default in audio-capable builds; the earlier
status above describes the lifecycle investigation at that time. That
investigation remains paused at the user's request.

For microphone startup or endpoint changes, check elapsed wall time as well
as WAV duration. An eight-second `arecord -d 8` file is always eight seconds
long even when packets arrive too slowly. Use a bounded direct ALSA capture
and a timed PipeWire capture, sequentially with other mic apps closed.
Record both wall time and sample count, then inspect initial samples and
zero gaps. Repeated openings must preserve the nominal capture rate, with
no stale audio before the startup silence.

Observed failing reopen experiments produced eight-second files in about
11.83 seconds, or only6.75 seconds of audio during ten seconds of PipeWire
recording. These are failures even though speech remains audible and
recordings have no clipping. The preserved pre-reopen image f4a02915 passed
the same timing probe (8.026/8.028 seconds, and9.931 seconds of PipeWire
audio in10.001 seconds). Further fixes require the same comparison before
controller loss/return testing. See HANDOFF for exact images and results.


2026-10-23: Final bank-local mic control fix78d6ea50 passed five opens,
timed ALSA/PipeWire captures and a155.38s held-open capture across BLE
loss/return. Same USB1/35 and capture process retained; no clipping,
reconnect spike, unexpected >=5ms zero gaps, or post-reconnect buffer
overflows. Details and scope limits in HANDOFF/headset-audio. This is a
microphone validation result only; the freeze investigation stays paused.
