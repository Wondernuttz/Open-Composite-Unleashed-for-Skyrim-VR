# Rendering experiment cleanup — 5 September 2026

## Scope

- Follow-up cleanup removes the entire unused Meta SpaceWarpProvider and extension registration, the unreachable Meta submission branches, deprecated aswForceCustom/controller/MV settings, and orphaned scheduler counters. This does not control external runtime ASW/SSW features.
- Removed the disabled ONNX/RTMW Legacy2D inference, its dormant OSC sender, old recorder, obsolete GPU picker, and private state. Shared body-tracking contracts and the active MediaPipe World3D path remain.
- Removed the unreachable split-frame scheduler and its first-eye scheduling hooks.
- Removed no-op ASWProvider interfaces, cache-slot overrides, callback registration, and the CPU/GPU setup work feeding them.
- Removed unused first-person/stencil capture, readback, and disabled accumulator hooks. Kept the shared scene-target/depth-clear hooks used by NVIDIA VRS and cross-vendor density-mask foveation.
- Removed unused synthetic-frame DLSS handles and FSR contexts/output textures. Real game-frame DLSS/FSR dispatch remains.
- Removed the nonfunctional buffered/experimental/upscaler-history controls and their runtime configuration fields. Configurator save removes retired INI keys while preserving active settings and comments.
- Kept the SKSE bridge's binary layout intact; reserved legacy fields are not executable rendering paths.
- Kept the working DAPA depth/parallax shader, one-warp cadence, and recent refresh-relative timing/ownership fixes. Kept controller, keyboard, laser, and eye-tracking behavior outside the deleted experimental consumers.

## Verification

- Continuous 3D pose, gait and calibration-recorder self-tests pass. Active MediaPipe conversion/preview/OSC helper bodies compare unchanged against the pre-cleanup snapshot.
- Runtime binary scan finds no retired Meta extension registration or submission messages. Regression checks reject reintroduced provider files or symbols.
- RelWithDebInfo runtime build and Release configurator publish succeed.
- DAPA timing tests pass for 60, 72, 80, 90, 96, 100, 120, and 144 Hz, including image ownership and timeout recovery.
- Gaze regression tests pass.
- Density-mask pixel/state tests pass on D3D11 WARP and hardware at three sizes, including incomplete edge clusters.
- Configurator migration test passes with root/default/asw/general section layouts; retained DAPA, gaze, input and third-party settings survive, and repeated saves are idempotent.

These are build and automated regression results, not an in-headset visual or performance certification. Community Shaders' earlier depth consumers and third-party frame-generation interoperability still require live testing. CSX binaries and game shader caches were not modified by this cleanup.
