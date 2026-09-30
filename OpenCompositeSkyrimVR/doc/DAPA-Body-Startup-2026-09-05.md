# DAPA body-mask startup repair — local test

The 22:05 Skyrim run loaded the correct MO2 test binaries. Its SKSE log entered
body-mask initialization at 22:07:38 but never reached the installation summary
or player-mask draws. The installer discarded several MinHook error statuses.
The old log cannot identify which of those calls failed.

A standalone hardware D3D11 test on this PC passed the original MinHook sequence.
That rules out a reproducible bare-driver hook failure, not a failure in the
modded game. The local CSX source also hooks DrawIndexed.

## Repair

- Chain the existing DrawIndexed and DrawIndexedInstanced vtable entries on the
  exact renderer context published by the bridge. Do not decode/patch another
  mod's function prologue, replace its hook, or use a possibly unwrapped context
  from GetImmediateContext.
- Preserve the existing lighting/effect setup and restore chains (six slots total).
- Publish original callbacks before installing each slot. Enable mask rendering
  only after all slots succeed. Report every startup stage/failure and roll back
  partial installs without overwriting later hooks.
- Retry missing renderer resources at new-game/post-load notification. Repeated
  successful installation is a no-op.
- Log first player-owned geometry and actual mask draws separately from hook
  installation. Installation alone is not proof of coverage.

## Validation

The standalone test uses the same slot helper as the plugin, with real hardware
D3D11 calls and existing MinHook detours in place. Both signatures reach the
existing hook exactly once; removing the slot restores those hooks. Synthetic
read-only slots test failed installation, repeated installation rejection,
protection restoration, rollback and preservation of a later hook.

No runtime shader, mask algorithm, INI setting, DLSS 5, foveation, strafe/rotation
tuning, keyboard or laser behavior changes in this repair. No new GPU pass is
introduced beyond the already-deployed body-mask experiment. Actual live coverage
and performance remain to be checked after restart.

Expected log: `DAPA BODY MASK v2: all six hooks installed`, followed by first
player-owned geometry and increasing `maskDraws`. Then inspect captured
`player-mask-left.png` and `player-mask-right.png` for hands/body/weapons coverage.
