#pragma once

#include "XrDriverPrivate.h"
#include <d3d11.h>
#include <d3dcompiler.h>
#include <vector>
#include <chrono>
#include <future>

// ── FP Draw Replay: captured D3D11 state for re-rendering hands on warp frames ──
// These structs are allocated in SKSE (game thread) and read by OC (submit thread).
// COM pointers are valid cross-thread since both DLLs are in the same process.

struct FPDrawCapture {
	bool valid = false;
	// IA state
	ID3D11Buffer* ib = nullptr;
	DXGI_FORMAT ibFmt = DXGI_FORMAT_UNKNOWN;
	UINT ibOff = 0;
	ID3D11Buffer* vb = nullptr;
	UINT vbStride = 0, vbOff = 0;
	ID3D11InputLayout* il = nullptr;
	D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
	UINT indexCount = 0, startIndex = 0;
	INT baseVertex = 0;
	UINT instanceCount = 1;
	// Shaders
	ID3D11VertexShader* vs = nullptr;
	ID3D11PixelShader* ps = nullptr;
	// PS resources
	ID3D11ShaderResourceView* psSRVs[8] = {};
	ID3D11SamplerState* psSamplers[4] = {};
	// OM
	ID3D11BlendState* blendState = nullptr;
	float blendFactor[4] = {};
	UINT sampleMask = 0xFFFFFFFF;
	ID3D11DepthStencilState* dss = nullptr;
	UINT stencilRef = 0;
	// RS
	ID3D11RasterizerState* rs = nullptr;
	// VS CB byte snapshots (sizes from VP-DIAG during FP draws)
	uint8_t cb0[80] = {};    bool hasCB0 = false;   // per-draw constants
	uint8_t cb1[48] = {};    bool hasCB1 = false;
	uint8_t cb2[400] = {};   bool hasCB2 = false;   // per-geometry (world transform)
	uint8_t cb9[3840] = {};  bool hasCB9 = false;   // bone matrices
	uint8_t cb10[3840] = {}; bool hasCB10 = false;  // prev bones
	uint8_t cb13[48] = {};   bool hasCB13 = false;  // VR stereo params
	// PS CB byte snapshots
	uint8_t psCB0[256] = {}; UINT psCB0Size = 0; bool hasPSCB0 = false;
	uint8_t psCB1[256] = {}; UINT psCB1Size = 0; bool hasPSCB1 = false;
	uint8_t psCB2[256] = {}; UINT psCB2Size = 0; bool hasPSCB2 = false;
};

struct FPReplayData {
	static constexpr int MAX_DRAWS = 16;
	static constexpr int NUM_BUFS = 2;

	struct CaptureSet {
		FPDrawCapture draws[MAX_DRAWS] = {};
		int drawCount = 0;
		// Shared CB[12] snapshot (1408 bytes — same for all FP draws in a batch)
		uint8_t cb12[1408] = {};
		bool hasCB12 = false;
		// Viewport from first FP draw
		D3D11_VIEWPORT viewport = {};
	};

	CaptureSet sets[NUM_BUFS] = {};
	std::atomic<int> readyIdx{ -1 };  // which set has valid data (-1 = none)
	int writeIdx = 0;                  // SKSE-only: which set to write next
};

// Hand composite CB layout (must match HLSL HandCompositeCB, 16-byte aligned)
struct HandCompositeConstants {
	float controllerDeltaL[2];
	float controllerDeltaR[2];
	float controllerUV_L[2];
	float controllerUV_R[2];
	float controllerRadius;
	float resolution[2];
	float _pad;
};

// Forward declaration — ASWProvider is accessed from XrBackend for frame injection
class ASWProvider;
extern ASWProvider* g_aswProvider;

// Set by dx11compositor when the game is on a loading screen or main menu.
// The ASW submit thread checks this to skip D3D11 warp operations during
// scene transitions — avoids contention with background texture loading
// threads (e.g. Community Shaders' OnLoadTextureSet).
#include <atomic>
extern std::atomic<bool> g_aswSkipWarp;

// ── ASW frame buffering: staging textures for decoupled game/submit pipeline ──
// When active, the game thread copies to staging textures instead of the OpenXR
// swapchain. SubmitFrames copies staging → swapchain at submit time.
static constexpr uint32_t kAswStagingSlotCount = 3;
static constexpr uint32_t kAswCacheSlotCount = 3;
extern bool g_aswStagingActive;                    // true when buffered pipeline is active
extern ID3D11Texture2D* g_aswStagingTex[2][kAswStagingSlotCount]; // per-eye ring staging textures
extern ID3D11Query* g_aswStagingDoneQuery[2][kAswStagingSlotCount]; // GPU completion markers per slot
extern std::atomic<uint64_t> g_aswStagingSlotSeq[2][kAswStagingSlotCount]; // sequence stamped into each slot
extern std::atomic<int64_t> g_aswStagingPublishNs[2][kAswStagingSlotCount]; // CPU publish timestamp per slot (steady-clock ns)
extern std::atomic<uint32_t> g_aswStagingWriteCursor[2]; // game publish cursor per eye
extern std::atomic<int> g_aswStagingPublishedSlot[2]; // newest published slot per eye (-1 if none)
extern std::atomic<int> g_aswStagingLastReadySlot[2]; // newest ready slot per eye (-1 if none)
extern std::atomic<uint64_t> g_aswStagingPublishSeq[2]; // publish sequence per eye
extern XrSwapchain g_aswStagingSwapchain[2];        // per-eye swapchain handles
extern std::vector<XrSwapchainImageD3D11KHR> g_aswStagingSwapImages[2]; // swapchain image arrays

class ASWProvider {
public:
	ASWProvider() = default;
	~ASWProvider();

	ASWProvider(const ASWProvider&) = delete;
	ASWProvider& operator=(const ASWProvider&) = delete;

	/// Parameters passed to the warp upscale callback.
	struct WarpUpscaleParams {
		int eye;
		ID3D11DeviceContext* ctx;
		ID3D11Texture2D* warpedColor;   ///< Render-res warped color
		ID3D11Texture2D* cachedDepth;   ///< Render-res cached depth (R32F, per-eye)
		uint32_t renderW, renderH;      ///< Render resolution
		uint32_t outputW, outputH;      ///< Output (display) resolution
		float nearZ, farZ;
		XrPosef cachedPose;             ///< Pose the cached frame was rendered at
		XrPosef warpPose;               ///< Pose the warp frame targets
		XrFovf  cachedFov;              ///< FOV the cached frame was rendered at
		float   poseDeltaMatrix[16];    ///< 4x4 view-space transform: warp view → cached view (Z-flip applied)
	};

	/// Callback type for upscaling warp output through DLSS/FSR3.
	/// Called per-eye after WarpFrame, before copying to output swapchain.
	/// Returns true on success, sets *outResult to the display-res upscaled texture.
	using WarpUpscaleCallback = bool(*)(const WarpUpscaleParams& params, ID3D11Texture2D** outResult);

	/// Initialize: compile compute shader, create staging textures + output swapchains.
	/// @param device       D3D11 device (from compositor)
	/// @param renderWidth  Per-eye render resolution (for cache + warp textures)
	/// @param renderHeight Per-eye render resolution
	/// @param outputWidth  Per-eye output resolution (for output swapchain, may differ when upscaler active)
	/// @param outputHeight Per-eye output resolution
	bool Initialize(ID3D11Device* device, uint32_t renderWidth, uint32_t renderHeight,
	    uint32_t outputWidth, uint32_t outputHeight);
	void Shutdown();
	bool IsReady() const { return m_ready; }

	/// Poll for background shader compilation completion (non-blocking).
	/// Creates shader objects on the game thread when compilation finishes.
	void TryFinishShaderCompilation();

	/// Cache current frame's data for warping next cycle.
	/// Call on each eye during the real frame's Invoke.
	void CacheFrame(int eye, ID3D11DeviceContext* ctx,
	    ID3D11Texture2D* colorTex, const D3D11_BOX* colorRegion,
	    ID3D11Texture2D* mvTex, const D3D11_BOX* mvRegion,
	    ID3D11Texture2D* depthTex, const D3D11_BOX* depthRegion,
	    const XrPosef& eyePose, const XrFovf& eyeFov,
	    float nearZ, float farZ);

	/// Cache stencil buffer for this eye (R8_UINT, extracted from depth-stencil).
	/// Call after CacheFrame for each eye. Writes to the current build slot.
	void CacheStencil(int eye, ID3D11DeviceContext* ctx,
	    ID3D11Texture2D* stencilTex, const D3D11_BOX* stencilRegion);

	/// Warp cached frame to new pose, write result to output texture.
	/// Call for each eye during the injected frame.
	/// @param slotOverride  If >= 0, force reading from this cache slot instead of m_publishedSlot.
	bool WarpFrame(int eye, const XrPosef& newPose, int slotOverride = -1);

	/// Get the output XR swapchain for the warped frame (for layer assembly).
	XrSwapchain GetOutputSwapchain() const { return m_outputSwapchain; }

	/// Get the depth XR swapchain for the warped frame (for XR_KHR_composition_layer_depth).
	/// Currently returns XR_NULL_HANDLE: submitting unwarped depth with warped color causes
	/// double reprojection (runtime's depth-based reproject on top of our ASW warp = "flying borders").
	/// TODO: Once forward scatter outputs warped depth, enable this to submit matching depth.
	XrSwapchain GetDepthSwapchain() const { return XR_NULL_HANDLE; }

	/// Get per-eye sub-image rect for the warped output (stereo-combined).
	XrRect2Di GetOutputRect(int eye) const;

	/// Acquire output swapchain, copy warped textures, release.
	/// Call once after WarpFrame for both eyes.
	bool SubmitWarpedOutput(ID3D11DeviceContext* ctx);

	/// Acquire output swapchain, clear to black, release.
	/// Used for debug mode 50 to submit a valid black layer (prevents runtime ATW).
	bool SubmitBlackOutput(ID3D11DeviceContext* ctx);

	bool HasCachedFrame() const { return m_publishedSlot.load(std::memory_order_acquire) >= 0; }

	/// True when at least 2 frames have been cached (N-1 warping can start).
	bool HasPreviousCachedFrame() const { return m_previousPublishedSlot.load(std::memory_order_acquire) >= 0; }

	/// Get the most recently published cache slot index (-1 if none).
	int GetPublishedSlot() const { return m_publishedSlot.load(std::memory_order_acquire); }

	/// Store the predicted display time associated with a cache slot.
	void SetSlotDisplayTime(int slot, XrTime t) {
		if (slot >= 0 && slot < (int)kAswCacheSlotCount)
			m_slotDisplayTime[slot] = t;
	}

	/// Get the predicted display time for a cache slot.
	XrTime GetSlotDisplayTime(int slot) const {
		return (slot >= 0 && slot < (int)kAswCacheSlotCount) ? m_slotDisplayTime[slot] : 0;
	}

	/// Age of cached frame in milliseconds (since last CacheFrame call)
	float GetCacheAgeMs() const {
		int slot = GetActiveCacheSlot();
		if (slot < 0) return 1e9f;
		auto now = std::chrono::steady_clock::now();
		return std::chrono::duration<float, std::milli>(now - m_slotTimestamp[slot]).count();
	}

	/// Pause/unpause ASW warping (e.g. during loading screens).
	void SetPaused(bool paused) { m_paused = paused; }
	bool IsPaused() const { return m_paused; }

	/// Set stick yaw delta (old − new, radians) for stick turn correction.
	void SetLocomotionYaw(float yawDelta) {
		m_prevLocoYaw = m_locoYaw;
		m_locoYaw = yawDelta;
	}

	/// Set locomotion translation delta (view-space, game units) for thumbstick movement correction.
	/// Computed from worldToCam delta minus OpenXR head translation.
	void SetLocomotionTranslation(float x, float y, float z) {
		m_prevLocoMag = sqrtf(m_locoTransX * m_locoTransX +
		    m_locoTransY * m_locoTransY + m_locoTransZ * m_locoTransZ);
		m_locoTransX = x; m_locoTransY = y; m_locoTransZ = z;
	}

	/// Store NiCamera Z at cache time for vertical warp correction.
	/// Called during CacheFrame to record the camera height for this slot.
	void SetSlotCameraPosZ(float z) {
		m_slotPrevCameraPosZ[m_buildSlot] = m_slotCameraPosZ[m_buildSlot];
		m_slotCameraPosZ[m_buildSlot] = z;
	}

	/// Store the live cameraPosPtr so WarpFrame can read it at warp time.
	void SetCameraPosPtr(uint64_t ptr) { m_cameraPosPtr = ptr; }

	/// Store the first-person root NiAVObject pointer for hand bounding sphere detection.
	void SetFirstPersonRootPtr(uint64_t ptr) { m_firstPersonRootPtr = ptr; }

	/// Store the FP depth-stencil texture (separate from main DS, rendered with zNear=1).
	/// The warp shader reads this to identify player character pixels.
	void SetFPDepthTex(ID3D11Texture2D* tex) { m_fpDepthTex = tex; }
	void SetFPGeomPointers(const uint64_t* ptrs, uint32_t count) {
		if (count > 16) count = 16;
		m_fpGeomCount = count;
		if (count > 0) memcpy(m_fpGeomPointers, ptrs, count * sizeof(uint64_t));
	}
	void CachePreFPDepth(int eye, ID3D11DeviceContext* ctx,
	    ID3D11Texture2D* preFPR32F, const D3D11_BOX* region);

	/// Store pointer to FP replay data (heap-allocated in SKSE, same process).
	/// Called once from dx11compositor when bridge provides the pointer.
	void SetFPReplayPtr(FPReplayData* ptr) { m_fpReplayPtr = ptr; }

	/// Store VR controller positions in tracking space (from GetDeviceToAbsoluteTrackingPose).
	/// Called each frame from XrBackend. [0]=left, [1]=right.
	void SetControllerPos(int hand, float x, float y, float z, bool valid) {
		if (hand >= 0 && hand < 2) {
			m_controllerPos[hand][0] = x;
			m_controllerPos[hand][1] = y;
			m_controllerPos[hand][2] = z;
			m_controllerValid[hand] = valid;
		}
	}
	bool GetControllerValid(int hand) const { return hand >= 0 && hand < 2 && m_controllerValid[hand]; }
	const float* GetControllerPos(int hand) const { return (hand >= 0 && hand < 2) ? m_controllerPos[hand] : nullptr; }

	/// Store projected controller UV positions for hand detection (current build slot, per-eye).
	void SetSlotControllerUV(int eye, float luv[2], float ruv[2], float radius) {
		if (eye < 0 || eye > 1) return;
		m_slotControllerUV[m_buildSlot][eye][0][0] = luv[0];
		m_slotControllerUV[m_buildSlot][eye][0][1] = luv[1];
		m_slotControllerUV[m_buildSlot][eye][1][0] = ruv[0];
		m_slotControllerUV[m_buildSlot][eye][1][1] = ruv[1];
		m_slotControllerRadius[m_buildSlot] = radius;
		m_slotHasControllerUV[m_buildSlot][eye] = true;
	}

	/// Store the RSS view matrix pointer for coordinate transforms at warp time.
	void SetRSSViewMatPtr(const float* ptr) { m_rssViewMatPtr = ptr; }

	/// Set a temporary scale on mvConfidence (1.0 = normal, 0.0 = ignore game MVs).
	/// Used at warp time to decay MV influence when sticks are released, preventing
	/// stale game MVs from causing overshoot on stop.
	void SetMVConfidenceScale(float s) { m_mvConfidenceScale = s; }
	float GetMVConfidenceScale() const { return m_mvConfidenceScale; }

	/// Set rotation MV scale (1.0 = rotation in residual, 0.0 = rotation suppressed).
	/// Independent from mvConfidenceScale so rotation and loco can stop separately.
	void SetRotMVScale(float s) { m_rotMVScale = s; }
	float GetRotMVScale() const { return m_rotMVScale; }

	/// Get current loco values (for warp-time decay tracking)
	float GetLocoTransX() const { return m_locoTransX; }
	float GetLocoTransY() const { return m_locoTransY; }
	float GetLocoTransZ() const { return m_locoTransZ; }
	float GetLocoYaw() const { return m_locoYaw; }

	/// Cache the clipToClip matrix for this eye (prevVP * inv(curVP), from camera MV computation).
	void SetClipToClip(int eye, const float* mat16) {
		if (eye >= 0 && eye < 2 && mat16) {
			memcpy(m_cachedClipToClip[eye], mat16, 16 * sizeof(float));
			m_hasClipToClip = true;
		}
	}

	/// Cache the loco-free clipToClip for this eye (prevVP * inv(curVP_original), no locomotion injection).
	/// Used by the warp shader: exactly matches the head-rotation component of camera MVs.
	/// Writes to the CURRENT BUILD SLOT so it's correctly associated with the cached frame data.
	void SetClipToClipNoLoco(int eye, const float* mat16) {
		if (eye >= 0 && eye < 2 && mat16) {
			memcpy(m_slotClipToClipNoLoco[m_buildSlot][eye], mat16, 16 * sizeof(float));
			m_slotHasClipToClipNoLoco[m_buildSlot] = true;
		}
	}

	/// Cache the FULL clipToClip (WITH locomotion injection) for this eye.
	/// prevVP * inv(curVP_adjusted) — captures head rot + head trans + locomotion parallax.
	/// Writes to the CURRENT BUILD SLOT (same timing as SetClipToClipNoLoco — BEFORE CacheFrame).
	void SetClipToClipWithLoco(int eye, const float* mat16) {
		if (eye >= 0 && eye < 2 && mat16) {
			memcpy(m_slotClipToClipWithLoco[m_buildSlot][eye], mat16, 16 * sizeof(float));
			m_slotHasClipToClipWithLoco[m_buildSlot] = true;
		}
	}

	/// Set the MV timing ratio for this injected frame.
	/// ratio = (T_inject - T_realFrame) / (T_realFrame - T_prevRealFrame)
	/// Gives the exact interpolation weight (0.5 at half-refresh).
	void SetMVTimingRatio(float r) { m_mvTimingRatio = r; }

	/// Set menu open state — when true, ASW skips MV corrections to prevent UI duplication.
	void SetMenuOpen(bool open) { m_isMenuOpen = open; }

	/// Cached pose/FOV for building projection views during injection
	XrPosef GetCachedPose(int eye) const {
		int slot = GetActiveCacheSlot();
		return (slot >= 0 && eye >= 0 && eye < 2) ? m_slotPose[slot][eye] : XrPosef{};
	}

	/// Predicted pose used for the last precomputed warp (set in WarpFrame).
	/// The warp shader applies poseDelta rotation from cached→predicted, so the
	/// output content is at this orientation. Declare this in the composition layer
	/// for a smaller ATW correction (~7ms vs ~29ms from cached pose).
	XrPosef GetPrecompPose(int eye) const {
		return (eye >= 0 && eye < 2) ? m_precompPose[eye] : XrPosef{};
	}

	XrFovf GetCachedFov(int eye) const {
		int slot = GetActiveCacheSlot();
		return (slot >= 0 && eye >= 0 && eye < 2) ? m_slotFov[slot][eye] : XrFovf{};
	}

	/// Cached near/far for depth layer submission
	float GetCachedNear() const {
		int slot = GetActiveCacheSlot();
		return (slot >= 0) ? m_slotNear[slot] : 0.1f;
	}
	float GetCachedFar() const {
		int slot = GetActiveCacheSlot();
		return (slot >= 0) ? m_slotFar[slot] : 10000.0f;
	}

	/// Frame ID: monotonic counter incremented each CacheFrame (eye 0 only)
	uint64_t GetCachedFrameId() const {
		int slot = GetActiveCacheSlot();
		return (slot >= 0) ? m_slotFrameId[slot] : 0;
	}

	/// Get the D3D11 device used during initialization (for obtaining context in XrBackend)
	ID3D11Device* GetDevice() const { return m_device; }

	/// Set a callback for upscaling warp output through DLSS (set by dx11compositor after init).
	void SetWarpUpscaleCallback(WarpUpscaleCallback cb) { m_warpUpscaleCallback = cb; }

	/// Get per-eye output dimensions (display-res when upscaler active)
	uint32_t GetEyeWidth() const { return m_outputWidth; }
	uint32_t GetEyeHeight() const { return m_outputHeight; }

	/// Get per-eye render dimensions (for cache + warp, may be smaller than output)
	uint32_t GetRenderWidth() const { return m_renderWidth; }
	uint32_t GetRenderHeight() const { return m_renderHeight; }

	/// Get cached depth texture for a given slot/eye (for DLSS warp dispatch)
	ID3D11Texture2D* GetCachedDepth(int slot, int eye) const {
		if (slot >= 0 && slot < (int)kAswCacheSlotCount && eye >= 0 && eye < 2)
			return m_cachedDepth[slot][eye];
		return nullptr;
	}

private:
	bool LaunchAsyncShaderCompilation();
	bool CreateStagingTextures(ID3D11Device* device);
	bool CreateOutputSwapchain(uint32_t width, uint32_t height);
	bool CreateDepthSwapchain(uint32_t width, uint32_t height);
	bool CreateHandRenderTargets(ID3D11Device* device, uint32_t w, uint32_t h);
	bool ReplayFPHands(int eye, ID3D11DeviceContext* ctx,
	    const float* forwardPoseDelta, const HandCompositeConstants& hcc);

	int GetActiveCacheSlot() const {
		int slot = m_warpReadSlot.load(std::memory_order_acquire);
		if (slot >= 0)
			return slot;
		return m_publishedSlot.load(std::memory_order_acquire);
	}

	// Quaternion math helpers
	static void QuatInverse(const XrQuaternionf& q, XrQuaternionf& out);
	static void QuatMultiply(const XrQuaternionf& a, const XrQuaternionf& b, XrQuaternionf& out);
	static void QuatRotateVec(const XrQuaternionf& q, const XrVector3f& v, XrVector3f& out);
	static void BuildPoseDeltaMatrix(const XrPosef& oldPose, const XrPosef& newPose,
	    float* outMatrix4x4);

	bool m_paused = false; // true during loading screens — suppresses warp
	float m_locoYaw   = 0.0f;  // stick yaw delta (old − new, radians)
	float m_prevLocoYaw = 0.0f; // previous frame's yaw for stop detection
	float m_locoTransX = 0.0f, m_locoTransY = 0.0f, m_locoTransZ = 0.0f; // loco translation (view-space, game units)
	float m_prevLocoMag = 0.0f; // previous frame's loco magnitude for stop detection
	float m_mvTimingRatio = 0.5f; // dynamic MV extrapolation weight (computed from frame timing)
	float m_cachedClipToClip[2][16] = {};          // per-eye clipToClip from camera MV computation
	bool m_hasClipToClip = false;
	float m_mvConfidenceScale = 1.0f;              // warp-time MV confidence scale (decays on loco stop)
	float m_rotMVScale = 1.0f;                     // rotation MV scale (decays on rotation stop)
	uint64_t m_cameraPosPtr = 0;        // live NiCamera::world.translate pointer (for warp-time Z read)
	const float* m_rssViewMatPtr = nullptr; // RSS view matrix pointer (for world→view transform at warp time)
	uint64_t m_firstPersonRootPtr = 0; // NiAVObject* for player 1st-person skeleton root
	ID3D11Texture2D* m_fpDepthTex = nullptr; // FP depth-stencil (game's live texture, NOT owned)
	ID3D11ShaderResourceView* m_fpDepthSRV = nullptr; // R24_UNORM_X8_TYPELESS SRV on FP DS
	ID3D11Texture2D* m_fpDepthSRVTex = nullptr; // Cached: which tex the SRV was created for

	// Pre-FP depth: depth snapshot BEFORE first-person geometry renders.
	// Compared against post-FP cached depth to identify FP pixels (where depth got closer).
	ID3D11Texture2D* m_cachedPreFPDepth[kAswCacheSlotCount][2] = {};
	ID3D11ShaderResourceView* m_srvPreFPDepth[kAswCacheSlotCount][2] = {};
	ID3D11Texture2D* m_preFPDepthSrcTex = nullptr; // bridge preFPDepthTexture (NOT owned)

	// FP geometry pointers from bridge — read worldBound LIVE at WarpFrame time
	uint64_t m_fpGeomPointers[16] = {};
	uint32_t m_fpGeomCount = 0;

	float m_controllerPos[2][3] = {}; // tracking-space positions [hand][xyz]
	bool m_controllerValid[2] = {};
	float m_slotControllerUV[kAswCacheSlotCount][2][2][2] = {}; // [slot][eye][hand][u,v]
	float m_slotControllerRadius[kAswCacheSlotCount] = {};
	bool m_slotHasControllerUV[kAswCacheSlotCount][2] = {}; // [slot][eye]

	// Per-slot clipToClipNoLoco: buffered with the cache slot so WarpFrame reads the
	// matrix from the SAME frame as the cached color/MV/depth textures.
	// Without per-slot buffering, the game thread can overwrite this with the NEXT frame's
	// data before the worker reads it (race condition in the P-after-S pipeline order).
	float m_slotClipToClipNoLoco[kAswCacheSlotCount][2][16] = {};
	bool m_slotHasClipToClipNoLoco[kAswCacheSlotCount] = {};
	float m_slotClipToClipWithLoco[kAswCacheSlotCount][2][16] = {};
	bool m_slotHasClipToClipWithLoco[kAswCacheSlotCount] = {};
	XrPosef m_precompPose[2] = {};  // predicted poses from WarpFrame (for composition layer)
	float m_lastPoseDelta[2][16] = {}; // poseDeltaMatrix from last WarpFrame (per-eye, for DLSS callback)

	bool m_ready = false;
	uint32_t m_renderWidth = 0, m_renderHeight = 0;   // cache + warp resolution (render-res)
	uint32_t m_outputWidth = 0, m_outputHeight = 0;   // output swapchain resolution (display-res when upscaler active)
	ID3D11Device* m_device = nullptr; // kept for obtaining immediate context in XrBackend
	WarpUpscaleCallback m_warpUpscaleCallback = nullptr;

	// Async shader compilation state
	struct PendingShaderBlob {
		const char* entry = nullptr;
		ID3DBlob* blob = nullptr;
		bool ok = false;
	};
	std::future<void> m_compileFuture;
	std::vector<PendingShaderBlob> m_pendingBlobs;
	bool m_compileStarted = false;

	// Compute shaders
	ID3D11ComputeShader* m_warpCS = nullptr;       // CSMain (backward warp)
	ID3D11ComputeShader* m_clearCS = nullptr;       // CSClear (clear atomicDepth + output)
	ID3D11ComputeShader* m_forwardDepthCS = nullptr; // CSForwardDepth (two-pass: depth only)
	ID3D11ComputeShader* m_forwardColorCS = nullptr; // CSForwardColor (two-pass: color with finalized depth)
	ID3D11ComputeShader* m_forwardDepthNpcOnlyCS = nullptr; // CSForwardDepthNpcOnly
	ID3D11ComputeShader* m_forwardColorNpcOnlyCS = nullptr; // CSForwardColorNpcOnly
	ID3D11ComputeShader* m_compositeNpcForwardCS = nullptr; // CSCompositeNpcForward
	ID3D11ComputeShader* m_dilateCS = nullptr;      // CSDilate (gap fill)
	ID3D11ComputeShader* m_npcDepthScatterCS = nullptr; // CSNpcDepthScatter (moving-NPC boundary extension)
	ID3D11ComputeShader* m_depthPreScatterCS = nullptr; // CSDepthPreScatter (warp depth forward for leading edges)
	ID3D11ComputeShader* m_blurVoidsCS = nullptr;      // CSBlurVoids (post-fill blur within void zones)
	ID3D11ComputeShader* m_legacyWarpCS = nullptr;     // Legacy backward-warp-only shader
	ID3D11Buffer* m_constantBuffer = nullptr;
	ID3D11SamplerState* m_linearSampler = nullptr;

	// Forward scatter: per-eye atomic depth buffer for depth test
	ID3D11Texture2D* m_atomicDepth[2] = {};
	ID3D11UnorderedAccessView* m_uavAtomicDepth[2] = {};
	ID3D11ShaderResourceView* m_srvAtomicDepth[2] = {};

	// Forward map: stores scatter destination per source pixel (R32_UINT: x16|y16)
	// For forward-backward consistency check — guaranteed disocclusion detection
	ID3D11Texture2D* m_forwardMap[2] = {};
	ID3D11UnorderedAccessView* m_uavForwardMap[2] = {};
	ID3D11ShaderResourceView* m_srvForwardMap[2] = {};

	// Triple-buffered cached textures:
	// game writes build slot while warp reads published slot.
	ID3D11Texture2D* m_cachedColor[kAswCacheSlotCount][2] = {};
	ID3D11Texture2D* m_cachedMV[kAswCacheSlotCount][2] = {};
	ID3D11Texture2D* m_cachedDepth[kAswCacheSlotCount][2] = {};
	ID3D11Texture2D* m_cachedStencil[kAswCacheSlotCount][2] = {};
	ID3D11ShaderResourceView* m_srvColor[kAswCacheSlotCount][2] = {};
	ID3D11ShaderResourceView* m_srvMV[kAswCacheSlotCount][2] = {};
	ID3D11ShaderResourceView* m_srvDepth[kAswCacheSlotCount][2] = {};
	ID3D11ShaderResourceView* m_srvStencil[kAswCacheSlotCount][2] = {};

	// Per-eye warped output (compute shader writes here)
	ID3D11Texture2D* m_warpedOutput[2] = {};
	ID3D11UnorderedAccessView* m_uavOutput[2] = {};
	ID3D11Texture2D* m_forwardNpcOutput[2] = {};
	ID3D11UnorderedAccessView* m_uavForwardNpcOutput[2] = {};
	ID3D11ShaderResourceView* m_srvForwardNpcOutput[2] = {};

	// Stereo-combined XR swapchain for warped output (both eyes side-by-side)
	XrSwapchain m_outputSwapchain = {};
	std::vector<ID3D11Texture2D*> m_outputSwapchainImages; // all swapchain images

	// Stereo-combined XR swapchain for depth (R32_FLOAT, both eyes side-by-side)
	XrSwapchain m_depthSwapchain = {};
	std::vector<ID3D11Texture2D*> m_depthSwapchainImages;

	// Cached pose/FOV/depth metadata per slot.
	XrPosef m_slotPose[kAswCacheSlotCount][2] = {};
	XrPosef m_slotPrevPose[kAswCacheSlotCount][2] = {};
	bool m_slotHasPrevPose[kAswCacheSlotCount] = {};
	XrFovf m_slotFov[kAswCacheSlotCount][2] = {};
	float m_slotNear[kAswCacheSlotCount] = {};
	float m_slotFar[kAswCacheSlotCount] = {};
	uint64_t m_slotFrameId[kAswCacheSlotCount] = {};
	float m_slotCameraPosZ[kAswCacheSlotCount] = {};  // NiCamera Z at cache time (for vertical warp correction)
	float m_slotPrevCameraPosZ[kAswCacheSlotCount] = {};  // Previous frame's NiCamera Z (for cached vertical delta)
	XrTime m_slotDisplayTime[kAswCacheSlotCount] = {};  // predicted display time at capture
	std::chrono::steady_clock::time_point m_slotTimestamp[kAswCacheSlotCount] = {};
	uint32_t m_slotDepthDataW[kAswCacheSlotCount] = {};
	uint32_t m_slotDepthDataH[kAswCacheSlotCount] = {};
	uint32_t m_slotMVDataW[kAswCacheSlotCount] = {};
	uint32_t m_slotMVDataH[kAswCacheSlotCount] = {};
	int m_buildSlot = 0;
	bool m_buildEyeReady[2] = {};
	bool m_isMenuOpen = false;
	std::atomic<int> m_publishedSlot{ -1 };
	std::atomic<int> m_previousPublishedSlot{ -1 };  // N-1 warping: previous cycle's cache slot
	std::atomic<int> m_warpReadSlot{ -1 };
	XrPosef m_lastPublishedPose[2] = {};
	bool m_hasLastPublishedPose = false;
	uint64_t m_frameCounter = 0;

	// FP hand replay state
	FPReplayData* m_fpReplayPtr = nullptr;          // SKSE-allocated, same process
	ID3D11Texture2D* m_handColor[2] = {};           // RGBA8 per-eye hand color RT
	ID3D11RenderTargetView* m_handRTV[2] = {};
	ID3D11ShaderResourceView* m_srvHandColor[2] = {};
	ID3D11Texture2D* m_handDepthTex[2] = {};        // D32_FLOAT per-eye hand depth
	ID3D11DepthStencilView* m_handDSV[2] = {};
	ID3D11ShaderResourceView* m_srvHandDepth[2] = {};  // R32_FLOAT SRV on D32_FLOAT
	ID3D11DepthStencilState* m_handDSS = nullptr;   // LESS_EQUAL, depth write enabled
	ID3D11ComputeShader* m_handCompositeCS = nullptr;
	// Pre-allocated replay constant buffers (reused each frame via UpdateSubresource)
	static constexpr UINT kReplayCBSizes[7] = { 80, 48, 400, 3840, 3840, 1408, 48 };
	static constexpr int kReplayCBSlots[7] = { 0, 1, 2, 9, 10, 12, 13 };
	ID3D11Buffer* m_replayCB[7] = {};               // VS CBs: [0]=cb0, [1]=cb1, ..., [5]=cb12, [6]=cb13
	ID3D11Buffer* m_replayPSCB[3] = {};             // PS CBs: slots 0-2 (256 bytes each)
	ID3D11Buffer* m_handCompositeCB = nullptr;      // CB for CSHandComposite (controller deltas)

	// Constant buffer layout (must match HLSL, 16-byte aligned)
	struct WarpConstants {
		float poseDeltaMatrix[16]; // 4x4 row-major               — 64 bytes
		float resolution[2];      // output/color resolution       — 8
		float nearZ, farZ;        //                               — 8
		float fovTanLeft, fovTanRight, fovTanUp, fovTanDown; //    — 16
		float depthScale;          // multiplier on linearized depth
		float edgeFadeWidth;       // depth-edge fade threshold (depth ratio units)
		float nearFadeDepth;       // parallax fades to 0 below this depth; 0 = disabled
		float mvConfidence;        // 0=pure parallax, 1=full MV correction — 16
		float mvPixelScale;        // overall MV magnitude multiplier
		float depthResolution[2];  // actual depth dimensions (may differ from resolution)
		float _pad0;               // alignment padding             — 16
		float mvResolution[2];     // actual MV dimensions (render-res when camera MVs + upscaler)
		int _pad_npcMask;          // removed: was hasNpcMask
		float _pad1;               // alignment padding             — 16
		int debugMode;             // 0=normal, 60=depth scatter viz
		float _pad2[3];            // alignment to 16 bytes                        — 16
		float headRotMatrix[16];       // 4x4 row-major: head rotation delta (prev→cur cached pose)
		                               // Used to subtract head rot from camera MVs    — 64 bytes
		float clipToClipNoLoco[16];    // 4x4 col-major: prevVP * inv(curVP_original)
		                               // head rot+trans without locomotion; used for
		                               // exact head MV subtraction from camera MVs    — 64 bytes
		int hasClipToClipNoLoco;       // 1 if clipToClipNoLoco is valid, 0 = fallback to headRotMatrix
		float _pad3[3];                // alignment                                    — 16 bytes
		float forwardPoseDelta[16];    // 4x4 row-major: OLD view -> NEW view (forward scatter) — 64 bytes
		float locoScreenDir[2];        // screen-space locomotion direction (from actorPos delta) — 8 bytes
		float staticBlendFactor;       // 1.0 when near-stationary (blend scatter→prevColor), 0.0 when moving — 4 bytes
		float rotMVScale;              // 1.0 = rotation in residual, 0.0 = rotation suppressed (stop transition) — 4 bytes
		float reprojClipToClip[16];    // 4x4 col-major: warp output → frame N clip space          — 64 bytes
		int hasReprojData;             // 1 if frame N data bound for disocclusion fill              — 4 bytes
		float _padReproj[3];           // alignment                                                 — 12 bytes
		float fullWarpC2C[16];         // 4x4 col-major: warp output → cached clip (head + loco)    — 64 bytes
		int hasFullWarpC2C;            // 1 if fullWarpC2C is valid                                  — 4 bytes
		float _padFullC2C[3];          // alignment                                                 — 12 bytes
		float controllerUV_LR[4];      // [Lu, Lv, Ru, Rv] — projected controller positions in UV       — 16 bytes
		float controllerRadius;        // screen-space radius in UV units for hand detection circle  — 4 bytes
		int hasControllerPose;         // 1 if controllerUV is valid (both hands)                    — 4 bytes
		float controllerDeltaL[2];     // warp-time UV - cached-frame UV for left hand               — 8 bytes
		float controllerDeltaR[2];     // warp-time UV - cached-frame UV for right hand              — 8 bytes
		float fpControllerScale;       // controller delta multiplier (ini: aswFPControllerScale)    — 4 bytes
		int hasFPSphere;               // 1 if fpSphere* is valid                                   — 4 bytes
		float fpSphereRadius;          // FP bounding sphere radius                                 — 4 bytes
		float _padCtrl;                // alignment                                                 — 4 bytes
		float fpSphereCenter[4];       // [x,y,z,pad] FP sphere center in rendering space           — 16 bytes
		int isMenuOpen;                // 1 if a gameplay menu is open (skip MV corrections)       — 4 bytes
		int isLegacyMode;              // 1 = legacy ASW (parallax only, no MV/scatter)          — 4 bytes
		float _padPreFP[2];            // alignment to 16 bytes                                  — 8 bytes
		float fpScreenBoxes[16 * 4];   // Up to 16 AABBs: [minU, minV, maxU, maxV] × 16         — 256 bytes
		int fpBoxCount;                // Number of valid screen-space FP bounding boxes          — 4 bytes
		float _padBoxes[3];            // alignment to 16 bytes                                  — 12 bytes
	};                                 //                         total: 880 bytes

};
