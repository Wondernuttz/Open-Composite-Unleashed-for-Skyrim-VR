#pragma once

class Config {
public:
	Config();
	~Config();

	bool RenderCustomHands() const { return renderCustomHands; }
	vr::HmdColor_t HandColour() const { return handColour; }
	float SupersampleRatio() const { return supersampleRatio; }
	bool Haptics() const { return haptics; }
	bool AdmitUnknownProps() const { return admitUnknownProps; }
	inline bool UseViewportStencil() const { return useViewportStencil; }
	inline bool ForceConnectedTouch() const { return forceConnectedTouch; }
	inline bool LogGetTrackedProperty() const { return logGetTrackedProperty; }
	inline bool StopOnSoftAbort() const { return stopOnSoftAbort; }
	inline bool EnableLayers() const { return enableLayers; }
	inline bool DX10Mode() const { return dx10Mode; }
	inline bool EnableAppRequestedCubemap() const { return enableAppRequestedCubemap; }
	inline bool EnableHiddenMeshFix() const { return enableHiddenMeshFix; }
	inline bool InvertUsingShaders() const { return invertUsingShaders; }
	inline bool InitUsingVulkan() const { return initUsingVulkan; }
	float HiddenMeshVerticalScale() const { return hiddenMeshVerticalScale; }
	inline bool LogAllOpenVRCalls() const { return logAllOpenVRCalls; }
	inline bool EnableAudioSwitch() const { return enableAudioSwitch; }
	std::string AudioDeviceName() const { return audioDeviceName; }
	inline bool EnableInputSmoothing() { return enableInputSmoothing; }
	int InputWindowSize() const { return inputWindowSize; }
	inline bool AdjustTilt() { return adjustTilt; }
	inline bool AdjustLeftRotation() { return adjustLeftRotation; }
	inline bool AdjustRightRotation() { return adjustRightRotation; }
	inline bool AdjustLeftPosition() { return adjustLeftPosition; }
	inline bool AdjustRightPosition() { return adjustRightPosition; }
	float Tilt() const { return tilt; }
	float LeftXRotation() const { return leftXRotation; }
	float LeftYRotation() const { return leftYRotation; }
	float LeftZRotation() const { return leftZRotation; }
	float RightXRotation() const { return rightXRotation; }
	float RightYRotation() const { return rightYRotation; }
	float RightZRotation() const { return rightZRotation; }
	float LeftXPosition() const { return leftXPosition; }
	float LeftYPosition() const { return leftYPosition; }
	float LeftZPosition() const { return leftZPosition; }
	float RightXPosition() const { return rightXPosition; }
	float RightYPosition() const { return rightYPosition; }
	float RightZPosition() const { return rightZPosition; }
	float LeftDeadZoneSize() const { return leftDeadZoneSize; }
	float LeftDeadZoneXSize() const { return leftDeadZoneXSize; }
	float LeftDeadZoneYSize() const { return leftDeadZoneYSize; }
	float RightDeadZoneSize() const { return rightDeadZoneSize; }
	float RightDeadZoneXSize() const { return rightDeadZoneXSize; }
	float RightDeadZoneYSize() const { return rightDeadZoneYSize; }
	inline bool DisableTriggerTouch() { return disableTriggerTouch; }
	inline float TriggerDeadzone() const { return triggerDeadzone; }
	inline float TriggerMax() const { return triggerMax; }
	float HapticStrength() { return hapticStrength; }
	inline bool DisableTrackPad() { return disableTrackPad; }
	inline bool EnableControllerSmoothing() { return enableControllerSmoothing; }
	inline bool EnableVRIKKnucklesTrackPadSupport() { return enableVRIKKnucklesTrackPadSupport; }
	std::string KeyboardText() { return keyboardText; }

	// Controller model: "hands" or "quest3"
	const std::string& ControllerModel() const { return controllerModel; }

	// [keyboard] section
	bool KbShortcutEnabled() const { return kbShortcutEnabled; }
	const std::string& KbShortcutButton() const { return kbShortcutButton; }
	const std::string& KbShortcutMode() const { return kbShortcutMode; }
	int KbShortcutTiming() const { return kbShortcutTiming; }
	float KbDisplayTilt() const { return kbDisplayTilt; }
	int KbDisplayOpacity() const { return kbDisplayOpacity; }
	int KbDisplayScale() const { return kbDisplayScale; }
	bool KbSoundsEnabled() const { return kbSoundsEnabled; }
	int KbSoundVolume() const { return kbSoundVolume; }
	int KbHoverVolume() const { return kbHoverVolume; }
	int KbPressVolume() const { return kbPressVolume; }
	int KbHapticStrength() const { return kbHapticStrength; }

	float PosSmoothMinCutoff() { return posSmoothMinCutoff; }
	float RotSmoothMinCutoff() { return rotSmoothMinCutoff; }
	float PosSmoothBeta() { return posSmoothBeta; }
	float RotSmoothBeta() { return rotSmoothBeta; }

	inline bool EnableGpuTiming() const { return enableGpuTiming; }
	inline bool SwapThumbsticks() const { return swapThumbsticks; }

	inline bool DlaaEnabled() const { return dlaaEnabled; }
	inline float DlaaLambda() const { return dlaaLambda; }
	inline float DlaaEpsilon() const { return dlaaEpsilon; }

	// FSR upscaling
	inline bool FsrEnabled() const { return fsrEnabled; }
	inline bool FsrNativeAA() const { return fsrNativeAA; }
	inline float FsrRenderScale() const { return fsrRenderScale; }
	inline float Fsr3Sharpness() const { return fsr3Sharpness; }
	inline float Fsr3JitterScale() const { return fsr3JitterScale; }
	inline bool Fsr3JitterCancellation() const { return fsr3JitterCancellation; }
	inline float Fsr3ShadingChangeScale() const { return fsr3ShadingChangeScale; }
	inline float Fsr3ReactivenessScale() const { return fsr3ReactivenessScale; }
	inline float Fsr3AccumulationPerFrame() const { return fsr3AccumulationPerFrame; }
	inline float Fsr3MinDisocclusionAccumulation() const { return fsr3MinDisocclusionAccumulation; }
	inline float Fsr3VelocityFactor() const { return fsr3VelocityFactor; }
	inline float Fsr3ReactiveBase() const { return fsr3ReactiveBase; }
	inline float Fsr3ReactiveEdgeBoost() const { return fsr3ReactiveEdgeBoost; }
	inline float Fsr3ReactiveColorBoost() const { return fsr3ReactiveColorBoost; }
	inline float Fsr3ReactiveColorThreshold() const { return fsr3ReactiveColorThreshold; }
	inline float Fsr3ReactiveColorScale() const { return fsr3ReactiveColorScale; }
	inline float Fsr3ReactiveDepthFalloffStart() const { return fsr3ReactiveDepthFalloffStart; }
	inline float Fsr3ReactiveDepthFalloffEnd() const { return fsr3ReactiveDepthFalloffEnd; }
	inline bool Fsr3CameraMV() const { return fsr3CameraMV; }
	inline float Fsr3ViewToMeters() const { return fsr3ViewToMeters; }
	inline int Fsr3DebugMode() const { return fsr3DebugMode; }
	inline bool Fsr3PostAAEnabled() const { return fsr3PostAAEnabled; }
	inline float Fsr3PostAALambda() const { return fsr3PostAALambda; }
	inline float Fsr3PostAAEpsilon() const { return fsr3PostAAEpsilon; }
	inline bool BlueSkyDefenderEnabled() const { return blueSkyDefenderEnabled || fsr3PostAAEnabled; }
	inline float BlueSkyDefenderLambda() const { return blueSkyDefenderEnabled ? blueSkyDefenderLambda : fsr3PostAALambda; }
	inline float BlueSkyDefenderEpsilon() const { return blueSkyDefenderEnabled ? blueSkyDefenderEpsilon : fsr3PostAAEpsilon; }

	// DLSS 4 Super Resolution (NVIDIA only, native DX11 NGX)
	inline bool  DlssEnabled()        const { return dlssEnabled; }
	inline int   DlssPreset()         const { return dlssPreset; }    // 0=Quality 1=Balanced 2=Perf 3=UltraPerf 4=DLAA 5=UltraQuality
	inline float DlssRenderScaleOverride() const { return dlssRenderScaleOverride; } // 0=preset scale, >0 custom render scale
	inline const std::string& DlssModel() const { return dlssModel; }     // Optional alias: default/auto/0, or J/K/L/M
	inline int   DlssRenderPreset()   const { return dlssRenderPreset; } // 0=NGX default, 10=J, 11=K(default), 12=L, 13=M, 14+ pass through
	inline int   DlssModeOverride()   const { return dlssModeOverride; } // -1=unset, 0=off, 1=DLSS+DLISP, 2=DLISP only, 3=DLSS(default)
	inline bool  DlssNgxVerboseLogging() const { return dlssNgxVerboseLogging; }
	inline float DlssSharpness()      const { return dlssSharpness; }
	inline float DlssMvScale()        const { return dlssMvScale; }
	inline float DlssBiasBase()       const { return dlssBiasBase; }       // Depth-edge bias mask baseline (reduces thin-geometry ghosting)
	inline float DlssBiasEdgeBoost()  const { return dlssBiasEdgeBoost; }  // Extra bias at depth edges (foliage silhouettes)
	inline float DlssBiasDepthFalloffStart() const { return dlssBiasDepthFalloffStart; }
	inline float DlssBiasDepthFalloffEnd() const { return dlssBiasDepthFalloffEnd; }
	inline float DlssJitterScale()    const { return dlssJitterScale; }    // Jitter magnitude multiplier (lower = less ghosting, less detail)

	// Motion vectors (SKSE bridge → FSR3 / OCU ASW)
	inline bool MotionVectorsEnabled() const { return motionVectorsEnabled; }
	inline float MotionVectorScale() const { return motionVectorScale; }

	// Actor motion vectors (per-NPC rigid-body MVs from scene graph transforms)
	inline bool ActorMV() const { return actorMV; }

	// OCU ASW — Asynchronous SpaceWarp
	inline bool ASWEnabled() const { return aswEnabled; }
	inline bool ASWForceCustom() const { return aswForceCustom; } // Deprecated: ASW always uses OCU PC-side legacy/simple modes
	inline float ASWWarpStrength() const { return aswWarpStrength; }
	inline float ASWRotationScale() const { return aswRotationScale; }
	inline float ASWTranslationScale() const { return aswTranslationScale; }
	inline float ASWLocoScale() const { return aswLocoScale; }
	inline float ASWFPControllerScale() const { return aswFPControllerScale; }
	inline float ASWDepthScale() const { return aswDepthScale; }
	inline float ASWEdgeFadeWidth() const { return aswEdgeFadeWidth; }
	inline float ASWNearFadeDepth() const { return aswNearFadeDepth; }
	inline float ASWMVConfidence() const { return aswMVConfidence; }
	inline float ASWMVPixelScale() const { return aswMVPixelScale; }
	inline int ASWDebugMode() const { return aswDebugMode; }
	inline bool ASWCaptureEnabled() const { return aswCaptureEnabled; }
	inline bool ASWConcurrentFrameThread() const { return aswConcurrentFrameThread; }
	inline bool ASWSpeculativeTrackingLead() const { return aswSpeculativeTrackingLead; }
	inline bool ASWBufferEnabled() const { return aswBufferEnabled; }
	inline bool ASWUpscalerReset() const { return aswUpscalerReset; }
	inline bool ASWUpscalerReactiveMask() const { return aswUpscalerReactiveMask; }

	// CAS sharpening (RCAS) — independent of FSR
	inline bool CasEnabled() const { return casEnabled; }
	inline float CasSharpness() const { return casSharpness; }

	// FSR radius optimization
	inline bool FsrRadiusEnabled() const { return fsrRadiusEnabled; }
	inline float FsrRadius() const { return fsrRadius; }

	// MIP LOD bias correction
	inline bool MipBiasEnabled() const { return mipBiasEnabled; }

	// NVIDIA VRS foveated rendering
	inline bool VrsEnabled() const { return vrsEnabled; }
	inline float VrsInnerRadius() const { return vrsInnerRadius; }
	inline float VrsMidRadius() const { return vrsMidRadius; }
	inline float VrsOuterRadius() const { return vrsOuterRadius; }
	inline bool VrsFavorHorizontal() const { return vrsFavorHorizontal; }

	// ASW tuning variables — public for hot-reload from ini file watcher
	float aswWarpStrength = 1.0f;  // 0.0 = no warp (static copy), 1.0 = full correction
	float aswRotationScale = 0.0f; // 0.0 = no rotation correction, 1.0 = full
	float aswTranslationScale = 1.0f; // 0.0 = no translation correction, 1.0 = full
	float aswLocoScale = 1.0f;     // locomotion correction scale (0=off, 1=full). Multiplied by timingRatio (~0.5).
	float aswFPControllerScale = 0.63f; // FP hand/weapon controller delta scale (0=off, 0.63=tuned, 1=raw)
	float aswDepthScale = 1.0f;    // multiplier on linearized depth (parallax intensity)
	float aswEdgeFadeWidth = 3.0f;   // depth-edge fade threshold (depth ratio units)
	float aswNearFadeDepth = 0.0f;   // parallax fades to 0 below this depth (meters); 0 = disabled
	float aswMVConfidence = 1.5f;    // MV extrapolation scale: 1.5 = correct for N-1 warping (1.5 periods from cache to display). 0 = off.
	float aswMVPixelScale = 1.0f;    // overall MV magnitude multiplier (1.0 = identity)
	int aswDebugMode = 0;            // 0=normal, 1=depth viz, 2=linearized depth, 3=MV magnitude, 50=black warp frame, 56=stationary NPC dest-depth reject, 57=stationary NPC path overview
	bool aswCaptureEnabled = false;  // true = capture warp diagnostics (color/depth/MV/CB) to TestWarp folder
	bool aswForceLegacy = false;     // Deprecated: Alpha legacy ASW is now the default when aswExperimentalMode=false
	bool aswExperimentalMode = false; // true = experimental ASW: single-pass parallax + game MV residual correction + depth-based FP mask + frame-N disocclusion fallback

private:
	static int ini_handler(
	    void* user, const char* section,
	    const char* name, const char* value,
	    int lineno);

	bool renderCustomHands = true;
	vr::HmdColor_t handColour = vr::HmdColor_t{ 0.3f, 0.3f, 0.3f, 1 };
	float supersampleRatio = 1.0f;
	bool haptics = true;
	bool admitUnknownProps = false;
	bool useViewportStencil = false;
	bool forceConnectedTouch = true;
	bool logGetTrackedProperty = false;
	bool stopOnSoftAbort = false;

	// Default to false since this was preventing PAYDAY 2 from starting, need to investigate to find out
	//  if this is game-specific, or if it's a problem with the layer system
	bool enableLayers = true;

	bool dx10Mode = false;
	bool enableAppRequestedCubemap = true;
	bool enableHiddenMeshFix = true;
	bool invertUsingShaders = false;
	bool initUsingVulkan = false;
	float hiddenMeshVerticalScale = 1.0f;
	bool logAllOpenVRCalls = false;
	bool enableAudioSwitch = false;
	std::string audioDeviceName = "";
	bool enableInputSmoothing = false;
	int inputWindowSize = 5;
	bool adjustTilt = false;

	bool adjustLeftRotation = false;
	bool adjustRightRotation = false;
	bool adjustLeftPosition = false;
	bool adjustRightPosition = false;

	float tilt = 0.0f;

	float leftXRotation = 0.0f;
	float leftYRotation = 0.0f;
	float leftZRotation = 0.0f;
	float rightXRotation = 0.0f;
	float rightYRotation = 0.0f;
	float rightZRotation = 0.0f;

	float leftXPosition = 0.0f;
	float leftYPosition = 0.0f;
	float leftZPosition = 0.0f;
	float rightXPosition = 0.0f;
	float rightYPosition = 0.0f;
	float rightZPosition = 0.0f;

	float leftDeadZoneSize = 0.0f;
	float leftDeadZoneXSize = 0.0f;
	float leftDeadZoneYSize = 0.0f;
	float rightDeadZoneSize = 0.0f;
	float rightDeadZoneXSize = 0.0f;
	float rightDeadZoneYSize = 0.0f;
	bool disableTriggerTouch = false;
	float triggerDeadzone = 0.0f;   // raw trigger value below which output = 0
	float triggerMax = 1.0f;        // raw trigger value at which output = 1.0 (for worn controllers)
	float hapticStrength = 0.1f;
	bool disableTrackPad = false;
	bool enableControllerSmoothing = false;
	bool enableVRIKKnucklesTrackPadSupport = false;
	bool swapThumbsticks = false; // Swap left/right thumbstick axes (movement ↔ look)
	float posSmoothMinCutoff = 1.25;
	float posSmoothBeta = 20;
	float rotSmoothMinCutoff = 1.5;
	float rotSmoothBeta = 0.2;
	std::string keyboardText = "";
	std::string controllerModel = "hands";
	bool enableGpuTiming = true;

	bool dlaaEnabled = false;
	float dlaaLambda = 3.0f;        // edge detection sensitivity (1.0-6.0)
	float dlaaEpsilon = 0.1f;       // luminance threshold offset (0.01-0.50)

	// FSR upscaling
	bool fsrEnabled = false;
	bool fsrNativeAA = false;       // Run FSR3 at native resolution for temporal AA without upscaling
	float fsrRenderScale = 0.67f;   // 0.5 - 1.0, lower = more GPU savings
	float fsr3Sharpness = 0.3f;     // 0.0 - 1.0, FSR3 built-in RCAS sharpness
	float fsr3JitterScale = 0.3f;   // 0.0 - 1.0, jitter amplitude (lower = more stable, higher = better temporal AA)
	bool fsr3JitterCancellation = false; // Camera MVs cancel jitter in shader; game MVs don't include jitter
	float fsr3ShadingChangeScale = 2.0f; // Higher = more reactive to shading changes (reduces ghosting on trees)
	float fsr3ReactivenessScale = 2.0f; // Multiplier on reactive mask values (higher = more aggressive ghosting reduction)
	float fsr3AccumulationPerFrame = 0.20f; // Lower = less ghosting but more flicker on thin geometry (0.0-1.0)
	float fsr3MinDisocclusionAccumulation = -0.333f; // Higher = less flicker on swaying thin objects (-1.0 to 1.0)
	float fsr3VelocityFactor = 1.0f; // 0.0 = improve temporal stability of bright pixels (FFX default 1.0)
	float fsr3ReactiveBase = 0.05f;    // Depth-edge reactive mask baseline (reduces thin-geometry ghosting)
	float fsr3ReactiveEdgeBoost = 0.10f; // Extra reactiveness at depth edges (tree silhouettes, thin geometry)
	float fsr3ReactiveColorBoost = 0.15f; // Extra reactiveness for high-frequency foliage-like color detail
	float fsr3ReactiveColorThreshold = 0.08f; // Luma contrast before color reactiveness starts
	float fsr3ReactiveColorScale = 8.0f; // Ramp speed for color-edge reactiveness
	float fsr3ReactiveDepthFalloffStart = 0.95f; // Depth where reactive mask begins fading (standard-Z, 0=near 1=far)
	float fsr3ReactiveDepthFalloffEnd = 0.998f;  // Depth where reactive mask reaches zero (distant mountains/sky)
	bool fsr3CameraMV = true;          // Camera MVs from depth + view-projection deltas (captures locomotion + head tracking)
	float fsr3ViewToMeters = 0.01428f;  // Skyrim: ~70 units = 1 meter
	int fsr3DebugMode = 0;             // 0=off, 1=FSR3 debug overlay, 2=bypass, 3=depth, 4=final MV, 5=residual MV, 6=raw bridge MV, 7=bridge fallback mask, 8=reactive mask
	bool fsr3PostAAEnabled = false;    // Optional post-FSR spatial AA pass for testing foliage shimmer
	float fsr3PostAALambda = 3.0f;     // Edge sensitivity for FSR3 post-AA
	float fsr3PostAAEpsilon = 0.10f;   // Luminance threshold for FSR3 post-AA
	bool blueSkyDefenderEnabled = false; // BlueSkyDefender spatial AA after FSR3 output
	float blueSkyDefenderLambda = 3.0f;  // Edge sensitivity for BlueSkyDefender post-AA
	float blueSkyDefenderEpsilon = 0.10f; // Luminance threshold for BlueSkyDefender post-AA

	// Motion vectors (SKSE bridge → FSR3 / OCU ASW)
	bool motionVectorsEnabled = true;
	float motionVectorScale = 1.0f;    // MV magnitude multiplier (1.0 = raw, <1 = dampen, >1 = amplify)

	// Actor motion vectors (per-NPC rigid-body MVs from scene graph transforms)
	bool actorMV = true;               // Enable per-actor motion vectors for nearby NPCs

	// OCU ASW — PC-side Asynchronous SpaceWarp (experimental)
	bool aswEnabled = false;
	bool aswForceCustom = false;           // Deprecated: ASW always uses OCU PC-side legacy/simple modes
	bool aswConcurrentFrameThread = false; // If true, dedicated XR thread owns wait/begin/end in buffered mode
	bool aswSpeculativeTrackingLead = false; // Diagnostic default: prefer begun-slot sync over speculative +1 period lead
	bool aswBufferEnabled = false;           // false = Alpha-style inline ASW; true = experimental split-frame warp scheduling
	bool aswUpscalerReset = false;           // If true, reset DLSS/FSR warp-upscale temporal history each ASW frame
	bool aswUpscalerReactiveMask = true;     // If true, bias ASW warp-upscale history at warped depth/color edges
	// NOTE: aswWarpStrength, aswRotationScale, aswTranslationScale, aswDepthScale
	// are declared in the public section above for hot-reload access

	// CAS sharpening (RCAS) — independent of FSR
	bool casEnabled = false;
	float casSharpness = 0.5f;  // CAS sharpness (0.0-1.0, lower = sharper)
	float fsrSharpness = 0.2f;      // 0.0 - 1.0, higher = sharper

	// FSR radius optimization
	bool fsrRadiusEnabled = false;
	float fsrRadius = 0.60f;        // fraction of screen height

	// MIP LOD bias correction
	bool mipBiasEnabled = true;

	// NVIDIA VRS foveated rendering
	bool vrsEnabled = false;
	float vrsInnerRadius = 0.60f;
	float vrsMidRadius = 0.80f;
	float vrsOuterRadius = 1.00f;
	bool vrsFavorHorizontal = true;

	// DLSS 4 Super Resolution (NVIDIA only, native DX11 NGX)
	bool  dlssEnabled      = false;
	int   dlssPreset       = 1;      // 0=Quality 1=Balanced 2=Perf 3=UltraPerf 4=DLAA 5=UltraQuality
	float dlssRenderScaleOverride = 0.0f; // 0=preset scale, >0 custom render scale
	std::string dlssModel;           // Optional friendly alias for dlssRenderPreset: default/auto/0, J, K, L, M
	int   dlssRenderPreset = 11;     // 0=NGX default, 10=J, 11=K(default), 12=L, 13=M, 14+ pass through
	int   dlssModeOverride = 3;      // -1=unset, 0=off, 1=DLSS+DLISP, 2=DLISP only, 3=DLSS(default)
	bool  dlssNgxVerboseLogging = false;
	float dlssSharpness    = 20.0f;
	float dlssMvScale      = 1.0f;   // Uniform camera MV scale for DLSS (1.0 = no correction)
	float dlssBiasBase     = 0.20f;  // Depth-edge bias mask baseline (reduces thin-geometry ghosting)
	float dlssBiasEdgeBoost = 0.50f; // Extra bias at depth edges (foliage silhouettes)
	float dlssBiasDepthFalloffStart = 0.95f; // Depth where bias mask begins fading (standard-Z, 0=near 1=far)
	float dlssBiasDepthFalloffEnd = 0.99f;   // Depth where bias mask reaches zero (distant mountains/sky)
	float dlssJitterScale  = 0.4f;   // Jitter magnitude multiplier (lower = less ghosting, less detail)

	// [keyboard] section
	bool kbShortcutEnabled = true;
	std::string kbShortcutButton = "left_stick";
	std::string kbShortcutMode = "double_tap";
	int kbShortcutTiming = 500;
	float kbDisplayTilt = 22.5f;
	int kbDisplayOpacity = 30;
	int kbDisplayScale = 100;
	bool kbSoundsEnabled = true;
	int kbSoundVolume = 50;      // 0-100%
	int kbHoverVolume = 50;      // 0-100%
	int kbPressVolume = 50;      // 0-100%
	int kbHapticStrength = 50;   // 0-100%
};

extern Config oovr_global_configuration;
