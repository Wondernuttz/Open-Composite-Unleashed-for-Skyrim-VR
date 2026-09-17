#pragma once

#include "XrDriverPrivate.h"
#include "DapaTiming.h"
#include "DapaMotion.h"
#include "DapaCapture.h"
#include "DapaDepthTransfer.h"
#include "DapaGpuTiming.h"
#include "../OpenOVR/Misc/FoveationBlackout.h"
#include <d3d11.h>
#include <vector>

// Forward declaration — ASWProvider is accessed from XrBackend for frame injection
class ASWProvider;
extern ASWProvider* g_aswProvider;
// Shared SKSE bridge menu state, independent of render-target readiness.
bool OCBridge_DapaMenuPaused();
bool OCBridge_DapaMaskCacheValid();

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
	/// Call on each eye during the real frame's Invoke. Returns true when this
	/// eye was cached successfully; eye 1 only succeeds after eye 0 from the
	/// same cache generation has also succeeded.
	/// A non-null bodyDepthMask must remain leased against producer mutation
	/// and release for the entire call. Failure to cache it rejects this pair.
	bool CacheFrame(int eye, ID3D11DeviceContext* ctx,
	    ID3D11Texture2D* colorTex, const D3D11_BOX* colorRegion,
	    bool sourceFlipV,
	    ID3D11Texture2D* mvTex, const D3D11_BOX* mvRegion,
	    ID3D11Texture2D* depthTex, const D3D11_BOX* depthRegion,
	    const XrPosef& eyePose, const XrFovf& eyeFov,
	    float nearZ, float farZ, ID3D11Texture2D* bodyDepthMask = nullptr,
	    const ocu_foveation::BlackoutFrame& blackout = {});

	/// Warp cached frame to new pose, write result to output texture.
	/// Call for each eye during the injected frame.
	bool WarpFrame(int eye, ID3D11DeviceContext* ctx,
	    const XrPosef& newPose);
	/// Get the output XR swapchain for the warped frame (for layer assembly).
	XrSwapchain GetOutputSwapchain() const { return m_outputSwapchain; }

	/// Get the depth XR swapchain for the warped frame (for XR_KHR_composition_layer_depth).
	XrSwapchain GetDepthSwapchain() const { return m_depthSwapchain; }

	/// False while the depth cache runs at a different resolution than the eye
	/// (external render scale). The parallax warp still works (shader samples
	/// depth by UV), or when reversed submit bounds require shader-side depth
	/// sampling that the unchanged XR depth swapchain cannot represent.
	bool DepthLayerValid() const
	{
		// The existing optional depth swapchain contains UNWARPED real depth.
		// Do not attach it to translated synthetic colour as if it still matched.
		return m_depthLayerValid && !m_cachedSourceFlipV[0] && !m_cachedSourceFlipV[1]
		    && !m_warpMoved[0] && !m_warpMoved[1];
	}

	/// Get per-eye sub-image rect for the warped output (stereo-combined).
	XrRect2Di GetOutputRect(int eye) const;

	/// Acquire output swapchain, copy warped textures, release.
	/// Call once after WarpFrame for both eyes.
	bool SubmitWarpedOutput(ID3D11DeviceContext* ctx, double displayPeriodMs);
	bool HasSubmittedDepth() const { return DepthLayerValid() && m_depthSubmittedThisFrame; }

	bool HasCachedFrame() const { return m_hasCachedFrame; }
	void InvalidateCachedFrame()
	{
		m_capture.HistoryInvalidated();
		m_hasCachedFrame = false;
		m_cacheBuildEyeMask = 0;
		m_cachedBlackout[0] = m_cachedBlackout[1] = {};
		m_motion.Reset();
		m_captureMovement.Reset();
		m_turn.Reset();
		m_motionGeometryValid[0] = m_motionGeometryValid[1] = false;
	}

	/// Cached pose/FOV for building projection views during injection
	XrPosef GetCachedPose(int eye) const { return m_cachedPose[eye]; }
	XrFovf GetCachedFov(int eye) const { return m_cachedFov[eye]; }

	/// Cached near/far for depth layer submission
	float GetCachedNear() const { return m_cachedNear; }
	float GetCachedFar() const { return m_cachedFar; }

	/// Get the D3D11 device used during initialization (for obtaining context in XrBackend)
	ID3D11Device* GetDevice() const { return m_device; }

	// Samples must accompany a successfully cached REAL eye. No synthetic history.
	void SetMotionGeometry(int eye, const float* view, const float* vp);
	void SampleLocomotion(XrTime time, DapaMotion::Vec3 position);
	void CaptureTick();
	bool CaptureBusy() const { return m_capture.Busy(); }
	bool CaptureRecording() const { return m_capture.Recording(); }
	void CaptureSubmission(XrTime time, XrResult result) { m_capture.Submission(time, int(result)); }
	void SampleLocomotionYaw(XrTime time, float yaw, bool turning) { m_turn.Sample(time,yaw,turning); }
	void SetWarpDisplayTime(XrTime time) { m_warpDisplayTime = time; }
	void SetPaused(bool paused)
	{
		if (paused && !m_paused) {
			// Menus can arrive after a stereo pair was cached. Discard that pair
			// and its motion history now; closing the menu must capture anew.
			m_injectionWanted = false;
			InvalidateCachedFrame();
		}
		m_paused = paused;
	}
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
	DapaGpuTiming m_gpuTiming;
	DapaGpuTiming::Scope MeasureGpu(ID3D11DeviceContext* ctx, DapaGpuTiming::Stage stage);
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
	DapaMotion::Predictor m_motion;
	DapaCaptureTelemetry::Measurement m_captureMovement;
	DapaCapture m_capture;
	XrTime m_warpDisplayTime = 0;
	float m_motionView[2][16] = {};
	float m_motionProjection[2][16] = {}, m_motionInvProjection[2][16] = {};
	bool m_motionGeometryValid[2] = {};
	bool m_warpMoved[2] = {};
	DapaMotion::YawPredictor m_turn;
	uint32_t m_eyeWidth = 0, m_eyeHeight = 0;
	// Depth cache dims — differ from eye dims when an external render-scale mod
	// upscales the submitted frame while depth stays at render resolution.
	uint32_t m_depthWidth = 0, m_depthHeight = 0;
	bool m_depthLayerValid = true;
	ID3D11Device* m_device = nullptr; // kept for obtaining immediate context in XrBackend

	// Compute shader
	ID3D11ComputeShader* m_warpCS = nullptr;
	ID3D11Buffer* m_constantBuffer = nullptr;
	ID3D11Buffer* m_blackoutConstantBuffer = nullptr;
	ID3D11SamplerState* m_linearSampler = nullptr;

	// Per-eye cached textures (staging copies of game frame data)
	ID3D11Texture2D* m_cachedColor[2] = {};
	ID3D11Texture2D* m_cachedMV[2] = {};
	ID3D11Texture2D* m_cachedDepth[2] = {};
	Microsoft::WRL::ComPtr<ID3D11Texture2D> m_bodyDepth[2];
	Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_bodySrv[2];
	bool m_bodyValid[2] = {};
	ID3D11ShaderResourceView* m_srvColor[2] = {};
	ID3D11ShaderResourceView* m_srvMV[2] = {};
	ID3D11ShaderResourceView* m_srvDepth[2] = {};

	// Per-eye warped output (compute shader writes here)
	ID3D11Texture2D* m_warpedOutput[2] = {};
	ID3D11UnorderedAccessView* m_uavOutput[2] = {};

	// Stereo-combined XR swapchain for warped output (both eyes side-by-side)
	XrSwapchain m_outputSwapchain = {};
	DapaTiming::ImageLease m_outputLease;
	DapaTiming::ImageLease m_depthLease;
	bool m_depthSubmittedThisFrame = false;
	std::vector<ID3D11Texture2D*> m_outputSwapchainImages; // all swapchain images

	// Runtime-negotiated depth; full-atlas transfer also supports D32_FLOAT.
	DapaDepthTransfer m_depthTransfer;
	XrSwapchain m_depthSwapchain = {};
	std::vector<ID3D11Texture2D*> m_depthSwapchainImages;

	// Cached pose/FOV/depth from real frame
	XrPosef m_cachedPose[2] = {};
	XrFovf m_cachedFov[2] = {};
	bool m_cachedSourceFlipV[2] = {};
	ocu_foveation::BlackoutFrame m_cachedBlackout[2];
	float m_cachedNear = 0.1f;
	float m_cachedFar = 10000.0f;
	bool m_hasCachedFrame = false;
	// Eyes successfully written for the in-progress generation. A published
	// frame is never exposed while this mask is non-zero.
	uint8_t m_cacheBuildEyeMask = 0;

	// Constant buffer layout (must match HLSL, 16-byte aligned = 272 bytes)
	struct WarpConstants {
		float poseDeltaMatrix[16]; // 4x4 row-major
		float resolution[2];
		float nearZ, farZ;
		float fovTanLeft, fovTanRight, fovTanUp, fovTanDown;
		float depthScale;          // multiplier on linearized depth
		float edgeFadeWidth;       // legacy layout/capture value; no UV confidence fade in solver v2
		float nearFadeDepth;       // parallax fades to 0 below this depth (game units); 0 = disabled
		float debugTint;           // >0.5 = red-tint warp frames (aswDebugMode=10)
		float depthResolution[2];  // depth grid size (may differ from resolution under external render scale)
		float sourceFlip[2];       // x is reserved; y=1 preserves reversed-V submit bounds
		float projection[16];
		float inverseProjection[16];
		float predictionValid;
		float padding[3];
	};
	static_assert(sizeof(WarpConstants) == 272);
	// Separate b1 keeps the existing capture/replay b0 layout unchanged.
	struct BlackoutConstants {
		float center[2], inner, middle;
		float scale, cutoff;
		uint32_t flags, enabled;
	};
	static_assert(sizeof(BlackoutConstants) == 32);
	BlackoutConstants m_uploadedBlackout{};
};
