# DAPA HIGGS held-object test — 2026-09-06

## Scope and cause

The working mask classified geometry by ancestry under the player's first- and
third-person roots. HIGGS physically grabbed objects keep their world-reference
roots, so bottles and other grabbed clutter did not enter this protection path.
The body/equipped GPU mask and CSX accepted-draw adapter are unchanged.

Official HIGGS source inspected with the user's permission, read-only:
https://github.com/adamhynek/higgs/tree/93bf67b1bc4c4a11a20ccaef0d5012781d0d7eee

Relevant files: include/higgsinterface001.h, src/pluginapi.cpp, src/hand.cpp,
src/hooks.cpp, src/main.cpp, README.md, include/version.h and LICENSE.
Installed HIGGS metadata reports 1.10.10.0, matching the inspected version.

## Implementation

- Optional SKSE interface request at PostPostLoad, after HIGGS registers its listener.
- Read-only GetGrabbedObject for both hands after HIGGS's normal player update.
  No API polling, form lookup or scene traversal added inside draw dispatch.
- Do not use grab-event polling: the event precedes HIGGS's held-state transition.
  Regular refresh handles two-hand releases whose drop events are suppressed.
- Pin both held geometry roots. Render classification compares identities during
  its existing bounded ancestry walk; only a possible held match takes a lock
  and revalidates ownership. No cached geometry is dereferenced by the renderer.
- Clear the relevant hand on drop/stash/consume and both hands on save/new-game
  transitions. Released objects return to ordinary world treatment.
- Do not classify an entire grabbed actor/ragdoll as player-owned.
- Accept the verified 1.10.10+ 1.x API prefix. Missing/older HIGGS leaves the working
  body/equipped path intact and reports the unavailable integration in the log.
- No HIGGS grabbing/physics/settings mutations. No runtime, CSX, INI, shader/cache,
  load-order, DLSS5 or configurator modifications.

## Local validation

Release SKSE build succeeded. Existing Main.cpp NiTArray narrowing warning remains.

- New DapaHiggsTest: independent raw x64 vtable verifies slots 0/3/4/5/8/31,
  build gate, left/right identity, descendant classification, independent hands,
  handoff, release/stale snapshots, model replacement, pin lifetime and concurrent
  root refresh. These are ABI/cache tests, not a running-game HIGGS integration test.
- DapaEngineDrawTest: all 17 mesh call fixtures and existing-owner chaining pass.
- DapaCsxDrawTest: exact installed CSX function signature and both call sites pass;
  WARP/hardware both-eye masks, accepted/suppressed/redirected passes and D24S8/D32
  normal/reversed biased depth/occlusion/state restoration tests pass.
- Existing DAPA world/player motion and 60/72/80/90/96/100/120/144 Hz timing tests pass.

New SKSE DLL SHA256:
`0FE2B6AA540B2E64CDDC206E2D1E6705FAC04CBE4BE3BB6DB104193B44379210`

## In-game acceptance still required

Grab a bottle in each hand, strafe and stick-turn, then transfer/drop it. Check
that the held item stays aligned and the released item resumes world motion.
Repeat with a staff or an object with separate decorative geometry. Body/equipped
items should retain their already-confirmed behavior. Transparent materials that
do not contribute to scene depth and articulated grabbed actors are not proven
covered by this change; Spell Wheel/HUD ownership is a separate question.

Log evidence in Documents/My Games/Skyrim VR/SKSE/OpenCompositeInput.log:
`DAPA HIGGS: interface 1 connected`, per-hand held-root transitions, and increasing
`HIGGS-ownedPasses` in the existing periodic mask report. No per-draw logging added.
Actual visual quality and incremental GPU cost require the live test; no FPS
guarantee is inferred from these local tests.
