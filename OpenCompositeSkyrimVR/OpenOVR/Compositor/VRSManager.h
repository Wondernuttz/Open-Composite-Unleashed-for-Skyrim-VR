#pragma once
#include "../Misc/FoveationRates.h"
#include "../Misc/FoveationBlackout.h"

#ifdef OC_HAS_NVAPI

#include <d3d11.h>
#include <vector>
#include "VRSSceneScope.h"

class VRSManager {
public:
	struct EyeRegion {
		int left = 0;
		int top = 0;
		int width = 0;
		int height = 0;
	};

	VRSManager() = default;
	~VRSManager();

	// Initialize NVAPI and check VRS support. Returns true if VRS is available.
	bool Initialize(ID3D11Device* device);

	// Set projection centers for each eye (normalized 0-1 coordinates).
	// Call for each new projection or eye-gaze sample.
	void SetProjectionCenters(float leftProjX, float leftProjY, float rightProjX, float rightProjY);
	void SetHorizontalScale(float scale);
	// The caller supplies a frame-latched mask only after presentation and
	// downstream consumers are ready. Configuration alone must not enable it.
	void SetBlackout(const ocu_foveation::Blackout& mask);

	// Create/update one shading-rate resource for the full bound stereo render
	// target. NVIDIA requires this resource to match the complete render target
	// in 16x16 tiles; a one-eye resource is invalid for a side-by-side atlas.
	bool UpdateStereoPattern(int renderWidth, int renderHeight,
	    const EyeRegion& leftEye, const EyeRegion& rightEye, float innerRadius, float midRadius,
	    const ocu_foveation::RingRates& rates);

	// Apply the full stereo-atlas pattern before the game starts drawing a frame.
	bool ApplyStereo();

	// Match a recognized active stereo viewport while keeping the full-target
	// rate resource. Unknown subrect semantics withhold VRS until they recover.
	bool UpdateActiveViewports(UINT count, const D3D11_VIEWPORT* viewports);
	struct PatternUpdates { unsigned resourceCreations = 0, uploads = 0; };
	PatternUpdates GetPatternUpdates() const { return patternUpdates; }

	// Disable VRS. Call before our own post-processing (FSR passes).
	void Disable();

	// Clean up all resources.
	void Shutdown();

	// Returns true if GPU supports VRS and initialization succeeded.
	bool IsAvailable() const { return available; }

	// Initialization is attempted at most once for a D3D device session. This
	// prevents unsupported adapters from retrying NVAPI every compositor frame.
	bool WasInitializationAttempted() const { return initializationAttempted; }

private:
	friend struct VRSBlackoutTestAccess;
	bool available = false;
	bool initializationAttempted = false;

	ID3D11Device* device = nullptr;
	ID3D11DeviceContext* context = nullptr;

	// One resource matching the full stereo render target.
	ID3D11Texture2D* vrsTex = nullptr;
	void* vrsView = nullptr; // ID3D11NvShadingRateResourceView* — opaque to avoid nvapi.h in header
	int patternWidth = 0;
	int patternHeight = 0;
	int renderWidth = 0;
	int renderHeight = 0;
	EyeRegion eyeRegions[2];
	ocu_vrs_scope::ViewportEyeRegion activeEyeRegions[2];
	bool activeViewportValid = true;
	PatternUpdates patternUpdates;

	// Projection centers per eye
	float projX[2] = { 0.5f, 0.5f };
	float projY[2] = { 0.5f, 0.5f };
	// Dirty detection must accumulate movement since the last actual upload,
	// not since the previous sample (which can drift forever in tiny steps).
	float uploadedProjX[2] = { 0.5f, 0.5f };
	float uploadedProjY[2] = { 0.5f, 0.5f };
	bool patternDirty = true;

	// Cached config values used to detect changes
	float cachedInnerRadius = 0.0f;
	float cachedMidRadius = 0.0f;
	float horizontalScale = 1.0f;
	ocu_foveation::Blackout blackout;
	ocu_foveation::RingRates cachedRates;
	bool shadingRatesSet = false; // True after EnableShadingRates() — avoid redundant NVAPI calls

	// Generate a full-target pattern containing both eye regions.
	std::vector<uint8_t> CreateStereoPattern() const;

	// Set the shading rate table on the device context
	bool EnableShadingRates();

	// Create the full-target pattern texture and NVAPI resource view.
	void SetupStereoPattern();
	// Upload moving gaze centers without recreating the resource view.
	void UploadStereoPattern();

	void ReleasePatternResources();
};

#else // !OC_HAS_NVAPI

// Stub when NVAPI is not available — all methods are no-ops
class VRSManager {
public:
	struct EyeRegion {
		int left = 0;
		int top = 0;
		int width = 0;
		int height = 0;
	};

	bool Initialize(ID3D11Device*) { return false; }
	void SetProjectionCenters(float, float, float, float) {}
	void SetHorizontalScale(float) {}
	void SetBlackout(const ocu_foveation::Blackout&) {}
	bool UpdateStereoPattern(int, int, const EyeRegion&, const EyeRegion&, float, float,
	    const ocu_foveation::RingRates&) { return false; }
	bool ApplyStereo() { return false; }
	bool UpdateActiveViewports(UINT, const D3D11_VIEWPORT*) { return false; }
	void Disable() {}
	void Shutdown() {}
	bool IsAvailable() const { return false; }
	bool WasInitializationAttempted() const { return true; }
};

#endif // OC_HAS_NVAPI
