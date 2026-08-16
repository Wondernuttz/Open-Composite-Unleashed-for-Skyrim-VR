#pragma once

#include "XrDriverPrivate.h"
#include <d3d11.h>
#include <vector>

struct FPReplayData;

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
	bool Initialize(ID3D11Device* device, uint32_t renderWidth, uint32_t renderHeight,
	    uint32_t outputWidth, uint32_t outputHeight)
	{
		(void)outputWidth;
		(void)outputHeight;
		return Initialize(device, renderWidth, renderHeight);
	}
	void Shutdown();
	bool IsReady() const { return m_ready; }
	void TryFinishShaderCompilation() {}

	/// Cache current frame's data for warping next cycle.
	/// Call on each eye during the real frame's Invoke. Returns true when this
	/// eye was cached successfully; eye 1 only succeeds after eye 0 from the
	/// same cache generation has also succeeded.
	bool CacheFrame(int eye, ID3D11DeviceContext* ctx,
	    ID3D11Texture2D* colorTex, const D3D11_BOX* colorRegion,
	    bool sourceFlipV,
	    ID3D11Texture2D* mvTex, const D3D11_BOX* mvRegion,
	    ID3D11Texture2D* depthTex, const D3D11_BOX* depthRegion,
	    const XrPosef& eyePose, const XrFovf& eyeFov,
	    float nearZ, float farZ);

	/// Warp cached frame to new pose, write result to output texture.
	/// Call for each eye during the injected frame.
	bool WarpFrame(int eye, ID3D11DeviceContext* ctx,
	    const XrPosef& newPose);
	bool WarpFrame(int eye, const XrPosef& newPose, int slotOverride = -1,
	    XrTime warpDisplayTime = 0)
	{
		(void)slotOverride;
		(void)warpDisplayTime;
		ID3D11DeviceContext* ctx = nullptr;
		if (m_device)
			m_device->GetImmediateContext(&ctx);
		if (!ctx)
			return false;
		bool ok = WarpFrame(eye, ctx, newPose);
		ctx->Release();
		return ok;
	}

	/// Get the output XR swapchain for the warped frame (for layer assembly).
	XrSwapchain GetOutputSwapchain() const { return m_outputSwapchain; }

	/// Get the depth XR swapchain for the warped frame (for XR_KHR_composition_layer_depth).
	XrSwapchain GetDepthSwapchain() const { return m_depthSwapchain; }

	/**
	 * Answer Compositor::InvalidateSwapchains for the chains this provider owns independently of
	 * the eye compositor. Only the XR swapchains: the compute shader and staging textures are
	 * unaffected by anything downstream.
	 *
	 * False means a needed rebuild failed and ASW has switched itself off; skip the frame.
	 */
	bool RebuildSwapchainsIfInvalidated();

	/// False while the depth cache runs at a different resolution than the eye
	/// (external render scale). The parallax warp still works (shader samples
	/// depth by UV), or when reversed submit bounds require shader-side depth
	/// sampling that the unchanged XR depth swapchain cannot represent.
	bool DepthLayerValid() const
	{
		return m_depthLayerValid && !m_cachedSourceFlipV[0] && !m_cachedSourceFlipV[1];
	}

	/// Get per-eye sub-image rect for the warped output (stereo-combined).
	XrRect2Di GetOutputRect(int eye) const;

	/// Acquire output swapchain, copy warped textures, release.
	/// Call once after WarpFrame for both eyes.
	bool SubmitWarpedOutput(ID3D11DeviceContext* ctx);
	bool SubmitBlackOutput(ID3D11DeviceContext* ctx) { (void)ctx; return false; }

	bool HasCachedFrame() const { return m_hasCachedFrame; }
	bool HasPreviousCachedFrame() const { return false; }
	int GetPublishedSlot() const { return m_hasCachedFrame ? 0 : -1; }
	int GetBuildSlot() const { return 0; }
	void SetSlotDisplayTime(int slot, XrTime t) { (void)slot; (void)t; }
	void InvalidateCachedFrame()
	{
		m_hasCachedFrame = false;
		m_cacheBuildEyeMask = 0;
	}

	/// Cached pose/FOV for building projection views during injection
	XrPosef GetCachedPose(int eye) const { return m_cachedPose[eye]; }
	XrPosef GetPrecompPose(int eye) const { return m_cachedPose[eye]; }
	XrFovf GetCachedFov(int eye) const { return m_cachedFov[eye]; }

	/// Cached near/far for depth layer submission
	float GetCachedNear() const { return m_cachedNear; }
	float GetCachedFar() const { return m_cachedFar; }

	/// Get the D3D11 device used during initialization (for obtaining context in XrBackend)
	ID3D11Device* GetDevice() const { return m_device; }

	struct WarpUpscaleParams {
		int eye;
		ID3D11DeviceContext* ctx;
		ID3D11Texture2D* warpedColor;
		ID3D11Texture2D* cachedDepth;
		uint32_t renderW, renderH;
		uint32_t outputW, outputH;
		float nearZ, farZ;
		XrPosef cachedPose;
		XrPosef warpPose;
		XrFovf cachedFov;
		float poseDeltaMatrix[16];
	};
	using WarpUpscaleCallback = bool (*)(const WarpUpscaleParams& params, ID3D11Texture2D** outResult);
	void SetWarpUpscaleCallback(WarpUpscaleCallback cb) { (void)cb; }
	bool HasWarpUpscaleCallback() const { return false; }
	ID3D11Texture2D* GetWarpMVTex(int eye) const { (void)eye; return nullptr; }

	void CacheStencil(int eye, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, const D3D11_BOX* region)
	{
		(void)eye; (void)ctx; (void)tex; (void)region;
	}
	void CachePreFPDepth(int eye, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, const D3D11_BOX* region)
	{
		(void)eye; (void)ctx; (void)tex; (void)region;
	}
	void SetFPDepthTex(ID3D11Texture2D* tex) { (void)tex; }
	void SetClipToClipNoLoco(int eye, const float* m) { (void)eye; (void)m; }
	void SetClipToClipWithLoco(int eye, const float* m) { (void)eye; (void)m; }
	void SetSlotControllerUV(int eye, const float* leftUV, const float* rightUV, float radius)
	{
		(void)eye; (void)leftUV; (void)rightUV; (void)radius;
	}
	void SetSlotCameraPosZ(float z) { (void)z; }
	void SetCameraPosPtr(uint64_t ptr) { (void)ptr; }
	void SetFirstPersonRootPtr(uint64_t ptr) { (void)ptr; }
	void SetRSSViewMatPtr(const float* ptr) { (void)ptr; }
	void SetFPReplayPtr(FPReplayData* ptr) { (void)ptr; }
	void SetMenuOpen(bool open) { (void)open; }
	// View-space camera delta per game frame (new − old, game units) from dx11compositor
	void SetLocomotionTranslation(float x, float y, float z) { m_locoX = x; m_locoY = y; m_locoZ = z; }
	void SetLocomotionYaw(float yaw) { m_locoYaw = yaw; }
	// Where this warp sits in the game frame (slot i of n → (i+1)/(n+1)); scales loco shift
	void SetWarpSlotFraction(float f) { m_slotFraction = f; }
	void SetMVConfidenceScale(float scale) { (void)scale; }
	float GetMVConfidenceScale() const { return 1.0f; }
	void SetControllerPos(int hand, float x, float y, float z, bool valid)
	{
		(void)hand; (void)x; (void)y; (void)z; (void)valid;
	}
	bool GetControllerValid(int hand) const { (void)hand; return false; }
	const float* GetControllerPos(int hand) const
	{
		(void)hand;
		static float zero[3] = {};
		return zero;
	}
	void SetPaused(bool paused) { m_paused = paused; }
	bool IsPaused() const { return m_paused; }

	// Auto-native: XrBackend signals whether injection will run; compositor skips
	// CacheFrame copies when not. Disabling invalidates the cache so re-engage
	// never warps a stale frame.
	void SetInjectionWanted(bool wanted)
	{
		if (m_injectionWanted && !wanted)
			InvalidateCachedFrame();
		m_injectionWanted = wanted;
	}
	bool IsInjectionWanted() const { return m_injectionWanted; }

private:
	bool CreateComputeShader(ID3D11Device* device);
	bool CreateStagingTextures(ID3D11Device* device);
	bool CreateOutputSwapchain(uint32_t width, uint32_t height);
	bool CreateDepthSwapchain(uint32_t width, uint32_t height);

	// Adaptive sizing (external render-scale support, e.g. Community Shaders VR):
	// the color/output path follows the submitted frame size; the depth cache
	// follows the game's depth target size. They may legitimately differ.
	void ReleaseStagingTextures();
	bool ResizeColorPath(uint32_t eyeW, uint32_t eyeH);
	bool ResizeDepthCache(uint32_t w, uint32_t h);

	// Quaternion math helpers
	static void QuatInverse(const XrQuaternionf& q, XrQuaternionf& out);
	static void QuatMultiply(const XrQuaternionf& a, const XrQuaternionf& b, XrQuaternionf& out);
	static void QuatRotateVec(const XrQuaternionf& q, const XrVector3f& v, XrVector3f& out);
	static void BuildPoseDeltaMatrix(const XrPosef& oldPose, const XrPosef& newPose,
	    float* outMatrix4x4);

	bool m_ready = false;
	bool m_paused = false;
	bool m_injectionWanted = true;
	// Stick locomotion delta for warp correction (view space, new − old, game units)
	float m_locoX = 0.0f, m_locoY = 0.0f, m_locoZ = 0.0f;
	float m_locoYaw = 0.0f; // stick yaw delta (old − new, radians)
	float m_slotFraction = 0.5f; // warp position within the game frame (0..1)
	uint32_t m_eyeWidth = 0, m_eyeHeight = 0;
	// Depth cache dims — differ from eye dims when an external render-scale mod
	// upscales the submitted frame while depth stays at render resolution.
	uint32_t m_depthWidth = 0, m_depthHeight = 0;
	bool m_depthLayerValid = true;
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

	// What Compositor::SwapchainGeneration() read when the current chains were built.
	uint32_t m_swapchainGeneration = 0;

	// Cached pose/FOV/depth from real frame
	XrPosef m_cachedPose[2] = {};
	XrFovf m_cachedFov[2] = {};
	bool m_cachedSourceFlipV[2] = {};
	float m_cachedNear = 0.1f;
	float m_cachedFar = 10000.0f;
	bool m_hasCachedFrame = false;
	// Eyes successfully written for the in-progress generation. A published
	// frame is never exposed while this mask is non-zero.
	uint8_t m_cacheBuildEyeMask = 0;

	// Constant buffer layout (must match HLSL, 16-byte aligned = 128 bytes)
	struct WarpConstants {
		float poseDeltaMatrix[16]; // 4x4 row-major
		float resolution[2];
		float nearZ, farZ;
		float fovTanLeft, fovTanRight, fovTanUp, fovTanDown;
		float depthScale;          // multiplier on linearized depth
		float edgeFadeWidth;       // depth-edge fade threshold (depth ratio units)
		float nearFadeDepth;       // parallax fades to 0 below this depth (game units); 0 = disabled
		float debugTint;           // >0.5 = red-tint warp frames (aswDebugMode=10)
		float depthResolution[2];  // depth grid size (may differ from resolution under external render scale)
		float sourceFlip[2];       // x is reserved; y=1 preserves reversed-V submit bounds
	};
	static_assert(sizeof(WarpConstants) == 128);
};
