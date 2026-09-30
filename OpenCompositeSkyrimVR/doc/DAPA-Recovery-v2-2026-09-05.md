# DAPA recovery-v2 test build — 2026-09-05

## Evidence and scope

The last headset session logged successful synthetic `xrEndFrame` results (0)
followed by 11,380 ms backoff, repeatedly. A 15.7 ms successful end at 15:28:08
triggered that maximum hold. This confirms injection was occurring, but does not
establish displayed FPS or explain all underlying runtime/GPU latency.

This build separates successful-but-slow pacing from pipeline failure recovery.
It does not change DLSS 5, CSX shaders, foveation, warp math, or saved settings.
It is a test build, not a headset-validated performance fix.

## Changes

- Moderate slow ends require three consecutive samples before yielding. The
  threshold is the greater of the existing refresh-scaled setting and 1.25
  runtime display periods. Setting the end threshold to zero still disables
  end-pressure detection; multi-slot wait protection remains enabled.
- A wait or enabled end-pressure sample above 2.7 periods yields immediately.
- Pacing yields start at two display periods, escalate to 16 periods, and are
  capped at 250 ms. The current blocking call has already happened: this guard
  cannot shorten that call. Repeated substantial stalls can still reduce output.
- The existing longer backoff remains for pipeline failures (failed calls,
  unavailable warp output/invalid pose). Successful slow ends cannot enter it.
- Skipped injection clears cache demand. Re-entry requires a fresh stereo pair;
  no stale pair is reused. This avoids redundant cache copies during holds.
- Auto-mode idle estimates no longer reuse an old synthetic wait after a skip.
- Respect synthetic `shouldRender=false` by balancing begin/end with no layers.
  Do not end a frame when begin failed. Positive session-loss status is not
  counted as a healthy synthetic submission.
- Low-frequency status logs report accepted real/synthetic submissions, attempts,
  errors, empty frames, holds, runtime period, and CPU call durations. There are
  no added GPU queries, fences, sleeps, waits, or shader passes.

OpenXR timing reference: [xrWaitFrame](https://registry.khronos.org/OpenXR/specs/1.0/man/html/xrWaitFrame.html)
and [xrBeginFrame](https://registry.khronos.org/OpenXR/specs/1.0/man/html/xrBeginFrame.html).
The spec permits runtime feedback-driven pacing; CPU end-call duration alone is
not evidence that an accepted frame failed.

## Verification

- RelWithDebInfo runtime build: PASS.
- OCUDapaTimingTest: PASS at 60, 72, 80, 90, 96, 100, 120, 144 Hz. Covers isolated
  and sustained pressure, multi-slot stalls, bounded cooldown, recovery, actual
  observed end-time samples, and existing swapchain acquisition/timeout rules.
- OCUVRSGazeSelfTest: PASS.
- OCUDensityMaskD3D11Test: PASS, WARP and hardware.
- Headset presentation smoothness, GPU cost, latency and sustained output:
  NOT YET VERIFIED. Helper tests do not simulate a complete OpenXR runtime.

## Next headset test

Keep CSX DLSS 5 on and VD SSW off. Use the same scene/settings initially.
In OCUnleashedSKSE.log confirm `build=recovery-v2`, then inspect several
`DAPA STATUS` windows. At a sustainable half-rate, accepted real and synthetic
rates should be close. Those numbers are submissions, NOT proof of presentation.
Compare them with VD FPS/latency and the observed smoothness. If `pacing-yield`
persists or submissions remain slow, preserve the log for further diagnosis;
do not claim the underlying bottleneck is gone merely because backoff is shorter.

The current saved profile had translation=0, locomotion=0, rotation=0 and
debug mode=10. It therefore does not exercise positional/locomotion parallax;
runtime rotational reprojection can still occur. Debug mode 10 tints synthetic
frames. These settings were deliberately preserved, not silently retuned.

Deployment target: MO2 `OCU - DAPA and Eye Tracking TEST` only. Public/Nexus
staging is intentionally not advanced to this unverified recovery build.
