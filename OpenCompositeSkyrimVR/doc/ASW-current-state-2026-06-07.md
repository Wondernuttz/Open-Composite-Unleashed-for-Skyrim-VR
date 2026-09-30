# OCU ASW Current State - 2026-06-07

This note captures the current ASW condition before reworking timing/scheduling.

## Repo State

- Repo: `<local OCU checkout>`
- Branch: `Unstable`
- HEAD: `792899cae4185abb44ce4d1428df1051114b6ab4`
- Deployed runtime DLL:
  `<MO2 mods folder>\OpenComposite Unleashed for Skyrim VR\root\openvr_api.dll`
- Deployed DLL SHA256:
  `9F2B0255555C76B829ABB039EB8706B87CB8946A897456C52E9DB99D88605F85`

The working tree is dirty. Do not assume HEAD alone represents the playable build.

## Current Live ASW Config

From deployed `opencomposite.ini`:

```ini
aswEnabled=true
aswWarpStrength=0.00
aswRotationScale=0.00
aswTranslationScale=0.00
aswDepthScale=0.00
aswNearFadeDepth=1.50
aswLocoScale=0.00
aswMVConfidence=2.50
aswMVPixelScale=1.00
aswExperimentalMode=false
aswBufferEnabled=false
aswUpscalerReset=false
aswUpscalerReactiveMask=false
aswFPControllerScale=0.45
aswEdgeFadeWidth=1.00
```

Important: zeroed warp strengths do not disable ASW. With `aswEnabled=true`,
the provider can still initialize, cache color/depth/MV data, extract depth,
dispatch compute, acquire/wait/release swapchains, and submit synthetic frames.

## Current Symptoms

- ASW can sharply preserve the center view around medium distance.
- There is persistent side/peripheral jitter, especially on lateral movement.
- The current ASW path can create severe latency/choppiness.
- Under ASW load, Windows speech-to-text / voice typing can corrupt or delay.
- This appears to be system/frame-timing pressure, not direct audio mutation.

## Current Diagnosis

The visual warp has value, but the scheduler is too aggressive.

The current PC-side ASW path can run extra work after a real frame:

1. Real frame is submitted.
2. OCU attempts an ASW frame.
3. OCU calls another `xrWaitFrame`.
4. OCU calls `xrBeginFrame`.
5. OCU locates predicted views.
6. OCU warps cached textures for both eyes.
7. OCU waits/acquires output swapchains.
8. OCU submits `xrEndFrame`.

This can add work on top of the normal game frame instead of reducing real-frame
cadence to half-rate. If it misses budget, it can steal time from the next real
frame and spiral into latency.

## Reference Model

OpenXR frame sync:

- `xrWaitFrame` throttles the application frame loop to synchronize submissions
  with display timing.
- `predictedDisplayTime` is the runtime's predicted display time for the next
  composited frame.
- `predictedDisplayPeriod` is the display period and should be used to predict
  later display times.
- OpenXR notes that accurate, consistent display time across a frame pipeline
  is important to avoid motion judder.
- `xrWaitFrame` should be called once per application-generated frame.

Proper reprojection/spacewarp model:

- If the headset is 90 Hz, run the app at about 45 real FPS and synthesize the
  alternate frames.
- If the headset is 120 Hz, run the app at about 60 real FPS and synthesize the
  alternate frames.
- Do not synthesize extra frames on top of a full-rate app without strict budget
  control.
- Synthetic frame generation should be opportunistic: skip when timing is bad.

## Implementation Direction

Short-term:

- Treat `aswEnabled=false` as a hard stop: no ASW init/cache/depth/MV/warp/submit.
- If ASW work is effectively zero, skip the ASW pipeline instead of doing no-op
  frame generation.
- Add timing guards around the injected ASW frame.
- Avoid blocking optional ASW on unbounded waits.
- Add stall cooldown instead of attempting ASW every frame after a bad wait.

Medium-term:

- Use `predictedDisplayPeriod` instead of hardcoding 60/90/120 Hz behavior.
- For synthetic frame pose, use the real frame's display time plus display
  period, then call `xrLocateViews` for that target time.
- Avoid calling a second `xrWaitFrame` just to obtain pose.
- If a second OpenXR frame is actually submitted, it must obey frame lifecycle,
  but it must be guarded and skipped when budget is not available.

Long-term:

- Move toward a half-rate scheduler: real, synthetic, real, synthetic.
- Coordinate with SKSE data for ground-truth depth/MV/camera/player movement,
  but keep headset pose prediction owned by the OpenXR runtime.

## Sources Consulted

- Khronos OpenXR specification, frame synchronization:
  https://registry.khronos.org/OpenXR/specs/1.0-khr/html/xrspec.html
- Monado OpenXR runtime frame pacing notes:
  https://monado.pages.freedesktop.org/monado/frame-pacing.html
- Microsoft WMR motion reprojection overview:
  https://learn.microsoft.com/en-us/previous-versions/mixed-reality/enthusiast-guide/using-steamvr-with-windows-mixed-reality
- Oculus/ASW 2.0 public technical discussion:
  https://roadtovr.com/oculus-launches-asw-2-0-asynchronous-spacewarp/

