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

	inline bool DlaaEnabled() const { return dlaaEnabled; }
	inline float DlaaLambda() const { return dlaaLambda; }
	inline float DlaaEpsilon() const { return dlaaEpsilon; }

	// FSR upscaling
	inline bool FsrEnabled() const { return fsrEnabled; }
	inline float FsrRenderScale() const { return fsrRenderScale; }
	inline float Fsr3Sharpness() const { return fsr3Sharpness; }
	inline float Fsr3JitterScale() const { return fsr3JitterScale; }
	inline bool Fsr3JitterCancellation() const { return fsr3JitterCancellation; }
	inline float Fsr3ViewToMeters() const { return fsr3ViewToMeters; }

	// Motion vectors (SKSE bridge → FSR3 / OCU ASW)
	inline bool MotionVectorsEnabled() const { return motionVectorsEnabled; }
	inline float MotionVectorScale() const { return motionVectorScale; }

	// OCU ASW — PC-side Asynchronous SpaceWarp (experimental)
	inline bool ASWEnabled() const { return aswEnabled; }
	inline float ASWWarpStrength() const { return aswWarpStrength; }
	inline float ASWRotationScale() const { return aswRotationScale; }
	inline float ASWTranslationScale() const { return aswTranslationScale; }
	inline float ASWDepthScale() const { return aswDepthScale; }

	// CAS sharpening (RCAS) — independent of FSR
	inline bool CasEnabled() const { return casEnabled; }
	inline float CasSharpness() const { return fsrSharpness; }

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
	float aswRotationScale = 1.0f; // 0.0 = no rotation correction, 1.0 = full
	float aswTranslationScale = 1.0f; // 0.0 = no translation correction, 1.0 = full
	float aswDepthScale = 1.0f;    // multiplier on linearized depth (parallax intensity)

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
	float hapticStrength = 0.1f;
	bool disableTrackPad = false;
	bool enableControllerSmoothing = false;
	bool enableVRIKKnucklesTrackPadSupport = false;
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
	float fsrRenderScale = 0.77f;   // 0.5 - 1.0, lower = more GPU savings
	float fsr3Sharpness = 0.5f;     // 0.0 - 1.0, FSR3 built-in RCAS sharpness
	float fsr3JitterScale = 1.0f;   // 0.0 - 1.0, jitter amplitude (lower = more stable)
	bool fsr3JitterCancellation = true; // MVs include jitter — tell FSR3 to compensate
	float fsr3ViewToMeters = 0.01428f;  // Skyrim: ~70 units = 1 meter

	// Motion vectors (SKSE bridge → FSR3 / OCU ASW)
	bool motionVectorsEnabled = true;
	float motionVectorScale = 1.0f;    // MV magnitude multiplier (1.0 = raw, <1 = dampen, >1 = amplify)

	// OCU ASW — PC-side Asynchronous SpaceWarp (experimental)
	bool aswEnabled = false;
	// NOTE: aswWarpStrength, aswRotationScale, aswTranslationScale, aswDepthScale
	// are declared in the public section above for hot-reload access

	// CAS sharpening (RCAS) — independent of FSR
	bool casEnabled = false;
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
