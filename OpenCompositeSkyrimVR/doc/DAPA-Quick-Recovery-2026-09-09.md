# DAPA quick recovery — 9 September 2026

The PSVR2 / RX 7900 XTX report shows successful synthetic submissions blocking for tens of milliseconds, 1,888 configuration/reset records driven by period fluctuations, and almost no held real frames. Two accounting bugs defeated the pacing guard: period changes reset its history, and the next cooldown update deducted time spent in the stall before the cooldown existed.

This local test change:

- Resets DAPA policy on enable/auto changes and session creation, not runtime cadence fluctuations.
- Uses absolute steady-clock deadlines from trouble observation for both pacing and API-error cooldowns.
- Keeps accepted-frame pressure separate from API-error recovery. Moderate isolated spikes remain tolerated. Repeated moderate or a severe spike yields for up to two baseline intervals, capped at 25 ms and one real-frame injection opportunity. The following real frame can retry with no clean-zone gate or escalating pacing hold.
- Keeps real-frame caching and motion history alive through brief pacing yields. API-error and auto-native holds still suppress cache work. Existing incomplete/stale stereo rejection remains in the compositor.
- Uses the fastest valid runtime period observed since session creation for DAPA pressure and auto thresholds. This is a conservative cadence baseline, not a physical refresh-rate measurement. Faster samples update it; slower samples do not. A headset refresh reduction within an existing session requires session recreation to relearn the slower baseline; pressure yields are still capped. OpenXR-provided display timestamps are unchanged.
- Adds throttled `DAPA FRAME TIMING` records: real/synthetic reported periods, baseline, predicted target spacing, elapsed time from real submission to synthetic end entry, begin/locate time, and layer assembly time. This does not synchronize the GPU, dump textures, or measure absolute target lateness.
- Skips capture-only movement bookkeeping and polling in capture-disabled production builds. The diagnostic shader entry explicitly returns without touching D3D inputs. `captureCompiled=0` identifies this in the configuration log.

Validation: RelWithDebInfo runtime build; timing regression tests at 60/72/80/90/96/100/120/144 Hz, synthetic stall sequences and cadence multiples, one-frame retry bounds, deadline accounting, API failures and image ownership; production capture inactivity; developer capture test with actual software-D3D shader/readback; existing DAPA motion tests. All passed. No headset smoothness or compositor-blocking fix is claimed from these tests.

The old capture repair notes identify a real diagnostic-shader compilation hitch. The PSVR2 report's detailed latency samples have captureBusy=0 and recording=0. The preserved Kemeros ProcMon audit shows asset reads and short OCU SKSE log writes, but is not an OpenXR frame-timing trace. These records do not establish that both testers' FPS collapses have the same initiating cause.

Retest the same gameplay route with existing CSX/RDM settings and manual DAPA, debug logging on and captures disabled. Check actual injection continuity, realAccepted/syntheticAccepted, held counts, and the new timing records after a scene-load or frame-time spike. API success and software tests do not establish presented FPS. The underlying synchronous synthetic xrEndFrame can still block the game thread; the new records narrow where its budget is spent.

Frame-loop reference: [Khronos xrWaitFrame](https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrWaitFrame.html). The runtime may change throttling and predicted timing in response to submission/completion timing. A separate synthetic frame needs its own wait/begin/end sequence; this patch does not fabricate display times or introduce concurrent OpenXR/D3D calls.
