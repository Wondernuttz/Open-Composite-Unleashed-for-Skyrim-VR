#pragma once

#include "XrDriverPrivate.h"
#include <d3d11.h>
#include <vector>

// Forward declaration — ASWProvider is accessed from XrBackend for frame injection
class ASWProvider;
extern ASWProvider* g_aswProvider;

class ASWProvider {
public:
	ASWProvider() = default;
	~ASWProvider();

	ASWProvider(const ASWProvider&) = delete;
	ASWProvider& operator=(const ASWProvider&) = delete;

	/// Initialize: compile compute shader, create staging textures + output swapchains.
	/// @param device   D3D11 device (from compositor)
	/// @param eyeWidth  Per-eye render width
	/// @param eyeHeight Per-eye render height
	bool Initialize(ID3D11Device* device, uint32_t eyeWidth, uint32_t eyeHeight);
	void Shutdown();
	bool IsReady() const { return m_ready; }

	/// Cache current frame's data for warping next cycle.
	/// Call on each eye during the real frame's Invoke.
	void CacheFrame(int eye, ID3D11DeviceContext* ctx,
	    ID3D11Texture2D* colorTex, const D3D11_BOX* colorRegion,
	    ID3D11Texture2D* mvTex, const D3D11_BOX* mvRegion,
	    ID3D11Texture2D* depthTex, const D3D11_BOX* depthRegion,
	    const XrPosef& eyePose, const XrFovf& eyeFov,
	    float nearZ, float farZ);

	/// Warp cached frame to new pose, write result to output texture.
	/// Call for each eye during the injected frame.
	bool WarpFrame(int eye, ID3D11DeviceContext* ctx,
	    const XrPosef& newPose);

	/// Get the output XR swapchain for the warped frame (for layer assembly).
	XrSwapchain GetOutputSwapchain() const { return m_outputSwapchain; }

	/// Get the depth XR swapchain for the warped frame (for XR_KHR_composition_layer_depth).
	XrSwapchain GetDepthSwapchain() const { return m_depthSwapchain; }

	/// Get per-eye sub-image rect for the warped output (stereo-combined).
	XrRect2Di GetOutputRect(int eye) const;

	/// Acquire output swapchain, copy warped textures, release.
	/// Call once after WarpFrame for both eyes.
	bool SubmitWarpedOutput(ID3D11DeviceContext* ctx);

	bool HasCachedFrame() const { return m_hasCachedFrame; }

	/// Cached pose/FOV for building projection views during injection
	XrPosef GetCachedPose(int eye) const { return m_cachedPose[eye]; }
	XrFovf GetCachedFov(int eye) const { return m_cachedFov[eye]; }

	/// Cached near/far for depth layer submission
	float GetCachedNear() const { return m_cachedNear; }
	float GetCachedFar() const { return m_cachedFar; }

	/// Get the D3D11 device used during initialization (for obtaining context in XrBackend)
	ID3D11Device* GetDevice() const { return m_device; }

private:
	bool CreateComputeShader(ID3D11Device* device);
	bool CreateStagingTextures(ID3D11Device* device);
	bool CreateOutputSwapchain(uint32_t width, uint32_t height);
	bool CreateDepthSwapchain(uint32_t width, uint32_t height);

	// Quaternion math helpers
	static void QuatInverse(const XrQuaternionf& q, XrQuaternionf& out);
	static void QuatMultiply(const XrQuaternionf& a, const XrQuaternionf& b, XrQuaternionf& out);
	static void QuatRotateVec(const XrQuaternionf& q, const XrVector3f& v, XrVector3f& out);
	static void BuildPoseDeltaMatrix(const XrPosef& oldPose, const XrPosef& newPose,
	    float* outMatrix4x4);

	bool m_ready = false;
	uint32_t m_eyeWidth = 0, m_eyeHeight = 0;
	ID3D11Device* m_device = nullptr; // kept for obtaining immediate context in XrBackend

	// Compute shader
	ID3D11ComputeShader* m_warpCS = nullptr;
	ID3D11Buffer* m_constantBuffer = nullptr;
	ID3D11SamplerState* m_linearSampler = nullptr;

	// Per-eye cached textures (staging copies of game frame data)
	ID3D11Texture2D* m_cachedColor[2] = {};
	ID3D11Texture2D* m_cachedMV[2] = {};
	ID3D11Texture2D* m_cachedDepth[2] = {};
	ID3D11ShaderResourceView* m_srvColor[2] = {};
	ID3D11ShaderResourceView* m_srvMV[2] = {};
	ID3D11ShaderResourceView* m_srvDepth[2] = {};

	// Per-eye warped output (compute shader writes here)
	ID3D11Texture2D* m_warpedOutput[2] = {};
	ID3D11UnorderedAccessView* m_uavOutput[2] = {};

	// Stereo-combined XR swapchain for warped output (both eyes side-by-side)
	XrSwapchain m_outputSwapchain = {};
	std::vector<ID3D11Texture2D*> m_outputSwapchainImages; // all swapchain images

	// Stereo-combined XR swapchain for depth (R32_FLOAT, both eyes side-by-side)
	XrSwapchain m_depthSwapchain = {};
	std::vector<ID3D11Texture2D*> m_depthSwapchainImages;

	// Cached pose/FOV/depth from real frame
	XrPosef m_cachedPose[2] = {};
	XrFovf m_cachedFov[2] = {};
	float m_cachedNear = 0.1f;
	float m_cachedFar = 10000.0f;
	bool m_hasCachedFrame = false;

	// Constant buffer layout (must match HLSL, 16-byte aligned = 112 bytes)
	struct WarpConstants {
		float poseDeltaMatrix[16]; // 4x4 row-major
		float resolution[2];
		float nearZ, farZ;
		float fovTanLeft, fovTanRight, fovTanUp, fovTanDown;
		float depthScale;          // multiplier on linearized depth
		float _pad[3];             // pad to 16-byte boundary
	};
};
