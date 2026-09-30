# Capture stall repair and submission diagnostics

The 19:44–19:45 run had nine failed photo attempts: eight stale/out-of-order next frames and one invalidated history. Attempts repeatedly performed D3DCompile inside BeginEye, on the synthetic frame's render thread, before allocating/copying diagnostic textures. The approximately 300 ms armed-to-copy intervals exceeded the recorder's 250 ms next-frame age guard. The old four-attempt budget then ended sessions after seconds, despite the user's continuing movement.

Changes:

- Compile the diagnostic shader once asynchronously on a CPU-only worker. Poll completion without waiting; keep rendering normally while pending. Cache bytecode and the device-specific shader for subsequent captures. Device changes recreate the shader on the render thread; transient textures are still released after each sample.
- Preserve the 250 ms timestamp guard and history/size/orientation checks. Failed pairs are not presented as valid comparisons. Log real/predicted/next timestamps and their ages when rejected.
- Movement session lasts until manual stop or 120 seconds, independent of photo success. At most eight photo attempts; three failures pause further photos while movement logging continues. Five-second retry/save spacing; 16384 movement record cap. Session reports show attempts and failed/cancelled samples.
- Nonblocking haptic patterns: one 100 ms pulse=start; two separated pulses=stop; three=unavailable. Grip mapping and normal grip actions remain untouched. Desktop helper text updated.
- Remove normal per-second DAPA latency logging unless DebugLogging is enabled. Routine status interval becomes five seconds; motion diagnostics run while recording or debug logging. CPU stage-stall messages include real xrEndFrame time and recording/capture-busy flags. Synthetic end timing brackets xrEndFrame alone. Global logger, render cadence, recovery policy and GPU synchronization are unchanged.

Limits: capture still allocates GPU resources, copies textures, reads CPU data and writes a session index; it is not free and may hitch. The non-capture 43–101 ms xrEndFrame stalls are not proven fixed or attributed to a specific encoder/network/GPU cause. These changes remove a confirmed capture-side blocker and give better separation of CPU stages for the next run. No forced GPU flushes or waits added.

Validation: async preparation, no capture while compilation pending, shader reuse after success/abort, distinct haptic sequences/focus loss, bounded session/photo policy, stale-frame rejection, actual stereo shader output, PNG/report and helper tests. Existing motion/edge/timing/gaze tests are rerun. Real game capture and non-recording hitch behavior require retesting.

DLSS, all INI settings, shaders used for normal DAPA output, strafe and rotation prediction remain unchanged. Deploy only to the MO2 DAPA and Eye Tracking TEST mod, with backups; no Nexus or remote publication.
