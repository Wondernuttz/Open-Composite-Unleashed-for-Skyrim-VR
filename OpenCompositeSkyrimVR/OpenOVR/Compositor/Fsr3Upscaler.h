#pragma once

#ifdef OC_HAS_FSR3

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdint>

// FidelityFX API types (we load functions dynamically via GetProcAddress)
#pragma warning(push)
#pragma warning(disable: 4005) // macro redefinition — ffx_api.h defines FFX_API_ENTRY
#include <ffx_api.h>
#pragma warning(pop)

class Fsr3Upscaler {
public:
	Fsr3Upscaler() = default;
	~Fsr3Upscaler();

	Fsr3Upscaler(const Fsr3Upscaler&) = delete;
	Fsr3Upscaler& operator=(const Fsr3Upscaler&) = delete;

	/// Initialize DX12 device on same adapter as DX11, load FidelityFX DLLs.
	bool Initialize(ID3D11Device* d3d11Device);
	void Shutdown();
	bool IsReady() const { return m_ready; }

	/// Parameters for a single FSR 3 temporal upscale dispatch.
	struct DispatchParams {
		ID3D11Texture2D* color;              ///< Game's rendered color at render resolution
		const D3D11_BOX* colorSourceRegion;  ///< Per-eye sub-region of color (nullptr = full)
		ID3D11Texture2D* motionVectors;      ///< MV texture (R16G16_FLOAT, may be stereo-combined)
		const D3D11_BOX* mvSourceRegion;     ///< Per-eye sub-region of MV texture (nullptr = full)
		ID3D11Texture2D* depth;              ///< Depth texture (D32_FLOAT or R32_FLOAT)
		const D3D11_BOX* depthSourceRegion;  ///< Per-eye sub-region of depth (nullptr = full)
		ID3D11Texture2D* reactiveMask;       ///< Reactive mask (R8_UNORM, 0=stable 1=reactive), nullptr to skip
		const D3D11_BOX* reactiveSourceRegion; ///< Per-eye sub-region of reactive mask (nullptr = full)
		float jitterX, jitterY;              ///< Sub-pixel jitter offset applied to camera
		float deltaTimeMs;                   ///< Frame time in milliseconds
		uint32_t renderWidth, renderHeight;  ///< Input (game) resolution per eye
		uint32_t outputWidth, outputHeight;  ///< Target output (display) resolution per eye
		float cameraNear, cameraFar;         ///< Camera planes
		float cameraFovY;                    ///< Vertical FOV in radians
		float sharpness;                     ///< FSR3 built-in RCAS sharpness (0.0-1.0)
		bool reset;                          ///< True on camera teleport / scene change
		bool jitterCancellation;             ///< MVs already include jitter — FSR3 should compensate
		float viewToMeters;                  ///< View-space to meters factor (Skyrim: 0.01428)
		float mvScale;                       ///< MV magnitude multiplier (1.0 = raw engine data)
		int debugMode;                       ///< 0=off, 1=FSR3 debug overlay, 2=bypass, 3=depth viz
	};

	/// Dispatch FSR 3 temporal upscaling for one eye (0=left, 1=right).
	/// Async pipeline: submits DX12 work without waiting, returns previous frame's output.
	/// First frame returns false (no output yet). GetOutputDX11() returns the completed output.
	bool Dispatch(int eyeIdx, ID3D11DeviceContext* d3d11Ctx, const DispatchParams& params);
	bool DispatchWarp(int eyeIdx, ID3D11DeviceContext* d3d11Ctx, const DispatchParams& params);

	/// Get the DX11 texture containing the upscaled output for the given eye.
	ID3D11Texture2D* GetOutputDX11(int eyeIdx) const;

	// --- Static jitter utilities (Halton[2,3] sequence, matches FSR convention) ---
	static void GetJitterOffset(float* outX, float* outY, int frameIndex, int phaseCount);
	static int  GetJitterPhaseCount(uint32_t renderWidth, uint32_t displayWidth);

private:
	bool LoadFfxDll();
	bool CreateDX12Device(IDXGIAdapter* adapter);
	bool CreateSharedFence();
	bool EnsureSharedTextures(uint32_t renderW, uint32_t renderH,
	    uint32_t outputW, uint32_t outputH, DXGI_FORMAT colorFormat);
	bool EnsureFsrContexts(uint32_t renderW, uint32_t renderH,
	    uint32_t outputW, uint32_t outputH, bool jitterCancellation);
	bool EnsureWarpFsrContexts(uint32_t renderW, uint32_t renderH,
	    uint32_t outputW, uint32_t outputH);
	bool DispatchInternal(int eyeIdx, ID3D11DeviceContext* d3d11Ctx,
	    const DispatchParams& params, bool warpContext);
	void DestroySharedTextures();
	void DestroyFsrContexts();

	bool CreateSharedTexture(uint32_t width, uint32_t height, DXGI_FORMAT format,
	    bool allowUAV, ID3D12Resource** outDX12, ID3D11Texture2D** outDX11, HANDLE* outHandle);
	void DestroySharedTexture(ID3D12Resource** dx12, ID3D11Texture2D** dx11, HANDLE* handle);

	bool m_ready = false;

	// FidelityFX function pointers (loaded via LoadLibrary)
	typedef ffxReturnCode_t (*PfnCreateContext)(ffxContext*, ffxCreateContextDescHeader*, const ffxAllocationCallbacks*);
	typedef ffxReturnCode_t (*PfnDestroyContext)(ffxContext*, const ffxAllocationCallbacks*);
	typedef ffxReturnCode_t (*PfnConfigure)(ffxContext*, const ffxConfigureDescHeader*);
	typedef ffxReturnCode_t (*PfnQuery)(ffxContext*, ffxQueryDescHeader*);
	typedef ffxReturnCode_t (*PfnDispatch)(ffxContext*, const ffxDispatchDescHeader*);

	PfnCreateContext  m_ffxCreateContext = nullptr;
	PfnDestroyContext m_ffxDestroyContext = nullptr;
	PfnConfigure      m_ffxConfigure = nullptr;
	PfnQuery          m_ffxQuery = nullptr;
	PfnDispatch       m_ffxDispatch = nullptr;
	HMODULE           m_ffxModule = nullptr;

	// DX11 interfaces (NOT owned)
	ID3D11Device*  m_d3d11Device = nullptr;
	ID3D11Device5* m_d3d11Device5 = nullptr;

	// DX12 infrastructure (OWNED)
	ID3D12Device*              m_d3d12Device = nullptr;
	ID3D12CommandQueue*        m_cmdQueue = nullptr;
	ID3D12CommandAllocator*    m_cmdAlloc[2] = {};  // per eye
	ID3D12GraphicsCommandList* m_cmdList[2] = {};   // per eye

	// Cross-API fence
	ID3D12Fence* m_d3d12Fence = nullptr;
	ID3D11Fence* m_d3d11Fence = nullptr;
	HANDLE       m_fenceSharedHandle = nullptr;
	HANDLE       m_fenceEvent = nullptr;
	uint64_t     m_fenceValue = 0;

	// Per-eye shared textures (DX12 creates with HEAP_FLAG_SHARED, DX11 opens)
	struct SharedEyeTextures {
		ID3D12Resource*  colorDX12 = nullptr;
		ID3D12Resource*  mvDX12 = nullptr;
		ID3D12Resource*  depthDX12 = nullptr;
		ID3D12Resource*  reactiveDX12 = nullptr;
		ID3D12Resource*  outputDX12[2] = {};    // double-buffered for async pipeline

		ID3D11Texture2D* colorDX11 = nullptr;
		ID3D11Texture2D* mvDX11 = nullptr;
		ID3D11Texture2D* depthDX11 = nullptr;
		ID3D11Texture2D* reactiveDX11 = nullptr;
		ID3D11Texture2D* outputDX11[2] = {};    // double-buffered for async pipeline

		HANDLE colorHandle = nullptr;
		HANDLE mvHandle = nullptr;
		HANDLE depthHandle = nullptr;
		HANDLE reactiveHandle = nullptr;
		HANDLE outputHandle[2] = {};             // double-buffered for async pipeline
	};
	SharedEyeTextures m_eye[2];

	// FSR 3 upscaler contexts (opaque handles from high-level API)
	ffxContext m_fsrContext[2] = {};
	ffxContext m_warpFsrContext[2] = {};
	bool m_fsrContextsCreated = false;
	bool m_warpFsrContextsCreated = false;
	bool m_fsrConfigApplied = false;

	// Async pipeline state: double-buffered output with 1-frame delay
	int m_outputWrite[2] = {0, 0};         // per eye: which output buffer DX12 writes to next
	bool m_hasOutput[2] = {false, false};   // per eye: has a completed output available?
	uint64_t m_eyeFence[2] = {0, 0};       // per eye: fence value of last DX12 submit

	// Cached dimensions for lazy re-creation
	uint32_t m_renderWidth = 0, m_renderHeight = 0;
	uint32_t m_outputWidth = 0, m_outputHeight = 0;
	DXGI_FORMAT m_colorFormat = DXGI_FORMAT_UNKNOWN;
};

#endif // OC_HAS_FSR3
