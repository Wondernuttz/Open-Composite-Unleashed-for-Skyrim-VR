# DAPA Spell Wheel ownership test — 2026-09-06

Source examined read-only: `C:/Users/borja/Desktop/SpellWheelVR 1.5.11 Source`.
No SpellWheelVR DLL, source or ESP changes; this integration is entirely in OCU.

## Findings

Spell Wheel's public interface reports whether wheels are open but does not expose
their geometry. Its implementation renders orbs, hover text and wrist bars using
dedicated projectile references. The authoritative source functions are
`GetNumberFromProjectileFormId`, `GetLetterFromTextProjectileFormId`, `IsBarProjectile`
in Helper.cpp and `UpdateProjectile` in SpellWheelVR.cpp.

## Change

- Resolve 247 exact UI projectile base records from SpellWheelVR.esp at DataLoaded:
  61 wheel slots per hand, 60 text slots per hand, and 5 bar records.
- Use CommonLib's form lookup, not an assumed load-order byte.
- During the existing bounded geometry-ancestry walk, retain the nearest reference
  owner from NiAVObject.userData. Compile-time check verifies the VR member offset
  is 0x110, matching CommonLib's GetUserData implementation.
- Only references whose projectile base is in the resolved catalog enter the
  existing body/held-object mask. Ordinary item meshes used as wheel icons qualify
  by reference ownership, not by their shared mesh/texture filename.
- No per-frame polling, scene scan, extra hook, cached transient reference,
  GPU readback or flush. One base-type check then binary search for projectiles.
- Conjuration displays, sparks, distance probes, ordinary spells/arrows and other
  world geometry are not included merely because they belong to the same mod.
- HIGGS, body ownership, CSX accepted-draw integration and GPU replay are unchanged.

## Validation

Release SKSE build passed (one existing Main.cpp NiTArray narrowing warning).
New C++ catalog tests passed for all IDs, load-order isolation, resolved light-ID
identity, absent mod, null IDs, non-projectiles and unrelated projectiles.
Read-only Python verification matched all 247 IDs against BOTH the author source
and the installed ESP's PROJ records; 42 named effect/probe IDs were excluded.
Existing HIGGS ABI/lifetime tests, all 17 engine-call tests, exact installed CSX
adapter tests, WARP/hardware both-eye depth-mask tests, world/player motion tests,
and 60/72/80/90/96/100/120/144 Hz timing tests passed.

Test build SHA256:
`9E97102FE12AEEC3EE95194FC18D5C292095422EA57B3FFE20352767D1C3CD92`

## Still requires live validation

This is an ownership integration, not proof that every transparent material is
protected. The unchanged mask depends on matching the visible scene depth.
Transparent/glowing parts that do not write depth may require a separate solution;
loosening depth rejection globally would risk regressing the world/body fix.

Test: open each wheel, strafe, stick-turn, change pages, highlight icons, and close
the wheel. Check opaque item models, spell/glow icons, hover text and wrist bars.
Check body/HIGGS alignment and real projectiles after closing the wheel.
No measured FPS/visual result is claimed before this test.

Log: Documents/My Games/Skyrim VR/SKSE/OpenCompositeInput.log should report
`DAPA SPELL WHEEL: resolved 247/247` and increasing `SpellWheel-ownedPasses` in
the existing periodic mask report while the UI is being drawn.
