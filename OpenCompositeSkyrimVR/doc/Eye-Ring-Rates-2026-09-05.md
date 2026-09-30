# Eye-tracked ring rates

Implemented locally. The follow-up one-page configurator deployment updates the
MO2 test mod; Nexus staging is not updated by that follow-up. See the one-page
deployment report for verification and backup details.

Video > Foveated Rendering now exposes custom center, middle and outer rates:
1x1, 1x2, 2x1, 2x2, 2x4, 4x2, 4x4. Ring sizes remain independent.
Custom rates are opt-in and apply only when the compositor selects valid eye
tracking. Fixed fallback and the legacy density-mask pattern remain unchanged.
Gaze loss still selects the existing explicit fixed fallback or full detail.

The effective-rate readout shows the compatibility cap. With compatibility on,
rates coarser than half density are capped to 1x2/2x1, retaining the requested
axis for 2x4/4x2. Square coarse rates use Favor Horizontal. Requested values are
preserved when toggling the cap; no global compatibility setting is auto-changed.

INI keys in [vrs] (root/default layouts also supported):

```ini
vrsEyeCustomRates=false
vrsEyeInnerRate=1x1
vrsEyeMidRate=2x1
vrsEyeOuterRate=2x2
```

Invalid rate strings resolve to 1x1. Existing INIs remain on the default path.
Save and restart the game to change configuration; valid-gaze mode selection
continues automatically per frame. These are not live headset controls.

## Backends

- NVIDIA NVAPI D3D11: three ring palette entries plus an independent full-rate
  entry for pixels outside the eye bounds. Hardware support is checked by the
  existing NVAPI capability query. Driver API failures disarm foveation.
- Cross-vendor D3D11 density mask: equivalent sample densities, NOT identical
  hardware VRS footprints. Keeps complete 2x2 pixel quads alive. Masking and
  reconstruction share rate dimensions and stay inside the same 8x8 cluster;
  partial edge clusters remain unmasked. Fixed/default uses the original pattern.

No DAPA scheduling, CSX binaries, upscaling ownership, shader caches, or live game
settings were changed. This does not repair or certify
the CSX sparse-depth integration boundary documented in Foveation-Validation-2026-09-05.md.

## Verification

- C++ runtime and C# configurator builds pass. Existing C++ warnings remain.
- Gaze/rate tests: all rate IDs/names, invalid inputs, compatibility limits,
  anisotropic cap direction, fixed-profile isolation.
- Configurator tests: all seven rates round-trip in root/default/vrs INIs,
  preserving other rings and fixed sizes; effective display matches cap rules.
- Production density-mask/reconstruction HLSL: 360 cases across D3D11 WARP and
  this PC's hardware device, seven uniform and seven mixed-ring patterns plus
  legacy behavior, both compatibility modes, normal/reversed depth, aligned and
  non-aligned stereo eye edges. Per-pixel mask matches expected density/axis;
  spatially varying reconstruction verifies the donor sample, alpha and no
  cross-eye reads; application blend/constant/UAV state restoration tested.
- Manual isolated configurator test: custom toggle, seven dropdown choices,
  cap/effective readout and save/reload. Corrected form fitting to keep Save
  reachable when Windows constrains initial window height.

Still requires in-headset visual/performance tests and real AMD hardware tests.
NVAPI runtime builds but its new rate palette has not been exercised in-game.
No universal artifact-free behavior or FPS gain is claimed.
