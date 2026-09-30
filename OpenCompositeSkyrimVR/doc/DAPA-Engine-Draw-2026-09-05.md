# DAPA body mask v3 — stable engine draw interception, local test

## Confirmed fault

In live Skyrim PID 33112 the four player-geometry vtable hooks remained installed,
but the renderer context's DrawIndexed/DrawIndexedInstanced entries reverted to
D3D11 functions and changed again between read-only snapshots. The mask bridge
pointer was zero. This explains why geometry detection did not produce a mask.
The writer of those dispatch entries was not identified; this is not evidence
that CSX deliberately removed OCU hooks.

## Changed

The SKSE producer now intercepts 17 fixed mesh-renderer call sites in Skyrim VR
1.4.15 instead of writing the D3D11 context vtable. Instruction signatures and
argument setup were inspected in the live unpacked executable. Both indexed
and indexed-instanced calls are covered, including their register and stack
arguments. Trampolines preserve the argument prefix and resume after the original
call. Native calls use current context dispatch, not saved D3D function pointers.

The original world draw runs first. Only positively identified player draws on
the published main depth target produce the existing R32 player-depth mask.
The existing mask GPU state preservation, stereo caching, final-depth validation
and world warp shader are unchanged. Geometry classification still uses current
first/third-person scene ancestry, not armor or index-buffer size guesses.

All sites are validated before code mutation. Unknown modifications reject
installation; partial installs roll back without overwriting newer patches.
Only runtime version 1.4.15 is accepted. No per-frame vtable repair/polling occurs.

## CSX interoperation and limitation

The local CSX source hooks RVA 0xDBDDF3 for its direct menu bridge and may suppress
or redirect that draw. An existing six-byte SKSE indirect-call owner is chained
exactly once; it is not bypassed for normal rendering. Such an opaque owned site
is pass-through for mask generation, since OCU cannot assume a draw actually
occurred after that callback. If player geometry reaches it, v3 explicitly logs
`existingOwner` rejection / cooperation needed. That coverage requires an owner
handoff rather than blindly redrawing CSX-suppressed content. Other unsupported
call modifications stop installation instead of being overwritten.

This repairs the demonstrated unstable-interception mechanism; it does not prove
complete live body/armor/weapon coverage. Floating HUD/debug text is not identified
by player ancestry and is not fixed by this patch. CSX's deployed DLL and source
were not changed. Full live CSX behavior still needs a game test.

## Diagnostics

`DAPA BODY MASK v3` logs installation, first classified player geometry, first
successful mask per call site, and periodic counts for setup, callbacks, owned
callbacks, mask draws, and rejection reasons: context, readiness, resources,
missing depth view, different depth target, format, allocation, existing owner.
Counters do not log/read back/flush on every rejected draw. The summary is emitted
every 131072 geometry setups; successful mask summaries remain limited to 5 s.

## Validation

- Release SKSE build.
- Standalone execution of all 17 exact call instruction fixtures. Checks indexed
  prefixes, both signatures, all six instanced arguments, current dispatch after
  simulated D3D vtable replacement, prior owner chaining, rollback, executable
  page protection restoration, and unknown-patch rejection.
- An additional fixture reproduces CSX-style indirect-call chaining.
- Existing production world-motion/body-mask GPU tests and all refresh-rate
  pacing/ownership/recovery tests pass.

These are automated tests, not live coverage or numerical GPU-cost measurements.
No runtime shader, DLSS 5 settings, foveation, strafe/rotation parameters, bitrate,
keyboard, laser configuration, or capture format changed.

## Test

Restart through the enabled `OCU - DAPA and Eye Tracking TEST` MO2 mod. First test
normal gameplay with recording off. Look down at hands/body/weapons and strafe.
Confirm v3 callbacks and maskDraws increase, then inspect player-mask-left/right
captures before claiming visual coverage. If coverage is missing, use the explicit
rejection counts; do not retune the working world correction to compensate.
