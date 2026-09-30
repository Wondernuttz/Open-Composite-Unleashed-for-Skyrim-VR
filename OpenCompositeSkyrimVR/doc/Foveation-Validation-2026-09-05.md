# Foveation repair validation — 5 September 2026

This is a local test build, not a claim that every headset/rendering stack has
passed visual testing. Fixed foveation and NVIDIA VRS selection remain available.
No Pulse rendering implementation was added, and no CSX DLL or shader cache was changed.

## Repairs

- Reconstruction now uses its own constant-independent fullscreen vertex shader.
- Mask and reconstruction draws explicitly set blending, color writes and sample mask,
  then restore application state.
- Frame flags reset before menu, missing-gaze or invalid-geometry early returns.
- Incomplete 8-pixel edge clusters render at full density, preventing reconstruction
  from a skipped neighbour at non-aligned eye-buffer dimensions.
- Application output-merger UAV bindings are captured/restored together with the
  actual RTV count. UAV append/counter offsets are preserved. Restoring eight RTV
  slots unconditionally had erased application UAV bindings.

## Executed validation

Production DensityMaskManager.cpp and its actual HLSL are compiled into
OCUDensityMaskD3D11Test, not replaced with a mathematical mock.

Passed WARP and hardware D3D11 tests: 128x64, 134x70 and 130x66 stereo atlases,
both depth directions, and both compatibility pattern modes (24 combinations).
Tests check mask survivors in both eyes, reconstructed RGBA pixels, no cross-eye
color contamination, hostile VS constants, disabled color writes, zero sample mask,
UAV binding preservation, second-eye submission, and next-frame reset.

The 130x66 test failed before the edge fix. The application UAV assertion failed
before state restoration was fixed. Both pass after the repairs.
OCUVRSGazeSelfTest also passed. The production OCOVR runtime builds successfully;
existing warnings remain in unrelated code.

These tests do not run Skyrim, a headset eye-gaze runtime, an AMD GPU, CSX's full
effect chain, DAPA, SSW or AFW. They prove specific pixel/state behavior, not visual
quality or performance of the combined game pipeline.

## CSX interaction: remaining integration work

The local CSX source exposes an external-upscaler ownership record (method, render
scale, mip bias and flags). That is not a density-mask depth reconstruction contract.
OCU currently injects a mask into scene depth and reconstructs final color at OpenVR
Submit. Depth-dependent CSX effects may have consumed sparse depth before that point.
Late color repair cannot retroactively repair those earlier inputs or temporal history.
The runtime now logs this boundary once when RDM sees CommunityShaders.dll.

The installed CommunityShaders.dll matches its own build manifest, but does not
match the DLL in the discovered CSX build directory. That source snapshot's Git
worktree pointer is unresolved. No CSX replacement was built from that mismatched
snapshot, and no existing configuration was silently disabled.

Proper integration needs a verified CSX source baseline and a defined reconstruction
boundary before its first dependent depth/color/motion consumer, including resize,
menu transition and history-reset behavior. It must preserve coherent depth and
motion alongside color; a second final-color filter is insufficient.

## Coldbomb AFW

No AFW source/interface was inspected or integrated in this change. Compatibility
is unknown. Integration needs explicit ownership of eye/frame scheduling, real vs
synthetic frame identity, pose/sample time, color/depth/motion validity and temporal
history. DAPA/SSW/AFW must not independently reinterpret the same frames without
an agreed contract. Do not infer compatibility from foveation tests alone.

## Direct3D reference

RTVs and pixel UAVs share output-merger binding slots; restoring state must respect
that overlap and preserve UAV counters:
https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-omsetrendertargetsandunorderedaccessviews
