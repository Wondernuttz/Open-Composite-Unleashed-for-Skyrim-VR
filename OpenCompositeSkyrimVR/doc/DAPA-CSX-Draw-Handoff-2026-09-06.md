# DAPA player draw handoff — 6 September 2026

## Fault and repair

The 5 September PID 7812 run classified 26,370 player passes, but recorded only
three mask draws. 26,382 player draw callbacks reached an existing CSX owner and
were deliberately passed through without masking. Geometry ancestry was working;
the missing accepted-draw observation was the immediate integration failure.

The v4 SKSE plugin preserves CSX's original callback exactly once and observes
its two accepted DrawIndexedInstanced sites. Ownership is scoped to the exact
context and six draw arguments on the rendering thread. Accepted player geometry
is replayed into the existing raw-depth mask while its vertex/skinning state is
still bound. Suppressed or redirected draws do not reach these sites and do not
produce a mask. Nested calls, other threads, and replay cannot inherit or claim
the same ownership twice.

This is an OCU-side binary-version adapter, not a rebuilt CSX source API. The
discovered CSX source/build does not match the installed DLL and contains Pulse
work. It was NOT rebuilt or deployed. A future maintained CSX source API should
replace the version adapter when the matching source baseline is available.

Supported installed CSX SHA256:
`C192935E9A509835C6B71A562D48614D1DD23FA077E8F2622BBE051BDC95AFA5`

The adapter validates all 900 bytes of the owner function at RVA 0x105BD0 before
any code writes, using SHA256
`2B5BC145D2A4D7F9C979DF117FA724ECB49B14E815830287B43984F2BD92EF24`.
Accepted sites: 0x105C3C and 0x105F10. These are process-memory hooks, not edits to
CommunityShaders.dll on disk. Unknown owners/changed function bytes are refused.
Both observer patches are rolled back if installation fails.

No persistent D3D11 vtable patch, per-draw module lookup, CPU image readback,
shader cache invalidation, DLSS5 setting change, or world-warp change was added.
Player masking still costs extra player-geometry draws; no zero-cost/FPS claim
is made. Module/signature checks run at installation, not per frame.

## Executed tests

- Release OpenCompositeInput, DapaCsxDrawTest, DapaEngineDrawTest builds passed.
  Existing unrelated Main.cpp NiTArray narrowing warning remains.
- DapaCsxDrawTest mapped the actual installed CSX without invoking its entry
  point: full-function hash, both hook installs, exact-byte rollback, wrong
  owner and changed-function rejection passed.
- Both exact accepted-call instruction fixtures passed the Windows x64 ABI,
  all six arguments including stack arguments, native/non-player behavior,
  ownership notification and rollback checks.
- Ownership scope tests passed nesting, non-player shadowing, separate thread,
  wrong context/arguments, reentry and exactly-once claims.
- WARP and hardware D3D11 tests passed both eye viewports with the production
  dispatch helper and mask renderer: accepted mask depth .4, untouched pixels
  -1, scene color preserved, PS and render target restored. Modeled suppressed,
  redirected and non-player cases produced no mask. These policy fixtures do
  not execute the whole CSX menu subsystem or Skyrim.
- OCUDapaMotionTest passed existing world corrections plus body-mask strafe,
  forward/back, yaw, reduced depth, flipped bounds, depth conventions and
  occlusion rejection.
- OCUDapaTimingTest passed 60/72/80/90/96/100/120/144 Hz and ownership/recovery.
- Git diff whitespace check passed (existing line-ending warnings only).

## Required live verification

First live run, PID 19916: both observers installed and over 26,000 player mask
draws were recorded with zero owner-not-accepted rejections. Equipped swords
were reported working by the user. However the hand/arm capture failed visual
verification: the first raw mask contained 540,509 covered pixels and zero
depth-validation matches. The silhouettes were present but their rasterized
depth values disagreed with final scene depth. This is not a successful body fix.

The second build addresses that separate producer error: replay uses a read-only
scene DSV with EQUAL comparison and no depth/stencil writes, and its pixel shader
samples actual stored scene depth through a compatible SRV. It does not assume
unbiased SV_Position.z equals stored depth and does not let a later depth-rejected
rear surface overwrite visible body coverage. Resource views are cached per
resource/mip; the previous PS resource at t0, DSV and all existing saved state
are restored. No validation tolerance was loosened.

A new regression failed before this change at the assertion that mask values
must equal biased scene depth. Afterward all eight WARP/hardware, D24S8/D32,
normal/reversed cases passed, including a subsequent occluded rear draw. The
existing motion, timing and accepted-draw tests passed again. This is a
reproduction of producer failure modes; the capture alone does not establish
which combination of bias and hidden-surface writes caused every mismatch.
The second build still requires its own live coverage check.

Look for `DAPA BODY MASK v4` installation and `first CSX-accepted player draw
MASKED`, then nonzero increasing `CSX(accepted=...,player=...)` and `maskDraws`
counters. Record hands/body while strafing and turning; the depth-validated
player-mask capture must cover visible player pixels in both eyes. This build
is not declared visually fixed solely because the standalone tests passed.

Floating debug HUD notifications are not player-owned geometry. This change
does not claim to isolate those notifications or fix independent CSX temporal
ghosts, runtime/streaming latency, or all possible third-party renderers.

Rollback: the previous OCU SKSE DLL and source are retained in the Desktop
`OCU Nexus Staging Archives/DAPA-CSX-Draw-Handoff-20260906` directory. Close
Skyrim before restoring that DLL. CSX and the OCU runtime DLL were not replaced.
