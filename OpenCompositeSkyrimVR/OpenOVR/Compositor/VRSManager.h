#pragma once

#ifdef OC_HAS_NVAPI

#include <d3d11.h>
#include <vector>

class VRSManager {
public:
	VRSManager() = default;
	~VRSManager();

	// Initialize NVAPI and check VRS support. Returns true if VRS is available.
	bool Initialize(ID3D11Device* device);

	// Set projection centers for each eye (normalized 0-1 coordinates).
	// Call this once when eye projection data is available.
	void SetProjectionCenters(float leftProjX, float leftProjY, float rightProjX, float rightProjY);

	// Create/recreate VRS pattern textures for the given eye resolution.
	// Call when resolution changes or radii config changes.
	void UpdatePatterns(int eyeWidth, int eyeHeight);

	// Apply VRS for a specific eye (0=left, 1=right). Call before game renders each eye.
	void ApplyForEye(int eye);

	// Disable VRS. Call before our own post-processing (FSR passes).
	void Disable();

	// Clean up all resources.
	void Shutdown();

	// Returns true if GPU supports VRS and initialization succeeded.
	bool IsAvailable() const { return available; }

private:
	bool available = false;
	bool nvapiLoaded = false;

	ID3D11Device* device = nullptr;
	ID3D11DeviceContext* context = nullptr;

	// Per-eye VRS pattern textures and views
	ID3D11Texture2D* vrsTex[2] = { nullptr, nullptr };
	void* vrsView[2] = { nullptr, nullptr }; // ID3D11NvShadingRateResourceView* — opaque to avoid nvapi.h in header
	int patternWidth[2] = { 0, 0 };
	int patternHeight[2] = { 0, 0 };

	// Projection centers per eye
	float projX[2] = { 0.5f, 0.5f };
	float projY[2] = { 0.5f, 0.5f };

	// Cached config values used to detect changes
	float cachedInnerRadius = 0.0f;
	float cachedMidRadius = 0.0f;
	float cachedOuterRadius = 0.0f;
	bool cachedFavorHorizontal = true;
	bool shadingRatesSet = false; // True after EnableShadingRates() — avoid redundant NVAPI calls

	// Generate the VRS pattern data for one eye
	std::vector<uint8_t> CreatePattern(int tileWidth, int tileHeight, float pX, float pY);

	// Set the shading rate table on the device context
	void EnableShadingRates();

	// Create the pattern texture and NVAPI shading rate resource view for one eye
	void SetupEyePattern(int eye, int eyeWidth, int eyeHeight);

	// Release resources for one eye
	void ReleaseEyeResources(int eye);
};

#else // !OC_HAS_NVAPI

// Stub when NVAPI is not available — all methods are no-ops
class VRSManager {
public:
	bool Initialize(ID3D11Device*) { return false; }
	void SetProjectionCenters(float, float, float, float) {}
	void UpdatePatterns(int, int) {}
	void ApplyForEye(int) {}
	void Disable() {}
	void Shutdown() {}
	bool IsAvailable() const { return false; }
};

#endif // OC_HAS_NVAPI
