# DAPA player-body protection — local test candidate

## What changed

The old SKSE first-person detector used a cached first-person root and index-buffer-size guesses. Its live log reached 31 million setup calls while reporting only six detected first-person setups. That detector did not provide a usable full-body mask to the current DAPA shader.

The replacement uses CommonLibVR `BSRenderPass::geometry`, `NiAVObject::parent`, and the current `PlayerCharacter::Get3D(true/false)` roots. Lighting and effect shader setup/restore hooks classify each pass by ownership. Armor is identified by its actual scene ancestry, not equipment names or a whitelist. It does not retain raw geometry pointers across equipment changes.

Only owned indexed draws against the game's published main depth resource are replayed into a single-channel R32 device-depth mask. Shadow/reflection depth targets are excluded. The original game draw is unchanged. There is no second fully shaded character render, production CPU readback, GPU flush, or new full-screen compute dispatch. Existing first-person staging/replay experiments and their repeated logging were replaced in this hook path.

The replay retains the current vertex/geometry shaders, skinning data and viewport. It temporarily replaces the pixel shader and output state, then restores pixel shader/class instances, render targets, depth/stencil state, blend factors, sample mask and output UAV bindings/counters. This follows the output-state coupling described in [Microsoft's D3D11 output-merger documentation](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-omsetrendertargetsandunorderedaccessviews).

The existing bridge reserved byte `_padPreFP[0] == 1` negotiates the new mask format. Old plugins leave it zero, so their old mask pointer is not used. The two source-eye regions are cached independently at the same resolution and bounds as source depth; the producer is recycled after eye 1. The mask is not inferred from closeness to the camera. Final-depth matching rejects masked geometry hidden by other surfaces. The regular world solve remains unchanged outside the protection rule.

## What it does not promise

- This is a test candidate, not verified live VRIK/CSX coverage or a measured FPS improvement.
- Player-owned source pixels are retained between real frames. Independent hand motion, skinning, cloth and walking animation remain at real-frame cadence; this does not generate those animations.
- Transparency, non-depth-writing effects, unusual mod reparenting and shader families other than lighting/effect may need additional handling. Final depth and color alignment under CSX/DLSS must be checked with the new masks.
- Rasterizing the mask costs vertex work/draw calls and some bandwidth. Per-eye mask copies and shader reads also cost GPU time. No numerical overhead promise has been made.
- Unknown or unsupported mask input leaves existing world correction running; it is not evidence of successful player protection.
- DLSS 5, rendering strength, foveation, bitrate, locomotion tuning and rotation settings are unchanged.

## Validation

- Runtime and SKSE Release builds succeed against local CommonLibVR-check. Existing unrelated C4244 and macro warnings remain.
- D3D11 WARP tests exercise the production body-mask renderer: exact raw depth in one eye viewport, untouched game color, restored blend/sample state and output UAV bindings.
- Production warp shader tests cover body retention under both strafe directions, forward/back motion and yaw, normal/reversed depth, two depth resolutions, vertical source flips, and wrong-depth/occluder rejection. Existing world-motion regression tests pass.
- Offline replay of the user's 21:37:46 stereo capture with no body mask produces zero changed pixels against the pre-body shader, in both eyes. This is not a live mask-coverage or performance test.
- Capture tests save both raw masks and depth-validated mask PNGs alongside real/prediction/next images, with stereo/time/stale-data safeguards retained. Timing and VRS gaze tests pass.

## Live test and evidence

Requires updating both `root/openvr_api.dll` and `SKSE/Plugins/OpenCompositeInput.dll` in the MO2 test mod while Skyrim is closed. Do not change INI settings. Restart through MO2.

First compare normal gameplay performance with recording off. Then capture hands/body during locomotion and stick turn. `player-mask-left.png` and `player-mask-right.png` must cover visible player geometry, not surrounding scenery. White means depth-validated protected pixels; black is unprotected. Raw mask `.f32` values are device depth, with -1 outside rendered mask coverage, in canonical image orientation. Metadata `playerMaskActive` records whether that eye had a usable mask input, not whether all body pixels were covered. Warp algorithm 3 is the world solver with player masking. The offline replay tool requires raw masks for algorithm 3 and refuses an incomplete replay.

Check `OpenCompositeInput.log` for `DAPA BODY MASK` detection/draw counts. No live-success or GPU-cost claim should be made until those masks and timings are inspected.
