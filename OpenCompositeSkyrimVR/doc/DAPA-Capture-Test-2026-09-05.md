# DAPA desktop capture test — 2026-09-05

Local diagnostic build for the MO2 mod `OCU - DAPA and Eye Tracking TEST`.
This captures real and reprojected images for inspection outside the headset.
It does not change DLSS 5, foveation, bitrate, locomotion settings, or the in-game debug tint.

## Run a test

1. Start Skyrim VR through MO2 with the test mod enabled. A new DLL requires a game restart.
2. Stand near a stationary NPC with a building and vegetation visible.
3. Release both grips, then squeeze **both grips together** to START a photo session. **One pulse** acknowledges start. The overlap needs only 0.15 seconds; it is not a double-tap. Normal grip actions are not consumed.
4. Release both grips and strafe steadily. Squeeze both together again to STOP. **Two separated pulses** acknowledge stop/saving. Do not open a menu or cross a loading screen during a sample.
5. Keep the headset rendering briefly so an in-flight sample can finish, then remove it. In `DAPA Capture.exe`, click **Open latest comparison**, or **Open capture folder / PNG photos**. The session index links each saved sample. PNG writing can take longer than the sample itself.
6. Repeat separately for walking forward, standing still, and moving alongside a running NPC.

Output: the current Windows user's Documents folder, under `My Games\Skyrim VR\DAPA Captures`. Nothing is uploaded.

Sessions collect spaced snapshots, not video: one sample at a time, with a five-second gap after saving before another starts. Movement recording continues until both grips stop it or the 120-second safety limit. Photo scheduling pauses after eight attempts or three failures, but movement recording continues. Three separated pulses mean unavailable/photo failure limit. An in-flight sample may finish after stop. A new session cannot start while the previous sample is still saving. No repeated toggle while grips are held; both must be released to rearm. Inactive/stale input is ignored. Haptic feedback follows the configured haptic strength and runtime support.

The keyboard shortcut **Ctrl + Shift + F10** and desktop **Capture in 3 seconds** button remain available for a single delayed sample outside a grip session. The grip gesture toggles capture only, not DAPA or DLSS 5.

## What the photos show

- Original real frame, clean DAPA prediction, and the next real frame, separately for each eye.
- Original/prediction and next/prediction overlay PNGs; difference PNGs.
- Interactive report with cyan/magenta edge comparison, opacity, eye selection, side-by-side, depth, and confidence views.
- Raw float depth plus exact timestamps, projection constants, poses, and submission result in metadata.

The saved prediction is captured before the optional red debug tint. The in-game tint remains unchanged. The confidence mask shows where the warp is accepted or rejected; if prediction is inactive, red does not itself prove bad depth.

The next real frame is at a later time, **not exact ground truth for the predicted instant**. Compare motion direction and silhouettes with the recorded timing. These are pre-runtime images: they cannot directly show Virtual Desktop encoding, headset reprojection, or which submitted frame was actually displayed.

## Cost and safety

Bounded photo sessions or single shots only. Diagnostic shader bytecode is compiled once on a CPU worker; pending compilation skips capture without blocking the normal game warp. The device shader is reused across samples. Capture still temporarily allocates/copies textures and reads back one image per render tick without a blocking GPU wait. PNG/report writing runs on a CPU worker; no sample queue or continuous video recording. Capturing can still hitch: do not use capture-frame timing as normal FPS or encoder performance evidence.

The temporary texture budget is 512 MiB, not a cap on total CPU memory. Larger captures are rejected. Repeated requests while busy are ignored. Invalidated history or mismatched stereo timing aborts the sample rather than mixing frames. Check `DAPA CAPTURE` entries in `OCUnleashedSKSE.log` if no report is produced.

## Verification

Built successfully in RelWithDebInfo. D3D11 WARP software-device test passed actual shader dispatch, stereo capture, clean output with the game tint retained, PNG decoding, exact timestamp serialization, next-real capture, and invalidation. Motion, timing (60–144 Hz cases), and VRS gaze regression tests passed. Desktop helper bounds self-test passed.

Synthetic report was visually checked in a local browser server. Direct file-browser execution was not verified by automation. The full-resolution PNGs are independently usable without the report.

Actual headset/game capture remains to be tested. This build adds diagnosis, not a confirmed NPC ghosting fix.

## Independent movement telemetry

New session folders end in `movement_session`. Each snapshot records raw current/previous actor positions, CPU sampling and XR display timestamps, unclamped displacement-derived velocity, predictor velocity/confidence separately, physical left/right stick axes with activity and age, and the captured game view matrices. The next real frame has its own movement record.

Speed uses Skyrim world units per CPU wall-clock second, not an assumed conversion to metres. A second velocity based on XR display-time intervals is retained for comparison. Camera-relative direction uses normalized view axes and the projection's forward convention. The viewer labels valid speed below 0.1 units/s as PLAYER STATIONARY. Initial, missing, stale or discontinuous samples are unavailable, not stationary. Head/eye pose change is shown separately. These samples cover successfully cached real stereo pairs, not every physics update; capture hitches affect sampling intervals.

Sessions also save `movement-timeline.csv` and full `movement-timeline.json`, capped at 16384 records with a dropped-record count. Timeline collection stops when the user stops capture or at the safety limit; an in-flight photo can still finish. Raw sticks are before game remapping and do not prove movement: collisions, room-scale movement and alternate input mechanisms can differ.

Record stationary-player tests too: (1) stationary scene/head, (2) walking NPC with head steady, (3) head moving while actor remains stationary. Record forward, backward, left strafe, right strafe, and matching-speed NPC tests separately. There is no per-NPC/object identity or independent object-motion telemetry yet. Image differences alone cannot establish it.

Grip-session regression tests cover initial held inputs, one-hand grips/bounce, overlap debounce, release-to-rearm, focus loss, stale input, atomic input-to-render mailbox, session stop during readback, restart while saving, early stop, session index, and automatic limit policy. Actual controller/haptic behavior still needs the in-game test.
