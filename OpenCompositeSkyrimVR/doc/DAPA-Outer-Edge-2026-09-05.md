# DAPA outer-edge extension test — 2026-09-05

When a source lookup leaves an eye's cached image, the old solver could return an unchanged strip of the old image beside the rotated scene. The candidate clamps the missing lookup to the last valid texel and makes up to two constrained boundary refinements to preserve alignment along the edge. Valid interior roots still take precedence; internal depth-hole background handling remains intact.

This replaces duplicated/unshifted strips with edge extension, not reconstructed hidden scenery. Visible stretching/colored streaks can remain at exposed borders, particularly during large turns. The lookup retains its existing 12% displacement bound. No cross-eye texture borrowing, black fill, new render passes, extra cached frames, or blending toward the old unwarped strip was added.

Validation:

- Added an exposed-turn-edge regression that failed before the fix and passes after it. Tests include both directions, monotonic edge behavior, asymmetric eyes, both handedness, standard/reversed depth, reduced depth resolution, flipped source textures and top/bottom exposure. Existing turn/strafe/foreground tests pass.
- Capture, viewer, timing and gaze tests pass; runtime build succeeds.
- Hardware replay of nine new stereo captures reproduced their old predictions exactly with the pre-edge shader. Eight captures remain exactly unchanged with the candidate. In the active turn, no pixels changed in the central 76%-width by 76%-height rectangle in either eye.
- The older paired-children capture was also replayed; the earlier face-boundary improvement is visually retained, with changed outer borders. Its original recording predates the boundary solver, so its baseline is not expected to match the original prediction.
- The active turn's final isolated GPU timing was approximately 1.002 ms before and 1.014 ms after for both eyes combined. This single warm-dispatch measurement is not a reliable small-delta performance claim or game benchmark.

DLSS, Rotation Scale 1.00, strafe prediction, foveation, input and recording feedback are unchanged. Deployment is restricted to OCU - DAPA and Eye Tracking TEST, with a rollback backup. No Nexus or remote publication.

Test in the headset: gentle turn, reverse/release, then strafe. Check peripheral edge streaking versus the previous duplicated strip. Recordings do not include runtime/headset compositing and their next-real frames are delayed by capture overhead.

Replay images: C:\Users\borja\Documents\My Games\Skyrim VR\DAPA Capture Reviews\Outer edge repair - 2026-09-05\turn-final
