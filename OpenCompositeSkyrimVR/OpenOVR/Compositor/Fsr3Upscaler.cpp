#include "stdafx.h"

#if defined(SUPPORT_DX) && defined(SUPPORT_DX11) && defined(OC_HAS_FSR3)

#include "Fsr3Upscaler.h"
#include "../Misc/Config.h"

// FidelityFX SDK headers (high-level API)
#include <ffx_api.h>
#include <ffx_api_types.h>
#include <ffx_upscale.h>
#include <dx12/ffx_api_dx12.h>

#include <cmath>
#include <algorithm>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

// ============================================================================
// Static jitter utilities (Halton[2,3] sequence — matches FSR 2/3 convention)
// ============================================================================

static float HaltonSequence(int index, int base)
{
	float f = 1.0f, r = 0.0f;
	while (index > 0) {
		f /= static_cast<float>(base);
		r += f * static_cast<float>(index % base);
		index /= base;
	}
	return r;
}

void Fsr3Upscaler::GetJitterOffset(float* outX, float* outY, int frameIndex, int phaseCount)
{
	int idx = (frameIndex % phaseCount) + 1; // 1-based for Halton
	*outX = HaltonSequence(idx, 2) - 0.5f;
	*outY = HaltonSequence(idx, 3) - 0.5f;
}

int Fsr3Upscaler::GetJitterPhaseCount(uint32_t renderWidth, uint32_t displayWidth)
{
	float ratio = static_cast<float>(displayWidth) / static_cast<float>(renderWidth);
	return std::max(1, static_cast<int>(8.0f * ratio * ratio + 0.5f));
}

// ============================================================================
// Lifecycle
// ============================================================================

Fsr3Upscaler::~Fsr3Upscaler()
{
	Shutdown();
}

bool Fsr3Upscaler::Initialize(ID3D11Device* d3d11Device)
{
	if (m_ready) return true;
	if (!d3d11Device) return false;

	m_d3d11Device = d3d11Device;

	// Need ID3D11Device5 for shared fences (Windows 10 1703+)
	HRESULT hr = d3d11Device->QueryInterface(IID_PPV_ARGS(&m_d3d11Device5));
	if (FAILED(hr)) {
		OOVR_LOG("FSR3: ID3D11Device5 not available (need Windows 10 1703+)");
		return false;
	}

	// Load FidelityFX loader DLL
	if (!LoadFfxDll()) {
		Shutdown();
		return false;
	}

	// Get DXGI adapter from DX11 device (to create DX12 device on same GPU)
	IDXGIDevice* dxgiDevice = nullptr;
	hr = d3d11Device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
	if (FAILED(hr)) {
		OOVR_LOGF("FSR3: QueryInterface(IDXGIDevice) failed (hr=0x%08X)", hr);
		Shutdown();
		return false;
	}

	IDXGIAdapter* adapter = nullptr;
	hr = dxgiDevice->GetAdapter(&adapter);
	dxgiDevice->Release();
	if (FAILED(hr)) {
		OOVR_LOGF("FSR3: GetAdapter failed (hr=0x%08X)", hr);
		Shutdown();
		return false;
	}

	// Create DX12 device on same adapter
	bool ok = CreateDX12Device(adapter);
	adapter->Release();
	if (!ok) {
		Shutdown();
		return false;
	}

	// Create shared cross-API fence
	if (!CreateSharedFence()) {
		Shutdown();
		return false;
	}

	m_ready = true;
	OOVR_LOG("FSR3: Initialized — DX12 device + shared fence ready");
	return true;
}

void Fsr3Upscaler::Shutdown()
{
	m_ready = false;

	DestroyFsrContexts();
	DestroySharedTextures();

	// Fence cleanup
	if (m_d3d11Fence) { m_d3d11Fence->Release(); m_d3d11Fence = nullptr; }
	if (m_d3d12Fence) { m_d3d12Fence->Release(); m_d3d12Fence = nullptr; }
	if (m_fenceSharedHandle) { CloseHandle(m_fenceSharedHandle); m_fenceSharedHandle = nullptr; }
	if (m_fenceEvent) { CloseHandle(m_fenceEvent); m_fenceEvent = nullptr; }
	m_fenceValue = 0;

	// DX12 command infrastructure
	for (int i = 0; i < 2; i++) {
		if (m_cmdList[i]) { m_cmdList[i]->Release(); m_cmdList[i] = nullptr; }
		if (m_cmdAlloc[i]) { m_cmdAlloc[i]->Release(); m_cmdAlloc[i] = nullptr; }
	}
	if (m_cmdQueue) { m_cmdQueue->Release(); m_cmdQueue = nullptr; }
	if (m_d3d12Device) { m_d3d12Device->Release(); m_d3d12Device = nullptr; }

	// DX11 QI refs
	if (m_d3d11Device5) { m_d3d11Device5->Release(); m_d3d11Device5 = nullptr; }
	m_d3d11Device = nullptr; // Not owned

	// FFX DLL
	if (m_ffxModule) { FreeLibrary(m_ffxModule); m_ffxModule = nullptr; }
	m_ffxCreateContext = nullptr;
	m_ffxDestroyContext = nullptr;
	m_ffxConfigure = nullptr;
	m_ffxQuery = nullptr;
	m_ffxDispatch = nullptr;
}

// ============================================================================
// Internal setup
// ============================================================================

bool Fsr3Upscaler::LoadFfxDll()
{
	// Load the upscaler DLL DIRECTLY — bypasses the loader's provider routing
	// which has a vtable mismatch crash (loader v2.1.0 vs upscaler v4.0.3).
	// The upscaler exports the same 5 high-level API functions as the loader,
	// so it works standalone without the loader intermediary.
	m_ffxModule = LoadLibraryW(L"amd_fidelityfx_upscaler_dx12.dll");
	if (!m_ffxModule) {
		OOVR_LOGF("FSR3: amd_fidelityfx_upscaler_dx12.dll not found (error %lu)", GetLastError());
		return false;
	}

	m_ffxCreateContext  = (PfnCreateContext)GetProcAddress(m_ffxModule, "ffxCreateContext");
	m_ffxDestroyContext = (PfnDestroyContext)GetProcAddress(m_ffxModule, "ffxDestroyContext");
	m_ffxConfigure      = (PfnConfigure)GetProcAddress(m_ffxModule, "ffxConfigure");
	m_ffxQuery          = (PfnQuery)GetProcAddress(m_ffxModule, "ffxQuery");
	m_ffxDispatch       = (PfnDispatch)GetProcAddress(m_ffxModule, "ffxDispatch");

	if (!m_ffxCreateContext || !m_ffxDestroyContext || !m_ffxDispatch) {
		OOVR_LOG("FSR3: Failed to resolve FFX function pointers from loader DLL");
		FreeLibrary(m_ffxModule);
		m_ffxModule = nullptr;
		return false;
	}

	OOVR_LOG("FSR3: FidelityFX upscaler DLL loaded directly (bypassing loader)");
	return true;
}

bool Fsr3Upscaler::CreateDX12Device(IDXGIAdapter* adapter)
{
	HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_d3d12Device));
	if (FAILED(hr)) {
		OOVR_LOGF("FSR3: D3D12CreateDevice failed (hr=0x%08X)", hr);
		return false;
	}

	// DIRECT command queue — FFX upscaler provider requires DIRECT type for its
	// internal dispatch table. COMPUTE type causes crash (unimplemented vtable slot).
	D3D12_COMMAND_QUEUE_DESC queueDesc = {};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
	hr = m_d3d12Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_cmdQueue));
	if (FAILED(hr)) {
		OOVR_LOGF("FSR3: CreateCommandQueue(DIRECT) failed (hr=0x%08X)", hr);
		return false;
	}

	// Per-eye command allocators and command lists
	for (int i = 0; i < 2; i++) {
		hr = m_d3d12Device->CreateCommandAllocator(
		    D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_cmdAlloc[i]));
		if (FAILED(hr)) {
			OOVR_LOGF("FSR3: CreateCommandAllocator[%d] failed (hr=0x%08X)", i, hr);
			return false;
		}

		hr = m_d3d12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
		    m_cmdAlloc[i], nullptr, IID_PPV_ARGS(&m_cmdList[i]));
		if (FAILED(hr)) {
			OOVR_LOGF("FSR3: CreateCommandList[%d] failed (hr=0x%08X)", i, hr);
			return false;
		}

		// Command lists start in recording state; close until first use
		m_cmdList[i]->Close();
	}

	OOVR_LOG("FSR3: DX12 device + DIRECT queue created on same adapter");
	return true;
}

bool Fsr3Upscaler::CreateSharedFence()
{
	// Create fence on DX12 side
	HRESULT hr = m_d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m_d3d12Fence));
	if (FAILED(hr)) {
		OOVR_LOGF("FSR3: CreateFence(SHARED) failed (hr=0x%08X)", hr);
		return false;
	}

	// Get shared HANDLE
	hr = m_d3d12Device->CreateSharedHandle(m_d3d12Fence, nullptr, GENERIC_ALL, nullptr, &m_fenceSharedHandle);
	if (FAILED(hr)) {
		OOVR_LOGF("FSR3: CreateSharedHandle(fence) failed (hr=0x%08X)", hr);
		return false;
	}

	// Open on DX11 side
	hr = m_d3d11Device5->OpenSharedFence(m_fenceSharedHandle, IID_PPV_ARGS(&m_d3d11Fence));
	if (FAILED(hr)) {
		OOVR_LOGF("FSR3: OpenSharedFence failed (hr=0x%08X)", hr);
		return false;
	}

	// CPU-side wait event (used for GPU drain in DestroyFsrContexts)
	m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

	OOVR_LOG("FSR3: Cross-API shared fence created");
	return true;
}

// ============================================================================
// Shared texture management
// ============================================================================

bool Fsr3Upscaler::CreateSharedTexture(uint32_t width, uint32_t height, DXGI_FORMAT format,
    bool allowUAV, ID3D12Resource** outDX12, ID3D11Texture2D** outDX11, HANDLE* outHandle)
{
	// Strategy: Create on DX11 side (game's device, guaranteed working), share NT handle to DX12.
	// The reverse direction (DX12 create → DX11 OpenSharedResource1) fails E_INVALIDARG on some
	// driver/device combinations — the game's DX11 device may lack shared resource support.
	D3D11_TEXTURE2D_DESC texDesc = {};
	texDesc.Width = width;
	texDesc.Height = height;
	texDesc.MipLevels = 1;
	texDesc.ArraySize = 1;
	texDesc.Format = format;
	texDesc.SampleDesc.Count = 1;
	texDesc.Usage = D3D11_USAGE_DEFAULT;
	texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
	if (allowUAV)
		texDesc.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
	texDesc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;

	HRESULT hr = m_d3d11Device->CreateTexture2D(&texDesc, nullptr, outDX11);
	if (FAILED(hr)) {
		OOVR_LOGF("FSR3: DX11 CreateTexture2D(%ux%u fmt=%u flags=0x%X) failed (hr=0x%08X)",
		    width, height, format, texDesc.MiscFlags, hr);
		return false;
	}

	// Get NT shared handle from DX11 resource
	IDXGIResource1* dxgiRes = nullptr;
	hr = (*outDX11)->QueryInterface(IID_PPV_ARGS(&dxgiRes));
	if (FAILED(hr)) {
		OOVR_LOGF("FSR3: QI for IDXGIResource1 failed (hr=0x%08X)", hr);
		(*outDX11)->Release(); *outDX11 = nullptr;
		return false;
	}

	hr = dxgiRes->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
	    nullptr, outHandle);
	dxgiRes->Release();
	if (FAILED(hr)) {
		OOVR_LOGF("FSR3: CreateSharedHandle(%ux%u) failed (hr=0x%08X)", width, height, hr);
		(*outDX11)->Release(); *outDX11 = nullptr;
		return false;
	}

	// Open from DX12 side
	hr = m_d3d12Device->OpenSharedHandle(*outHandle, IID_PPV_ARGS(outDX12));
	if (FAILED(hr)) {
		OOVR_LOGF("FSR3: DX12 OpenSharedHandle(%ux%u fmt=%u) failed (hr=0x%08X)",
		    width, height, format, hr);
		CloseHandle(*outHandle); *outHandle = nullptr;
		(*outDX11)->Release(); *outDX11 = nullptr;
		return false;
	}

	{ static int count = 0; if (count++ < 8) {
		D3D12_RESOURCE_DESC dx12Desc = (*outDX12)->GetDesc();
		OOVR_LOGF("FSR3: Shared texture OK (%ux%u fmt=%u uav=%d) DX12 flags=0x%X dim=%u",
		    width, height, format, allowUAV,
		    (unsigned)dx12Desc.Flags, (unsigned)dx12Desc.Dimension);
	}}
	return true;
}

void Fsr3Upscaler::DestroySharedTexture(ID3D12Resource** dx12, ID3D11Texture2D** dx11, HANDLE* handle)
{
	if (*dx11) { (*dx11)->Release(); *dx11 = nullptr; }
	if (handle && *handle) { CloseHandle(*handle); *handle = nullptr; }
	if (*dx12) { (*dx12)->Release(); *dx12 = nullptr; }
}

bool Fsr3Upscaler::EnsureSharedTextures(uint32_t renderW, uint32_t renderH,
    uint32_t outputW, uint32_t outputH, DXGI_FORMAT colorFormat)
{
	// Reuse if dimensions + format unchanged
	if (m_eye[0].colorDX12 && m_renderWidth == renderW && m_renderHeight == renderH
	    && m_outputWidth == outputW && m_outputHeight == outputH
	    && m_colorFormat == colorFormat) {
		return true;
	}

	// Dimensions changed — recreate everything
	DestroyFsrContexts();
	DestroySharedTextures();

	m_renderWidth = renderW;
	m_renderHeight = renderH;
	m_outputWidth = outputW;
	m_outputHeight = outputH;
	m_colorFormat = colorFormat;

	for (int eye = 0; eye < 2; eye++) {
		auto& e = m_eye[eye];

		// Color: render resolution, same format as game texture
		if (!CreateSharedTexture(renderW, renderH, colorFormat, false,
		        &e.colorDX12, &e.colorDX11, &e.colorHandle))
			return false;

		// Motion vectors: render resolution, R16G16_FLOAT
		if (!CreateSharedTexture(renderW, renderH, DXGI_FORMAT_R16G16_FLOAT, false,
		        &e.mvDX12, &e.mvDX11, &e.mvHandle))
			return false;

		// Depth: render resolution, R32_FLOAT (compatible with D32_FLOAT for copy)
		if (!CreateSharedTexture(renderW, renderH, DXGI_FORMAT_R32_FLOAT, false,
		        &e.depthDX12, &e.depthDX11, &e.depthHandle))
			return false;

		// Reactive mask: render resolution, R8_UNORM (depth-edge reactiveness for ghosting reduction)
		if (!CreateSharedTexture(renderW, renderH, DXGI_FORMAT_R8_UNORM, false,
		        &e.reactiveDX12, &e.reactiveDX11, &e.reactiveHandle))
			return false;

		// Output: output resolution, same format as color, with UAV for FSR writes
		// Double-buffered: DX12 writes one while DX11 reads the other (async pipeline)
		for (int buf = 0; buf < 2; buf++) {
			if (!CreateSharedTexture(outputW, outputH, colorFormat, true,
			        &e.outputDX12[buf], &e.outputDX11[buf], &e.outputHandle[buf]))
				return false;
		}
	}

	OOVR_LOGF("FSR3: Shared textures created — render=%ux%u output=%ux%u fmt=%u",
	    renderW, renderH, outputW, outputH, colorFormat);
	return true;
}

void Fsr3Upscaler::DestroySharedTextures()
{
	for (int eye = 0; eye < 2; eye++) {
		auto& e = m_eye[eye];
		DestroySharedTexture(&e.colorDX12, &e.colorDX11, &e.colorHandle);
		DestroySharedTexture(&e.mvDX12, &e.mvDX11, &e.mvHandle);
		DestroySharedTexture(&e.depthDX12, &e.depthDX11, &e.depthHandle);
		DestroySharedTexture(&e.reactiveDX12, &e.reactiveDX11, &e.reactiveHandle);
		for (int buf = 0; buf < 2; buf++)
			DestroySharedTexture(&e.outputDX12[buf], &e.outputDX11[buf], &e.outputHandle[buf]);

		// Reset async pipeline state
		m_outputWrite[eye] = 0;
		m_hasOutput[eye] = false;
		m_eyeFence[eye] = 0;
	}
	m_renderWidth = m_renderHeight = m_outputWidth = m_outputHeight = 0;
	m_colorFormat = DXGI_FORMAT_UNKNOWN;
}

// ============================================================================
// FSR 3 context management
// ============================================================================

bool Fsr3Upscaler::EnsureFsrContexts(uint32_t renderW, uint32_t renderH,
    uint32_t outputW, uint32_t outputH, bool jitterCancellation)
{
	if (m_fsrContextsCreated) return true;

	for (int eye = 0; eye < 2; eye++) {
		// High-level API: chain upscale descriptor + version + DX12 backend descriptor
		ffxCreateContextDescUpscale upscaleDesc = {};
		upscaleDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
		upscaleDesc.flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE
		    | FFX_UPSCALE_ENABLE_DEPTH_INVERTED
		    | FFX_UPSCALE_ENABLE_DEBUG_VISUALIZATION
		    | FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
		if (jitterCancellation)
			upscaleDesc.flags |= FFX_UPSCALE_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION;
		OOVR_LOGF("FSR3: Context creation flags=0x%X (jitterCancel=%s)",
		    upscaleDesc.flags, jitterCancellation ? "ON" : "OFF");
		// Skyrim VR uses reversed-Z depth [1=near, 0=far] — confirmed by
		// ReverbG2OpenXr FSR2 depth dilation (closestDepth = max in 3×3 neighbourhood).
		// Note: NOT setting DEPTH_INFINITE — Skyrim VR uses finite far plane
		upscaleDesc.maxRenderSize = { renderW, renderH };
		upscaleDesc.maxUpscaleSize = { outputW, outputH };
		upscaleDesc.fpMessage = nullptr;

		// Version descriptor — required by FFX SDK v4.0.3 for proper provider initialization
		ffxCreateContextDescUpscaleVersion versionDesc = {};
		versionDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
		versionDesc.version = FFX_UPSCALER_VERSION;

		ffxCreateBackendDX12Desc backendDesc = {};
		backendDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
		backendDesc.device = m_d3d12Device;

		// Chain: upscale → version → backend
		upscaleDesc.header.pNext = &versionDesc.header;
		versionDesc.header.pNext = &backendDesc.header;

		ffxReturnCode_t rc = m_ffxCreateContext(&m_fsrContext[eye], &upscaleDesc.header, nullptr);
		if (rc != 0) {
			OOVR_LOGF("FSR3: ffxCreateContext eye=%d failed (rc=%u)", eye, rc);
			DestroyFsrContexts();
			return false;
		}
	}

	m_fsrContextsCreated = true;
	OOVR_LOGF("FSR3: Upscaler contexts created — render=%ux%u output=%ux%u", renderW, renderH, outputW, outputH);

	return true;
}

bool Fsr3Upscaler::EnsureWarpFsrContexts(uint32_t renderW, uint32_t renderH,
    uint32_t outputW, uint32_t outputH)
{
	if (m_warpFsrContextsCreated) return true;

	for (int eye = 0; eye < 2; eye++) {
		ffxCreateContextDescUpscale upscaleDesc = {};
		upscaleDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
		upscaleDesc.flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE
		    | FFX_UPSCALE_ENABLE_DEPTH_INVERTED
		    | FFX_UPSCALE_ENABLE_DEBUG_VISUALIZATION
		    | FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
		OOVR_LOGF("FSR3 warp: Context creation flags=0x%X", upscaleDesc.flags);

		upscaleDesc.maxRenderSize = { renderW, renderH };
		upscaleDesc.maxUpscaleSize = { outputW, outputH };
		upscaleDesc.fpMessage = nullptr;

		ffxCreateContextDescUpscaleVersion versionDesc = {};
		versionDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
		versionDesc.version = FFX_UPSCALER_VERSION;

		ffxCreateBackendDX12Desc backendDesc = {};
		backendDesc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
		backendDesc.device = m_d3d12Device;

		upscaleDesc.header.pNext = &versionDesc.header;
		versionDesc.header.pNext = &backendDesc.header;

		ffxReturnCode_t rc = m_ffxCreateContext(&m_warpFsrContext[eye], &upscaleDesc.header, nullptr);
		if (rc != 0) {
			OOVR_LOGF("FSR3 warp: ffxCreateContext eye=%d failed (rc=%u)", eye, rc);
			for (int e = 0; e <= eye; e++) {
				if (m_warpFsrContext[e] && m_ffxDestroyContext) {
					m_ffxDestroyContext(&m_warpFsrContext[e], nullptr);
					m_warpFsrContext[e] = nullptr;
				}
			}
			return false;
		}
	}

	m_warpFsrContextsCreated = true;
	OOVR_LOGF("FSR3 warp: Upscaler contexts created - render=%ux%u output=%ux%u",
	    renderW, renderH, outputW, outputH);
	return true;
}

void Fsr3Upscaler::DestroyFsrContexts()
{
	if (!m_fsrContextsCreated && !m_warpFsrContextsCreated) return;

	// GPU must be idle before destroying contexts
	if (m_cmdQueue && m_d3d12Fence && m_fenceEvent) {
		m_cmdQueue->Signal(m_d3d12Fence, ++m_fenceValue);
		m_d3d12Fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent);
		WaitForSingleObject(m_fenceEvent, 5000);
	}

	for (int eye = 0; eye < 2; eye++) {
		if (m_fsrContext[eye] && m_ffxDestroyContext) {
			m_ffxDestroyContext(&m_fsrContext[eye], nullptr);
			m_fsrContext[eye] = nullptr;
		}
		if (m_warpFsrContext[eye] && m_ffxDestroyContext) {
			m_ffxDestroyContext(&m_warpFsrContext[eye], nullptr);
			m_warpFsrContext[eye] = nullptr;
		}
	}

	m_fsrContextsCreated = false;
	m_warpFsrContextsCreated = false;
	m_fsrConfigApplied = false;
}

// ============================================================================
// Dispatch — the main per-eye upscaling call
// ============================================================================

bool Fsr3Upscaler::Dispatch(int eyeIdx, ID3D11DeviceContext* d3d11Ctx, const DispatchParams& params)
{
	return DispatchInternal(eyeIdx, d3d11Ctx, params, false);
}

bool Fsr3Upscaler::DispatchWarp(int eyeIdx, ID3D11DeviceContext* d3d11Ctx, const DispatchParams& params)
{
	return DispatchInternal(eyeIdx, d3d11Ctx, params, true);
}

bool Fsr3Upscaler::DispatchInternal(int eyeIdx, ID3D11DeviceContext* d3d11Ctx,
    const DispatchParams& params, bool warpContext)
{
	if (!m_ready || eyeIdx < 0 || eyeIdx > 1 || !d3d11Ctx) return false;

	// Get color format from the game texture
	D3D11_TEXTURE2D_DESC colorDesc;
	params.color->GetDesc(&colorDesc);
	DXGI_FORMAT colorFormat = colorDesc.Format;

	// Lazy-create shared textures and FSR contexts on first frame or resolution change
	if (!EnsureSharedTextures(params.renderWidth, params.renderHeight,
	        params.outputWidth, params.outputHeight, colorFormat))
		return false;

	if (warpContext) {
		if (!EnsureWarpFsrContexts(params.renderWidth, params.renderHeight,
		        params.outputWidth, params.outputHeight))
			return false;
	} else {
		if (!EnsureFsrContexts(params.renderWidth, params.renderHeight,
		        params.outputWidth, params.outputHeight, params.jitterCancellation))
			return false;
	}

	auto& eye = m_eye[eyeIdx];
	ffxContext* contexts = warpContext ? m_warpFsrContext : m_fsrContext;

	// ── Synchronous wait: ensure previous DX12 work for this eye is complete ──
	// Must complete before resetting command allocator or submitting new work.
	if (m_eyeFence[eyeIdx] > 0) {
		uint64_t completed = m_d3d12Fence->GetCompletedValue();
		if (completed < m_eyeFence[eyeIdx]) {
			// DX12 still processing — CPU-wait for completion
			m_d3d12Fence->SetEventOnCompletion(m_eyeFence[eyeIdx], m_fenceEvent);
			WaitForSingleObject(m_fenceEvent, 5000);
		}
	}

	// Previous DX12 work is complete (or this is the first frame).
	// Determine which buffer DX12 will write to, and which has the completed output.
	int writeBuf = m_outputWrite[eyeIdx];
	bool hasValidOutput = m_hasOutput[eyeIdx]; // true after first DX12 dispatch completes

	// ── Step 1: Tell DX11 GPU that DX12 is done with the read buffer ──
	// This ensures DX11 pipeline ordering: the caller's read of outputDX11[readBuf]
	// (which happens AFTER Dispatch returns) will see DX12's completed writes.
	ID3D11DeviceContext4* ctx4 = nullptr;
	HRESULT hr = d3d11Ctx->QueryInterface(IID_PPV_ARGS(&ctx4));
	if (FAILED(hr)) {
		OOVR_LOGF("FSR3: QueryInterface(ID3D11DeviceContext4) failed (hr=0x%08X)", hr);
		return false;
	}

	if (m_eyeFence[eyeIdx] > 0) {
		// DX11 GPU waits for previous DX12 completion — should be instant since CPU check passed
		ctx4->Wait(m_d3d11Fence, m_eyeFence[eyeIdx]);
	}

	// ── Step 2: DX11 copies input data → shared staging textures ──

	// Color: per-eye sub-region copy (stereo-combined) or full copy
	if (params.colorSourceRegion) {
		d3d11Ctx->CopySubresourceRegion(eye.colorDX11, 0, 0, 0, 0,
		    params.color, 0, params.colorSourceRegion);
	} else {
		d3d11Ctx->CopyResource(eye.colorDX11, params.color);
	}

	// Motion vectors: sub-region copy for per-eye extraction from stereo-combined texture
	if (params.motionVectors) {
		if (params.mvSourceRegion) {
			d3d11Ctx->CopySubresourceRegion(eye.mvDX11, 0, 0, 0, 0,
			    params.motionVectors, 0, params.mvSourceRegion);
		} else {
			d3d11Ctx->CopyResource(eye.mvDX11, params.motionVectors);
		}
	}

	// Depth: copy R32_FLOAT source → R32_FLOAT shared staging
	// NOTE: Caller (dx11compositor) must pre-convert D24/R24G8 depth to R32F
	// via the DepthExtract compute shader — CopySubresourceRegion requires
	// format-compatible textures (R24G8 → R32F is NOT copy-compatible).
	if (params.depth) {
		if (params.depthSourceRegion) {
			d3d11Ctx->CopySubresourceRegion(eye.depthDX11, 0, 0, 0, 0,
			    params.depth, 0, params.depthSourceRegion);
		} else {
			d3d11Ctx->CopySubresourceRegion(eye.depthDX11, 0, 0, 0, 0,
			    params.depth, 0, nullptr);
		}
	}

	// Reactive mask: copy R8_UNORM source → R8_UNORM shared staging (same sub-region as depth)
	if (params.reactiveMask) {
		if (params.reactiveSourceRegion) {
			d3d11Ctx->CopySubresourceRegion(eye.reactiveDX11, 0, 0, 0, 0,
			    params.reactiveMask, 0, params.reactiveSourceRegion);
		} else {
			d3d11Ctx->CopySubresourceRegion(eye.reactiveDX11, 0, 0, 0, 0,
			    params.reactiveMask, 0, nullptr);
		}
	}

	// ── Step 3: DX11 signals fence → DX12 can read inputs ──

	ctx4->Signal(m_d3d11Fence, ++m_fenceValue);
	uint64_t dx11DoneValue = m_fenceValue;

	// ── Step 4: DX12 waits for DX11 inputs, dispatches FSR 3 (fire-and-forget) ──

	m_cmdQueue->Wait(m_d3d12Fence, dx11DoneValue);

	// Reset command allocator and list for this eye (safe: previous work is complete)
	m_cmdAlloc[eyeIdx]->Reset();
	m_cmdList[eyeIdx]->Reset(m_cmdAlloc[eyeIdx], nullptr);
	auto cmdList = m_cmdList[eyeIdx];

	// Apply runtime tuning once per context lifetime (reduces ghosting on thin geometry)
	if (!warpContext && !m_fsrConfigApplied && m_ffxConfigure) {
		m_fsrConfigApplied = true;
		struct { uint64_t key; float value; const char* name; } cfgEntries[] = {
			{ FFX_API_CONFIGURE_UPSCALE_KEY_FSHADINGCHANGESCALE,
			  oovr_global_configuration.Fsr3ShadingChangeScale(), "fShadingChangeScale" },
			{ FFX_API_CONFIGURE_UPSCALE_KEY_FREACTIVENESSSCALE,
			  oovr_global_configuration.Fsr3ReactivenessScale(), "fReactivenessScale" },
			{ FFX_API_CONFIGURE_UPSCALE_KEY_FACCUMULATIONADDEDPERFRAME,
			  oovr_global_configuration.Fsr3AccumulationPerFrame(), "fAccumulationAddedPerFrame" },
			{ FFX_API_CONFIGURE_UPSCALE_KEY_FMINDISOCCLUSIONACCUMULATION,
			  oovr_global_configuration.Fsr3MinDisocclusionAccumulation(), "fMinDisocclusionAccumulation" },
			{ FFX_API_CONFIGURE_UPSCALE_KEY_FVELOCITYFACTOR,
			  oovr_global_configuration.Fsr3VelocityFactor(), "fVelocityFactor" },
		};
		for (auto& entry : cfgEntries) {
			ffxConfigureDescUpscaleKeyValue cfg = {};
			cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_UPSCALE_KEYVALUE;
			cfg.key = entry.key;
			cfg.ptr = &entry.value;
			for (int e = 0; e < 2; e++) {
				ffxReturnCode_t rc = m_ffxConfigure(&m_fsrContext[e], &cfg.header);
				if (rc != 0) {
					OOVR_LOGF("FSR3: ffxConfigure %s eye=%d failed (rc=%d)", entry.name, e, (int)rc);
				}
			}
			OOVR_LOGF("FSR3: Configured %s=%.3f", entry.name, entry.value);
		}
	}

	// Build FSR 3 dispatch descriptor (high-level API)
	ffxDispatchDescUpscale dispatchDesc = {};
	dispatchDesc.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
	dispatchDesc.commandList = cmdList;

	// Input resources — shared textures with implicit state promotion (ALLOW_SIMULTANEOUS_ACCESS)
	dispatchDesc.color = ffxApiGetResourceDX12(
	    eye.colorDX12, FFX_API_RESOURCE_STATE_COMPUTE_READ);
	dispatchDesc.depth = ffxApiGetResourceDX12(
	    eye.depthDX12, FFX_API_RESOURCE_STATE_COMPUTE_READ);
	dispatchDesc.depth.description.usage = FFX_API_RESOURCE_USAGE_DEPTHTARGET;
	dispatchDesc.motionVectors = ffxApiGetResourceDX12(
	    eye.mvDX12, FFX_API_RESOURCE_STATE_COMPUTE_READ);

	// Reactive mask: depth-edge detection (reduces ghosting on thin geometry like trees)
	if (params.reactiveMask) {
		dispatchDesc.reactive = ffxApiGetResourceDX12(
		    eye.reactiveDX12, FFX_API_RESOURCE_STATE_COMPUTE_READ);
		// Also pass as transparencyAndComposition — this gives FSR3 an additional
		// signal for disocclusion handling at alpha-tested/object edges against sky.
		dispatchDesc.transparencyAndComposition = dispatchDesc.reactive;
	}

	// Output resource — write to the current write buffer (double-buffered)
	dispatchDesc.output = ffxApiGetResourceDX12(
	    eye.outputDX12[writeBuf], FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, FFX_API_RESOURCE_USAGE_UAV);

	// Jitter and motion vector parameters
	dispatchDesc.jitterOffset = { params.jitterX, params.jitterY };
	// Skyrim's kMOTION_VECTOR RT stores screen-space velocity in UV-space [0..1].
	// Scale converts UV→pixel: motionVectorScale = {perEyeRenderW, perEyeRenderH}.
	// Note: The FSR2 reference project negates MVs in its custom accumulate shader,
	// but it uses a custom FSR2 implementation. Testing confirmed that negating MVs
	// with AMD's FSR3 SDK causes full-screen smearing when turning — the SDK's
	// convention expects positive scale for Skyrim's MV format.
	float mvScale = params.mvScale;
	dispatchDesc.motionVectorScale = { (float)params.renderWidth * mvScale, (float)params.renderHeight * mvScale };

	dispatchDesc.renderSize = { params.renderWidth, params.renderHeight };
	dispatchDesc.upscaleSize = { params.outputWidth, params.outputHeight };
	dispatchDesc.frameTimeDelta = params.deltaTimeMs;
	dispatchDesc.cameraNear = params.cameraNear;
	dispatchDesc.cameraFar = params.cameraFar;
	dispatchDesc.cameraFovAngleVertical = params.cameraFovY;
	dispatchDesc.reset = params.reset;
	dispatchDesc.preExposure = 1.0f;
	dispatchDesc.enableSharpening = (params.sharpness > 0.0f);
	dispatchDesc.sharpness = params.sharpness;
	dispatchDesc.viewSpaceToMetersFactor = params.viewToMeters;
	// IMPORTANT: dispatch flags use FFX_UPSCALE_FLAG_* constants, NOT creation-time
	// FFX_UPSCALE_ENABLE_* constants! These share the same bit positions but mean
	// different things. Jitter cancellation is a creation-time-only flag (already set
	// in EnsureFsrContexts). Setting it here would accidentally enable
	// FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_PQ (also bit 2), causing FSR3 to treat
	// SDR color as HDR PQ and breaking temporal accumulation.
	dispatchDesc.flags = (params.debugMode == 1 ? FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW : 0)
	    | FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;


	// DIAG: log the actual dispatch params once per ~30 stereo frames per eye
	{
		static int s_fsr3DispDiag[2] = { 0, 0 };
		s_fsr3DispDiag[eyeIdx]++;
		if (s_fsr3DispDiag[eyeIdx] % 30 == 0) {
			OOVR_LOGF("FSR3-DISPATCH: eye=%d frame=%d jitter=(%.4f,%.4f) mvScale=(%.3f,%.3f) "
			          "render=%ux%u upscale=%ux%u dt=%.3fms sharpening=%d sharp=%.3f reset=%d",
			    eyeIdx, s_fsr3DispDiag[eyeIdx],
			    dispatchDesc.jitterOffset.x, dispatchDesc.jitterOffset.y,
			    dispatchDesc.motionVectorScale.x, dispatchDesc.motionVectorScale.y,
			    dispatchDesc.renderSize.width, dispatchDesc.renderSize.height,
			    dispatchDesc.upscaleSize.width, dispatchDesc.upscaleSize.height,
			    dispatchDesc.frameTimeDelta,
			    dispatchDesc.enableSharpening ? 1 : 0, dispatchDesc.sharpness,
			    dispatchDesc.reset ? 1 : 0);
		}
	}

	// Record FSR 3 commands onto our command list
	ffxReturnCode_t rc = m_ffxDispatch(&contexts[eyeIdx], &dispatchDesc.header);
	if (rc != 0) {
		OOVR_LOGF("%s: ffxDispatch eye=%d failed (rc=%u)",
		    warpContext ? "FSR3 warp" : "FSR3", eyeIdx, rc);
		cmdList->Close();
		ctx4->Release();
		return false;
	}

	// Close and execute
	cmdList->Close();
	ID3D12CommandList* cmdLists[] = { cmdList };
	m_cmdQueue->ExecuteCommandLists(1, cmdLists);

	// ── Step 5: SYNCHRONOUS wait — DX12 must finish before DX11 reads output ──
	// (Temporary for debugging: eliminates 1-frame delay to test visual correctness)

	m_cmdQueue->Signal(m_d3d12Fence, ++m_fenceValue);
	m_eyeFence[eyeIdx] = m_fenceValue;

	// DX11 GPU waits for DX12 completion (synchronous — ensures output is ready NOW)
	ctx4->Wait(m_d3d11Fence, m_fenceValue);

	ctx4->Release();

	// Output is in outputDX12[writeBuf] / outputDX11[writeBuf] — ready to read immediately
	// (No double-buffering for now: always read what we just wrote)
	m_outputWrite[eyeIdx] = writeBuf; // Keep pointing to current buffer for GetOutputDX11
	m_hasOutput[eyeIdx] = true;
	return true;
}

ID3D11Texture2D* Fsr3Upscaler::GetOutputDX11(int eyeIdx) const
{
	if (eyeIdx < 0 || eyeIdx > 1) return nullptr;
	// Synchronous mode: return the buffer we just wrote to (DX12 already completed)
	return m_eye[eyeIdx].outputDX11[m_outputWrite[eyeIdx]];
}

#endif // defined(SUPPORT_DX) && defined(SUPPORT_DX11) && defined(OC_HAS_FSR3)
