# DAPA stick-turn test — MO2 test build

The installed rotation scale was zero. Saved snapshot 2026-09-05_18-46-29-091_13464 recorded a stationary actor and nearly full left turn input, with no rotational warp. Existing captures did not record actor yaw, so they cannot establish the exact correction angle or validate actor-heading alignment to the rendered frame.

## Local changes

- Separate actor-yaw predictor using real-pair XR timestamps, independent of walking velocity/history. Predict only during active turn input. Suppress onset/reversal confidence, stop on release, reject invalid timestamps, gaps and large snap deltas. Bound the predicted angle to 0.12 radians.
- Transform Skyrim world-Z turn into each cached eye's actual view basis rather than assuming screen-up equals world-up. Tracked headset orientation still belongs to the runtime; it is not extrapolated here.
- Rotation Scale and Warp Strength multiply the prediction. Existing zero remains off; use Rotation Scale 1.00 for the test. No global default or other settings changed.
- Capture actor yaw, angular rate, validity, input and confidence. Warp constants 66 and 67 now contain applied backward world yaw and turn confidence. Existing periodic motion log includes angular diagnostics.
- Clarify configurator help: Rotation Scale applies to stick turns, not physical head turns.

The boundary-repair shader, strafe predictor, DLSS path, foveation, submission cadence and recording gesture are unchanged. No additional production GPU passes, textures, readbacks or waits. Turning now activates the existing warp where it previously did nothing; total cost still needs measurement in game.

## Verification and remaining limits

CPU tests cover stationary turning at 60/72/80/90/96/100/120/144 Hz, actual fractional horizons, onset, release, reversal, snapping, wraparound, stale/missing/duplicate samples. D3D11 WARP tests check opposite turn directions, tilted/scaled view transforms, both handedness, asymmetric projections, reversed/standard depth, and existing strafe/foreground tests.

In-headset sign, actor-heading/render timing and visual quality remain unverified. The current gate uses the existing physical right-stick X action; alternative turn bindings are not newly supported. Prediction does not recover hidden background, independent NPC animation, or a full eye-pivot translation during rotation. Capture readback perturbs timing, so benchmark without recording. Existing turning captures have about 180 ms to the following real frame, not exact synthetic-time ground truth.

Deployment target: OCU - DAPA and Eye Tracking TEST. Skyrim and the configurator were closed before replacement. Previous runtime, configurator and INI are backed up under DAPA-Stick-Turn-20260905/MO2-before-turn in OCU Nexus Staging Archives. Rotation Scale is set to 1.00 for this test; all other INI settings are preserved. No Nexus deployment or remote publication.
