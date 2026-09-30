# DAPA motion v1 — local experiment, not a release

## Implemented

- Actor locomotion sampled from successfully cached real stereo pairs, using OpenXR predicted display timestamps. Prediction targets the actual synthetic display time, not an assumed half-frame fraction.
- No synthetic-to-synthetic feedback. Stops have no velocity decay. Startup, reversal/acceleration, duplicate/backwards timestamps, invalid values, gaps over 100 ms, teleports over 15 game units, and prediction horizons over one real interval or 50 ms suppress prediction.
- Separate per-eye game view/projection matrices. Projection is recovered from inverse(view) * unjitteredViewProjection. Raw depth reconstruction no longer guesses a reversed-Z near/far formula. Matrix inversion rejects invalid inputs.
- Actor XYZ is kept separate from headset position; the old actor XY / NiCamera Z mixture was removed. HMD translation remains a separate existing control. Its backward-lookup sign, view scale and forward-axis conversion now follow the cached pose and game projection.
- Synthetic colour lookup checks destination depth consistency and local depth discontinuities. Excessive displacement or out-of-bounds/invalid reconstruction backs off instead of dragging foreground colour. The existing warp dispatch is reused; no new full-screen pass, GPU readback, flush, motion-vector copy or neural evaluation was added. Shader work per active pixel increased and must be timed on hardware.
- Optional unwarped runtime depth attachment is omitted for translated synthetic colour; it would not represent that output. The shader still uses depth. DAPA stays enabled.
- Existing stick-yaw control retained with timestamp scaling and no lingering release decay. Runtime ATW remains responsible for head rotation.
- DAPA MOTION v1 diagnostics every two seconds report projection validity, real interval, prediction horizon, confidence and actual applied translation.

## Deliberately NOT implemented / limitations

Object-motion-vector prediction remains OFF. The bridge exposes a resource generation but does not certify alignment of object vectors, depth, jitter and CSX's final neural colour for each displayed real frame. Enabling that path without validation would reproduce the untrusted-vector problem. No claim is made that running NPCs, animation, transparencies, particles or disocclusions are solved. A camera-only warp may disturb an NPC moving alongside the player; test that separately.

The game projection used here is unjittered. Live CSX depth/colour registration and remaining subpixel jitter must be checked in the headset. Automated synthetic scenes do not establish the correctness of live renderer offsets or end-to-end VR timing.

This does not fix or diagnose Virtual Desktop encoding/networking storms. DLSS 5, DLSS upscaling, foveation, render resolution, bitrate and SSW settings were not changed.

## Automated verification

Build: cmake --build build --config RelWithDebInfo --target OCOVR OCUDapaMotionTest OCUDapaTimingTest --parallel 8

Run build/tests/RelWithDebInfo/OCUDapaMotionTest.exe. It compiles and executes the exact production warp shader using D3D11 WARP (CPU software device). Cases cover standard/reversed Z, left/right handedness, asymmetric eye projections, full/half-resolution depth, identity, both strafe directions, forward movement, foreground/background rejection, reversed-V bounds and invalid geometry. CPU cases cover timing at 60/72/80/90/96/100/120/144 Hz, stop/reversal/teleport/stale/invalid history and scaled projection math.

OCUDapaTimingTest checks the unchanged recovery-v2 timing/ownership guards. OCUVRSGazeSelfTest checks the existing gaze math. These are not headset visual validation or hardware performance measurements.

## Headset A/B test (pending)

1. Exit Skyrim before deployment. Preserve the current MO2 DLL and INI. Only use the existing OCU - DAPA and Eye Tracking TEST mod. No Nexus release update.
2. Start with the existing settings: aswLocoScale=0, aswTranslationScale=0, aswRotationScale=0, aswDebugMode=10. This is the no-movement-correction baseline.
3. For the new camera-prediction comparison, change ONLY aswLocoScale to 1.00. Keep the red synthetic-frame tint initially. Do not enable a motion-vector/upscaler toggle to test this path.
4. Check DAPA MOTION v1 geometry=1/1; stable movement should show a plausible interval/horizon, confidence near 1 and nonzero translation. Static, missing geometry and rejected history must not retain motion.
5. Strafe both directions past a stationary post and NPC. Compare forward/backward motion; stop abruptly and reverse. Check each eye, near/far geometry, a running NPC and walking alongside a matched-speed NPC. Test a loading transition.
6. Compare timing and VD overlay in the same location. Record whether separation improves without a new forward-motion defect or meaningful latency regression. Do not interpret accepted submissions as presented headset FPS.
7. Setting aswLocoScale back to 0 disables actor prediction for a live comparison. A full rollback requires exiting the game and restoring the saved DLL/INI. Turning off the tint is not a motion fix.

No headset pass or deployment is claimed by this document. See the conversation for actual deployment status.
