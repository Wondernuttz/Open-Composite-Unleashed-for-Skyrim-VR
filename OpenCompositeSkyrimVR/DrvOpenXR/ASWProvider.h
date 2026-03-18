#pragma once

#include "XrDriverPrivate.h"
#include <d3d11.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <vector>
#include <chrono>

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
	/// Uses D3D12 command list internally (no D3D11 context needed).
	/// @param slotOverride  If >= 0, force reading from this cache slot instead of m_publishedSlot.
	bool WarpFrame(int eye, const XrPosef& newPose, int slotOverride = -1);

	/// Signal that CacheFrame GPU copies are complete.
	/// Called by game thread after CacheFrame (inserts D3D11 fence signal).
	void SignalCacheDone(ID3D11DeviceContext* ctx);

	/// Get the output XR swapchain for the warped frame (for layer assembly).
	XrSwapchain GetOutputSwapchain() const { return m_outputSwapchain; }

	/// Get the depth XR swapchain for the warped frame (for XR_KHR_composition_layer_depth).
	/// Currently returns XR_NULL_HANDLE: submitting unwarped depth with warped color causes
	/// double reprojection (runtime's depth-based reproject on top of our ASW warp = "flying borders").
	/// TODO: Once forward scatter outputs warped depth, enable this to submit matching depth.
	XrSwapchain GetDepthSwapchain() const { return XR_NULL_HANDLE; }

	/// Get per-eye sub-image rect for the warped output (stereo-combined).
	XrRect2Di GetOutputRect(int eye) const;

	/// CPU-wait for the cache fence (D3D11 game draws + cache copies complete).
	/// Call BEFORE xrWaitFrame(warp) so the GPU drains during the fence wait,
	/// and xrWaitFrame returns quickly (already past target time).
	/// WarpFrame will then skip its internal fence wait.
	void WaitForCacheFence();

	/// Close + execute the D3D12 compute command list from WarpFrame.
	/// Signals fence but does NOT CPU-wait. GPU runs asynchronously.
	/// Call after WarpFrame for both eyes, BEFORE xrWaitFrame(warp).
	/// Returns the fence value to wait on later.
	uint64_t FlushWarpComputeAsync();

	/// CPU-wait for a previously flushed compute fence, then copy warped
	/// output to swapchain and release. Call AFTER xrWaitFrame(warp).
	bool FinishWarpCopy(uint64_t computeFenceVal);

	/// Acquire output swapchain, copy warped textures, release.
	/// Call once after WarpFrame for both eyes.
	bool SubmitWarpedOutput(ID3D11DeviceContext* ctx);

	/// Close + execute the D3D12 command list recorded by WarpFrame(),
	/// signal fence but DO NOT CPU-wait. Call after WarpFrame() for both eyes.
	/// The GPU work runs asynchronously; call WaitForWarpCompletion() before
	/// reading the result (or it's called automatically by CopyPrecomputedWarpToSwapchain).
	bool FlushWarpCommandList();

	/// CPU-wait for the D3D12 warp compute to finish (fence from FlushWarpCommandList).
	/// Called automatically by CopyPrecomputedWarpToSwapchain, but can be called
	/// explicitly if needed. Returns true if the warp is ready.
	bool WaitForWarpCompletion();

	/// Copy the precomputed warp output (from a prior FlushWarpCommandList)
	/// to the output swapchain. Acquire → D3D11 copy → release.
	/// Call on a CLEAN D3D11 queue (before game signal) for zero contention.
	bool CopyPrecomputedWarpToSwapchain(ID3D11DeviceContext* ctx, uint32_t* outSwapIdx = nullptr);

	/// True when a precomputed warp is ready (or pending GPU completion) for CopyPrecomputedWarpToSwapchain.
	bool HasPrecomputedWarp() const { return m_precomputedWarpReady || m_warpFenceWaitPending; }

	/// Enable/disable GPU gap insertion. Must be enabled by the worker thread
	/// before InsertGpuGap will do anything (prevents stall with no release).
	void EnableGpuGap(bool enabled) { m_gpuGapEnabled.store(enabled, std::memory_order_release); }

	/// Insert a GPU-side stall on the D3D11 command stream (after left eye draws).
	/// The GPU stalls at this point until ReleaseGpuGap() is called from the worker.
	/// CPU returns immediately — no blocking. No-op if not enabled.
	bool InsertGpuGap(ID3D11DeviceContext* ctx);

	/// Release the GPU stall by signaling the gap fence from the D3D12 queue.
	/// Called by the worker thread when xrWaitFrame(warp) returns (~13ms).
	/// Thread-safe (D3D12 Signal is thread-safe).
	void ReleaseGpuGap();

	/// Signal that the game thread's staging copy is GPU-submitted (called after D3D11 Flush).
	/// The D3D12 game copy queue waits on this fence before reading the staging texture.
	void SignalGameStagingDone(ID3D11DeviceContext* ctx);

	/// Copy game staging textures to swapchain images via D3D12 (both eyes in one batch).
	/// Bypasses D3D11 GPU queue contention (game draws don't block the copy).
	/// @param count  Number of copies (typically 2, one per eye)
	/// @param stagingTexs  Array of source staging textures
	/// @param swapchainImages  Array of destination swapchain images
	/// Returns true if D3D12 copy was used, false if fallback D3D11 copy needed.
	bool CopyGameStagingToSwapchainD3D12(
	    ID3D11DeviceContext* ctx,
	    int count,
	    ID3D11Texture2D** stagingTexs,
	    ID3D11Texture2D** swapchainImages);

	/// Acquire output swapchain, clear to black, release.
	/// Used for debug mode 50 to submit a valid black layer (prevents runtime ATW).
	bool SubmitBlackOutput(ID3D11DeviceContext* ctx);

	bool HasCachedFrame() const { return m_publishedSlot.load(std::memory_order_acquire) >= 0; }

	/// True when at least 2 frames have been cached (N-1 warping can start).
	bool HasPreviousCachedFrame() const { return m_previousPublishedSlot.load(std::memory_order_acquire) >= 0; }

	/// Get the most recently published cache slot index (-1 if none).
	int GetPublishedSlot() const { return m_publishedSlot.load(std::memory_order_acquire); }

	/// Get the latest cache slot whose D3D12 fence has completed.
	/// Prefers preferredSlot if its fence is ready, else returns the most recent ready slot.
	/// Returns -1 if no slot is ready.
	int GetLatestReadySlot(int preferredSlot) const;

	/// Store the predicted display time associated with a cache slot.
	void SetSlotDisplayTime(int slot, XrTime t) {
		if (slot >= 0 && slot < (int)kAswCacheSlotCount)
			m_slotDisplayTime[slot] = t;
	}

	/// Get the predicted display time for a cache slot.
	XrTime GetSlotDisplayTime(int slot) const {
		return (slot >= 0 && slot < (int)kAswCacheSlotCount) ? m_slotDisplayTime[slot] : 0;
	}

	/// Get the current global cache fence value (for CPU-side fence waits).
	uint64_t GetCacheFenceValue() const { return m_cacheFenceValue.load(std::memory_order_acquire); }

	/// Non-blocking check: what fence value has the GPU completed so far?
	uint64_t GetFenceCompletedValue() const { return m_d3d12Fence ? m_d3d12Fence->GetCompletedValue() : 0; }

	/// CPU-wait for a specific fence value to complete on the shared D3D11↔D3D12 fence.
	/// Returns true if the wait succeeded, false on timeout or error.
	bool WaitForFenceValue(uint64_t value, DWORD timeoutMs = 50);

	/// Age of cached frame in milliseconds (since last CacheFrame call)
	float GetCacheAgeMs() const {
		int slot = GetActiveCacheSlot();
		if (slot < 0) return 1e9f;
		auto now = std::chrono::steady_clock::now();
		return std::chrono::duration<float, std::milli>(now - m_slotTimestamp[slot]).count();
	}

	/// Set stick yaw delta (old − new, radians) for stick turn correction.
	void SetLocomotionYaw(float yawDelta) { m_locoYaw = yawDelta; }

	/// Set locomotion translation delta (view-space, game units) for thumbstick movement correction.
	/// Computed from worldToCam delta minus OpenXR head translation.
	void SetLocomotionTranslation(float x, float y, float z) {
		m_locoTransX = x; m_locoTransY = y; m_locoTransZ = z;
	}

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

	/// Set the MV timing ratio for this injected frame.
	/// ratio = (T_inject - T_realFrame) / (T_realFrame - T_prevRealFrame)
	/// Gives the exact interpolation weight (0.5 at half-refresh).
	void SetMVTimingRatio(float r) { m_mvTimingRatio = r; }

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

	/// Get per-eye dimensions ASW was initialized with (for detecting upscaler resolution change)
	uint32_t GetEyeWidth() const { return m_eyeWidth; }
	uint32_t GetEyeHeight() const { return m_eyeHeight; }

private:
	bool CreateComputeShader(ID3D11Device* device);
	bool CreateStagingTextures(ID3D11Device* device);
	bool CreateOutputSwapchain(uint32_t width, uint32_t height);
	bool CreateDepthSwapchain(uint32_t width, uint32_t height);

	// D3D12 warp pipeline setup
	bool CreateDX12Device(IDXGIAdapter* adapter);
	bool CreateSharedFence();
	bool CreateDX12ComputePipeline();
	void ShareTextureD3D11ToD3D12(ID3D11Texture2D* d3d11Tex, ID3D12Resource** outD3d12);

	// D3D12 descriptor heap helpers
	D3D12_GPU_DESCRIPTOR_HANDLE GetSrvGpuHandle(int slot, int eye) const;
	D3D12_GPU_DESCRIPTOR_HANDLE GetUavGpuHandle(int eye) const;

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

	float m_locoYaw   = 0.0f;  // stick yaw delta (old − new, radians)
	float m_locoTransX = 0.0f, m_locoTransY = 0.0f, m_locoTransZ = 0.0f; // loco translation (view-space, game units)
	float m_mvTimingRatio = 0.5f; // dynamic MV extrapolation weight (computed from frame timing)
	float m_cachedClipToClip[2][16] = {};          // per-eye clipToClip from camera MV computation
	bool m_hasClipToClip = false;

	// Per-slot clipToClipNoLoco: buffered with the cache slot so WarpFrame reads the
	// matrix from the SAME frame as the cached color/MV/depth textures.
	// Without per-slot buffering, the game thread can overwrite this with the NEXT frame's
	// data before the worker reads it (race condition in the P-after-S pipeline order).
	float m_slotClipToClipNoLoco[kAswCacheSlotCount][2][16] = {};
	bool m_slotHasClipToClipNoLoco[kAswCacheSlotCount] = {};
	XrPosef m_precompPose[2] = {};  // predicted poses from WarpFrame (for composition layer)

	bool m_ready = false;
	uint32_t m_eyeWidth = 0, m_eyeHeight = 0;
	ID3D11Device* m_device = nullptr; // kept for obtaining immediate context in XrBackend

	// Compute shaders
	ID3D11ComputeShader* m_warpCS = nullptr;       // CSMain (backward warp)
	ID3D11ComputeShader* m_clearCS = nullptr;       // CSClear (clear atomicDepth + output)
	ID3D11ComputeShader* m_forwardCS = nullptr;     // CSForward (legacy single-pass scatter)
	ID3D11ComputeShader* m_forwardDepthCS = nullptr; // CSForwardDepth (two-pass: depth only)
	ID3D11ComputeShader* m_forwardColorCS = nullptr; // CSForwardColor (two-pass: color with finalized depth)
	ID3D11ComputeShader* m_dilateCS = nullptr;      // CSDilate (gap fill)
	ID3D11Buffer* m_constantBuffer = nullptr;
	ID3D11SamplerState* m_linearSampler = nullptr;

	// Forward scatter: per-eye atomic depth buffer for depth test
	ID3D11Texture2D* m_atomicDepth[2] = {};
	ID3D11UnorderedAccessView* m_uavAtomicDepth[2] = {};

	// Triple-buffered cached textures:
	// game writes build slot while warp reads published slot.
	ID3D11Texture2D* m_cachedColor[kAswCacheSlotCount][2] = {};
	ID3D11Texture2D* m_cachedMV[kAswCacheSlotCount][2] = {};
	ID3D11Texture2D* m_cachedDepth[kAswCacheSlotCount][2] = {};
	ID3D11ShaderResourceView* m_srvColor[kAswCacheSlotCount][2] = {};
	ID3D11ShaderResourceView* m_srvMV[kAswCacheSlotCount][2] = {};
	ID3D11ShaderResourceView* m_srvDepth[kAswCacheSlotCount][2] = {};

	// Per-eye warped output (compute shader writes here)
	ID3D11Texture2D* m_warpedOutput[2] = {};
	ID3D11UnorderedAccessView* m_uavOutput[2] = {};

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
	XrTime m_slotDisplayTime[kAswCacheSlotCount] = {};  // predicted display time at capture
	std::chrono::steady_clock::time_point m_slotTimestamp[kAswCacheSlotCount] = {};
	uint32_t m_slotDepthDataW[kAswCacheSlotCount] = {};
	uint32_t m_slotDepthDataH[kAswCacheSlotCount] = {};
	uint32_t m_slotMVDataW[kAswCacheSlotCount] = {};
	uint32_t m_slotMVDataH[kAswCacheSlotCount] = {};
	int m_buildSlot = 0;
	bool m_buildEyeReady[2] = {};
	std::atomic<int> m_publishedSlot{ -1 };
	std::atomic<int> m_previousPublishedSlot{ -1 };  // N-1 warping: previous cycle's cache slot
	std::atomic<int> m_warpReadSlot{ -1 };
	XrPosef m_lastPublishedPose[2] = {};
	bool m_hasLastPublishedPose = false;
	uint64_t m_frameCounter = 0;

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
		float _pad1[2];            // alignment padding             — 16
		int debugMode;             // 0=normal, 1=depth viz, 2=MV magnitude viz
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
		float _pad4;                   // alignment                                    — 4 bytes
	};                                 //                         total: 384 bytes

	// Depth/MV data dimensions are tracked per cache slot.

	// ── D3D12 warp pipeline ──
	// Separate D3D12 command queue for warp compute, eliminates D3D11 GPU queue contention.
	ID3D12Device* m_d3d12Device = nullptr;
	ID3D12CommandQueue* m_d3d12CmdQueue = nullptr;
	ID3D12CommandAllocator* m_d3d12CmdAlloc = nullptr;      // Warp compute (step P)
	ID3D12GraphicsCommandList* m_d3d12CmdList = nullptr;   // Warp compute (step P)
	ID3D12RootSignature* m_d3d12RootSig = nullptr;
	ID3D12PipelineState* m_d3d12PipelineState = nullptr;     // CSMain PSO
	ID3D12PipelineState* m_d3d12ClearPSO = nullptr;          // CSClear PSO
	ID3D12PipelineState* m_d3d12ForwardPSO = nullptr;        // CSForward PSO (legacy)
	ID3D12PipelineState* m_d3d12ForwardDepthPSO = nullptr;   // CSForwardDepth PSO (two-pass)
	ID3D12PipelineState* m_d3d12ForwardColorPSO = nullptr;   // CSForwardColor PSO (two-pass)
	ID3D12PipelineState* m_d3d12DilatePSO = nullptr;         // CSDilate PSO
	ID3D12DescriptorHeap* m_d3d12SrvUavHeap = nullptr;
	ID3D12Resource* m_d3d12ConstantBuffer = nullptr;     // Upload heap, CPU-writable (2x regions, one per eye)
	void* m_d3d12CbMappedPtr = nullptr;                  // Persistent map (base of 2-eye buffer)
	uint32_t m_d3d12CbEyeStride = 0;                    // Byte offset between eye 0 and eye 1 CB regions
	uint32_t m_d3d12HeapDescriptorSize = 0;              // CBV_SRV_UAV descriptor increment

	// Shared fence (D3D11 ↔ D3D12 synchronization)
	ID3D12Fence* m_d3d12Fence = nullptr;
	ID3D11Fence* m_d3d11Fence = nullptr;
	ID3D11Device5* m_d3d11Device5 = nullptr;
	HANDLE m_fenceSharedHandle = nullptr;
	HANDLE m_fenceEvent = nullptr;
	uint64_t m_fenceValue = 0;
	uint64_t m_warpFenceValue = 0;  // Fence value for warp dispatch completion
	std::atomic<uint64_t> m_cacheFenceValue{ 0 };  // Latest cache-done fence value
	uint64_t m_slotCacheFenceValue[kAswCacheSlotCount] = {};  // Per-slot cache-done fence value
	bool m_cacheFenceWaitedEarly = false;  // Set by WaitForCacheFence(), cleared by WarpFrame
	std::atomic<uint64_t> m_stagingFenceValue{ 0 }; // Latest staging-flush-done fence value

	// ── GPU gap fence (D3D12→D3D11) ──
	// Separate from cache fence. InsertGpuGap stalls D3D11 GPU, ReleaseGpuGap signals from D3D12.
	ID3D12Fence* m_d3d12GapFence = nullptr;
	ID3D11Fence* m_d3d11GapFence = nullptr;
	HANDLE m_gapFenceSharedHandle = nullptr;
	std::atomic<uint64_t> m_gapFenceValue{ 0 };   // monotonic counter
	std::atomic<uint64_t> m_gapTargetValue{ 0 };   // value GPU is currently waiting for
	std::atomic<bool> m_gpuGapEnabled{ false };     // must be enabled by worker before InsertGpuGap fires

	// Shared resources (D3D11 textures opened on D3D12)
	ID3D12Resource* m_d3d12CachedColor[kAswCacheSlotCount][2] = {};
	ID3D12Resource* m_d3d12CachedMV[kAswCacheSlotCount][2] = {};
	ID3D12Resource* m_d3d12CachedDepth[kAswCacheSlotCount][2] = {};
	ID3D12Resource* m_d3d12WarpedOutput[2] = {};
	ID3D12Resource* m_d3d12AtomicDepth[2] = {};    // forward scatter depth test (R32_UINT)
	bool m_d3d12Ready = false;  // true when D3D12 pipeline is fully initialized
	bool m_precomputedWarpReady = false;  // true when warp GPU work is confirmed done
	bool m_warpFenceWaitPending = false;  // true when GPU work submitted but not yet CPU-waited
	uint64_t m_precomputedWarpFenceValue = 0;  // fence value for precomputed warp sync

	// D3D12 swapchain image sharing — bypasses D3D11 GPU queue entirely
	static constexpr uint32_t kMaxSwapchainImages = 4;
	ID3D12Resource* m_d3d12SwapchainImages[kMaxSwapchainImages] = {};
	bool m_d3d12DirectCopy = false;  // true when swapchain images are shared to D3D12

	// D3D12 debug stripe upload buffer (for aswDebugMode 51)
	ID3D12Resource* m_d3d12StripeUpload = nullptr;
	uint32_t m_d3d12StripeWidth = 0;   // cached from swapchain desc
	uint32_t m_d3d12StripeH = 64;

	// ── D3D12 game staging copy ──
	// Separate command allocator/list for game staging→swapchain copies (step G2).
	// Independent of warp compute (step P) and warp copy (step W2) allocators.
	ID3D12CommandAllocator* m_d3d12GameCopyCmdAlloc = nullptr;
	ID3D12GraphicsCommandList* m_d3d12GameCopyCmdList = nullptr;
	bool m_gameCopyFenceWaitPending = false;
	uint64_t m_gameCopyFenceValue = 0;
	HANDLE m_gameCopyFenceEvent = nullptr;

	// Cache of D3D12 handles for shared D3D11 textures (lazy-populated).
	// Key: raw D3D11 texture pointer. Value: D3D12 resource.
	// Used for both staging textures and game swapchain images.
	static constexpr uint32_t kMaxGameSharedTextures = 16;
	struct SharedTexEntry {
		ID3D11Texture2D* d3d11Tex = nullptr;
		ID3D12Resource* d3d12Res = nullptr;
	};
	SharedTexEntry m_gameSharedTexCache[kMaxGameSharedTextures] = {};
	uint32_t m_gameSharedTexCount = 0;

	ID3D12Resource* GetOrShareTextureD3D12(ID3D11Texture2D* d3d11Tex);

	// Diagnostic capture: saves warp inputs/outputs to disk for offline iteration
	void CaptureWarpDiagnostics(int eye, int slot, const WarpConstants& cb,
	    ID3D11DeviceContext* ctx, bool isPreDispatch);
};
