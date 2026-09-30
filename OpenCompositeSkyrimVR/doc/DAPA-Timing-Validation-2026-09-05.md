# DAPA timing repair — local test build, 5 September 2026

## Scope

No new GPU passes, texture copies, GPU readbacks, forced Flush calls, shader edits,
or per-frame heap allocations were added. Normal operation still inserts one warp
between real frames. Saved user settings, CSX and shader caches are not changed.
There are additional CPU clock reads and ownership checks; zero performance impact
has not been established by an in-game benchmark.

## Changes

- Stall detection is 2.7 runtime display periods (30 ms at 90 Hz), not a fixed
  30 ms for every headset rate.
- Cooldowns use elapsed steady-clock time: 180, 710, 2840 and 11380 ms, capped.
  Adaptive dwell/recovery also uses elapsed time. Falling FPS no longer stretches
  these delays into increasingly long waits. Setting/rate changes reset history.
- Adaptive engagement uses at least 55% of runtime refresh as its effective
  threshold, while respecting higher configured thresholds. This avoids an empty
  engagement band at e.g. 144 Hz with the old default 50 FPS threshold. The saved
  value is not rewritten. Always-on mode remains independent of this threshold.
- Color/depth image waits share a requested budget of one quarter of a display
  period, capped at 2 ms total. A color timeout skips that synthetic submission
  and enters the existing backoff path. An optional runtime depth timeout omits
  the depth attachment for that submission; the warp still uses cached game depth.
- A timeout preserves the acquired image/index for the next wait. The code never
  acquires another image or releases the timed-out image prematurely. Negative
  errors and session-loss status cannot be mistaken for successful writable images.
- Output/depth release results are checked. The runtime never receives a stale
  depth attachment merely because the depth swapchain exists.

## Validation

OCUDapaTimingTest compiles the production timing/ownership helpers and exercises
60, 72, 80, 90, 96, 100, 120 and 144 Hz. It checks stall scaling, wait budgets,
adaptive engagement range, cooldown expiry, escalation limits, clean recovery,
and recovery at maximum adaptive dwell. Mock OpenXR entry points inject 1,000
timeouts, then recovery, release failure, and session loss; assertions check exact
image ownership, no repeated acquire, and no release before successful wait.

This does not reproduce the reported combat slowdown in a running headset.
OpenXR permits a timed wait to exceed its requested timeout because of scheduling
or contention. xrWaitFrame and xrEndFrame do not offer the same timeout parameter;
this patch cannot guarantee recovery from a hung graphics driver/runtime.

## Follow-up measurement

Compare the same combat scene before/after at the same headset refresh, graphics
settings and DAPA mode with other frame generators disabled. Record application
frame-time spikes, VD game/encoding/network/decoding latency and ASW wait/submit/end
diagnostics. Passing helper tests is not proof of improved headset performance.

OpenXR timeout ownership reference:
https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrWaitSwapchainImage.html
