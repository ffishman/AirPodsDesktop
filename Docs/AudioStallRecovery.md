# Silent audio recovery (audio stall recovery)

Working notes for the `feat/audio-stall-recovery` branch. Kept in the branch so the design and the
field evidence travel with the code.

## Problem

Windows keeps a Bluetooth headset (observed with AirPods Pro 3, product id `0x2027`) connected and
selected as the default output, but no sound reaches it. Everything looks healthy:

- the A2DP endpoint ("Headphones", not "Hands-Free") is the default for both console and
  communications roles;
- it is not muted and the volume is non-zero;
- the Bluetooth device reports connected, and the services (`bthserv`, `BTAGService`, `Audiosrv`,
  `AudioEndpointBuilder`) are running.

Yet the endpoint peak meter stays at zero while a system sound is playing. Turning the device off
and on in the Bluetooth settings fixes it immediately (peak 0.60 during the same test sound
afterwards). Most likely cause: the AirPods were taken over by another Apple device of the same
iCloud account (automatic switching), or the A2DP stream failed to restart after a suspend. Windows
never notices either case.

Quick connect does not help here: it only reconnects devices whose endpoint is not active, and in
this failure the endpoint is active.

## Design

| Piece | File | Layer |
|---|---|---|
| Detector and recovery budget | `Source/Core/AudioStallDetector.*` | `apd_domain` (pure C++) |
| Controller, worker thread, modes | `Source/Core/AudioStall.*` | `apd_core` |
| Windows backend (sessions, meter, probe, KS) | `Source/Core/AudioStall_win.*` | `apd_core`, Windows only |
| Shared MMDevice/KS helpers (extracted from quick connect) | `Source/Core/BtAudio_win.*` | `apd_core`, Windows only |
| Setting `audio_stall_recovery` (`Disable` / `Observe` / `Recover`) | `Source/Core/Settings.*` | `apd_core` |
| Settings card, tray notices | `Source/Gui/SettingsWindow.*`, `Source/Gui/TrayIcon.*` | `apd_gui` |
| Tests | `Tests/AudioStallTests.cpp` | |

### Evidence

Every 500 ms, on the default multimedia render endpoint, and only when it is a Bluetooth endpoint:

1. **Sessions**: is any other process's audio session active? Sessions of this process (the probe
   and the low audio latency player) are ignored.
2. **Probe** (strong evidence): while sessions are active, a shared-mode `IAudioClient` stream of our
   own is kept open on the endpoint, filled with silence (`AUDCLNT_BUFFERFLAGS_SILENT`,
   `AUDCLNT_STREAMFLAGS_NOPERSIST`, 1 s buffer). If the engine pumps the endpoint, the probe's
   `IAudioClock` position advances between polls. If the stream is stuck, it stands still. The probe
   is closed as soon as no other session is active, so it never keeps the headset awake on its own.
3. **Peak meter** (weak evidence, log only): a zero peak with active sessions is normal for a paused
   video or a muted call, so it never triggers recovery. It is logged once per episode so the field
   log shows whether a real stall was also visible there, and whether the probe missed one.

A Bluetooth endpoint is recognized by the device instance id stored on the endpoint
(`{b3f8fa53-0004-438e-9003-51a46e139bfc},39`, `BTHENUM\...` for A2DP or `BTHHFENUM\...` for
hands-free), with the KS filter id as a fallback. The filter id alone is not enough: with Bluetooth
audio offload (seen with Intel Smart Sound Technology, filter `intelaudio#...\intcbttoporender_...`)
the filter belongs to the platform audio driver. That driver still implements
`KSPROPSETID_BtAudio`: `BASICSUPPORT` queries for `ONESHOT_RECONNECT` and `ONESHOT_DISCONNECT`
return `KSPROPERTY_TYPE_GET` support, so recovery works the same way.

A stall is declared after the probe stands still for 5 s with sessions active.

### Recovery

`Recover` mode sends `KSPROPERTY_ONESHOT_DISCONNECT` to every active endpoint of the device's
container, waits 2 s, then `KSPROPERTY_ONESHOT_RECONNECT` to all of them. These are the requests the
Settings app sends for Disconnect/Connect, and they need no elevation.

Rate limit: at least 30 s between attempts, at most 3 attempts per 10 minutes, then a single
"giving up" notice. A recovery counts as verified when the probe advances again within 20 s.

### Modes

- **Off**: nothing runs.
- **Observe only**: detect and log, plus one tray notice per stall. Never touches the connection.
  This is the mode for the first days of field testing, to look for false positives.
- **Automatic**: detect and recover.

In Observe only and Automatic, a quick connect click on the tray icon while a stall is detected
disconnects and reconnects the device instead of reporting "already connected". A manual request
skips the automatic rate limit.

## Field testing

1. Install the CI build, set **Silent audio recovery** to **Observe only**.
2. Keep **Low audio latency** off during the test, otherwise it is impossible to tell which of the
   two avoided a stall.
3. Use the PC normally. When the audio goes silent, check the log (Settings > About > open logs
   directory) before reconnecting by hand. The relevant lines start with `AudioStall:`.
4. What to look for:
   - `stall detected` at the time of the real stall: the probe works.
   - `endpoint peak stayed at zero ... probe=advancing` during a real stall: the probe misses this
     failure mode and the design needs another signal.
   - `stall detected` while sound was actually playing: false positive.
   - `probe ... failed`: the probe cannot open a stream on the stuck endpoint.
5. Once Observe only has been clean for a while, switch to **Automatic**.

Run with `--trace` to also get one log line per poll.

## Upstream plan

After field testing: open a feature request upstream with the evidence above and the log excerpts,
then a PR from this branch. Commits are already split as `refactor:` (helper extraction) and
`feat:` (the feature).
