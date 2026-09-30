# DAPA source-depth boundary repair v2 — local test build

## Change

The previous shader blended sampling coordinates between the unchanged pixel and a displaced coordinate using depth confidence. This could leave the original silhouette behind while shifting its interior. The replacement solves for a source coordinate using the source pixel's depth and forward projection. It checks nearer coverage along displacement so it does not merely erode the moving foreground. An unresolved depth-edge lookup selects a sampled background candidate rather than retaining old foreground color.

This remains a bounded, single-dispatch inverse warp: at most four primary iterations, two probes, and three refinement iterations per probe. No new production textures, readbacks, queued frames or GPU waits. The old shader is retained only as an offline test fixture, not as a production branch.

Player prediction, DLSS, foveation, bitrate, eye poses/submission and input behavior are unchanged. Both-grips recording and movement telemetry remain available. The near-depth fade and depth multiplier are retained. The legacy `aswEdgeFadeWidth` value is preserved in config/capture layout but no longer drives the removed UV confidence fade; do not tune it to evaluate v2.

## Evidence

- Hardware replay of all 14 recorded stereo samples exactly reproduced the old prediction RGB (zero difference, both eyes). This validates replay inputs, not the correctness of those predictions.
- The candidate preserved all seven inactive/standing snapshots exactly.
- Visual review of the paired-children capture shows removal of the obvious duplicate facial outlines in both eyes. Additional backward, sideways and forward scenery captures were inspected. This is not a claim of perfect reconstruction or unchanged image quality everywhere.
- Synthetic foreground/background tests preserve a moving foreground silhouette for both strafe directions, asymmetric projections, either handedness, standard/reversed depth and 1x/half-resolution depth. Guard, near fade and depth multiplier tests also pass.
- Isolated hardware shader measurements across moving samples: about 0.45–0.72 ms more per stereo pair. These are warm repeated dispatches outside the game, not application FPS or total VR latency. In-game cost may differ.

## Limits and test

Background fill cannot recover truly hidden detail. Thin surfaces, hands, disocclusion strips, depth/color misalignment and motion over the bounded search distance can still produce artifacts. Independently moving/animated NPCs still have no per-object motion vectors. Next real captures are later timestamps and are not exact ground truth for the synthetic slot.

New captures identify `warpAlgorithm: source-depth-boundary-v2` (warp constant 65 = 2). For these captures, confidence green means a solved source lookup; red can mean unresolved/background-filled or inactive. Red no longer means an unchanged pixel. Raw diagnostic G/B encode selected source displacement.

Restart Skyrim from MO2, then compare strafing past a stationary NPC, standing still with a walking NPC, forward/backward movement, plants/building edges and hands. Capture using both grips, release, then both again to stop. Compare timing without recording: captures themselves cause overhead.

Original photos and settings are preserved. Deployment is limited to `OCU - DAPA and Eye Tracking TEST`; no Nexus release or remote publication. Restore the archived pre-repair DLL with Skyrim closed if the test regresses.
