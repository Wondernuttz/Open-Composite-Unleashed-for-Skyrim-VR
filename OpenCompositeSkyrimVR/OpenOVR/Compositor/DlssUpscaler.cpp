#include "stdafx.h"

#if defined(SUPPORT_DX) && defined(SUPPORT_DX11) && defined(OC_HAS_DLSS)

#include "DlssUpscaler.h"
#include "../Misc/Config.h"
#include "../logging.h"

#include <cmath>
#include <algorithm>
#include <string>

// ============================================================================
// Jitter utilities (Halton[2,3] — identical to Fsr3Upscaler implementation)
// ============================================================================

static float DlssHalton(int index, int base)
{
	float f = 1.0f, r = 0.0f;
	while (index > 0) {
		f /= static_cast<float>(base);
		r += f * static_cast<float>(index % base);
		index /= base;
	}
	return r;
}

void DlssUpscaler::GetJitterOffset(float* outX, float* outY, int frameIndex, int phaseCount)
{
	int idx = (frameIndex % phaseCount) + 1; // 1-based for Halton
	*outX = DlssHalton(idx, 2) - 0.5f;
	*outY = DlssHalton(idx, 3) - 0.5f;
}

int DlssUpscaler::GetJitterPhaseCount(uint32_t renderWidth, uint32_t displayWidth)
{
	float ratio = static_cast<float>(displayWidth) / static_cast<float>(renderWidth);
	return std::max(1, static_cast<int>(8.0f * ratio * ratio + 0.5f));
}

// ============================================================================
// Lifecycle
// ============================================================================

// Get the directory containing our DLL (openvr_api.dll / vrclient_x64.dll).
// NGX needs this to find nvngx_dlss.dll when deployed alongside our DLL
// rather than alongside the game executable.
static std::wstring GetOurDllDirectory()
{
	HMODULE hMod = nullptr;
	// Get handle to THIS DLL (not the exe) using the address of this function
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	    reinterpret_cast<LPCWSTR>(&GetOurDllDirectory), &hMod);
	if (!hMod) return L".";
	wchar_t path[MAX_PATH] = {};
	GetModuleFileNameW(hMod, path, MAX_PATH);
	// Strip filename, keep directory
	std::wstring dir(path);
	auto pos = dir.find_last_of(L"\\/");
	if (pos != std::wstring::npos) dir.resize(pos);
	return dir;
}

bool DlssUpscaler::Initialize(ID3D11Device* d3d11Device)
{
	m_device = d3d11Device;

	// Resolve directory containing our DLL — nvngx_dlss.dll should be deployed here.
	std::wstring dllDir = GetOurDllDirectory();

	// Tell NGX to also search our DLL directory for feature DLLs (nvngx_dlss.dll).
	// Without this, NGX only checks the game executable's directory and system paths.
	const wchar_t* featurePaths[] = { dllDir.c_str() };
	NVSDK_NGX_FeatureCommonInfo featureInfo = {};
	featureInfo.PathListInfo.Path = featurePaths;
	featureInfo.PathListInfo.Length = 1;

	OOVR_LOGF("DLSS: Initializing NGX — DLL dir: %ls", dllDir.c_str());

	// Use Init_with_ProjectID for custom engine integration. A valid GUID-format
	// project ID is required — the NGX core validates the format. AppId=0 with plain
	// Init only works for NVIDIA-registered titles.
	NVSDK_NGX_Result result = NVSDK_NGX_D3D11_Init_with_ProjectID(
	    "a0f57b54-1daf-4934-90ae-c4035c19df04",  // Open Composite Unleashed project GUID
	    NVSDK_NGX_ENGINE_TYPE_CUSTOM,
	    "1.0.0",
	    dllDir.c_str(),
	    d3d11Device,
	    &featureInfo);
	if (NVSDK_NGX_FAILED(result)) {
		OOVR_LOGF("DLSS: NGX Init failed (0x%08X) — no NVIDIA GPU or driver too old", (unsigned)result);
		return false;
	}

	// Allocate the capability/parameter block.
	result = NVSDK_NGX_D3D11_GetCapabilityParameters(&m_params);
	if (NVSDK_NGX_FAILED(result)) {
		OOVR_LOGF("DLSS: GetCapabilityParameters failed (0x%08X)", (unsigned)result);
		NVSDK_NGX_D3D11_Shutdown1(m_device);
		return false;
	}

	// Log additional diagnostic info
	unsigned int needsUpdatedDriver = 0;
	m_params->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsUpdatedDriver);
	unsigned int minDriverMajor = 0, minDriverMinor = 0;
	m_params->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &minDriverMajor);
	m_params->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minDriverMinor);
	OOVR_LOGF("DLSS: NeedsUpdatedDriver=%u MinDriver=%u.%u", needsUpdatedDriver, minDriverMajor, minDriverMinor);

	// Confirm DLSS Super Sampling is supported on this GPU/driver.
	int dlssAvailable = 0;
	result = m_params->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &dlssAvailable);
	if (NVSDK_NGX_FAILED(result) || !dlssAvailable) {
		// Log feature discovery failure details to help diagnose missing nvngx_dlss.dll
		unsigned int featureSupported = 0;
		m_params->Get(NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &featureSupported);
		OOVR_LOGF("DLSS: SuperSampling not available (result=0x%08X available=%d featureInit=0x%08X)",
		    (unsigned)result, dlssAvailable, featureSupported);
		OOVR_LOGF("DLSS: Ensure nvngx_dlss.dll is deployed alongside openvr_api.dll at: %ls", dllDir.c_str());
		NVSDK_NGX_D3D11_DestroyParameters(m_params);
		m_params = nullptr;
		NVSDK_NGX_D3D11_Shutdown1(m_device);
		return false;
	}

	OOVR_LOG("DLSS: Initialization OK — DLSS Super Resolution ready");
	m_ready = true;
	return true;
}

void DlssUpscaler::Shutdown()
{
	if (!m_device) return;
	m_ready = false;
	// Destroy features — need a context; use immediate context
	ID3D11DeviceContext* ctx = nullptr;
	m_device->GetImmediateContext(&ctx);
	if (ctx) {
		DestroyFeature(0, ctx);
		DestroyFeature(1, ctx);
		ctx->Release();
	}
	DestroyStagingTextures();
	DestroyOutputTexture(0);
	DestroyOutputTexture(1);
	if (m_params) {
		NVSDK_NGX_D3D11_DestroyParameters(m_params);
		m_params = nullptr;
	}
	NVSDK_NGX_D3D11_Shutdown1(m_device);
	m_device = nullptr;
}

DlssUpscaler::~DlssUpscaler()
{
	Shutdown();
}

// ============================================================================
// Feature management
// ============================================================================

bool DlssUpscaler::EnsureOutputTexture(int eyeIdx, uint32_t outputW, uint32_t outputH)
{
	if (m_output[eyeIdx] &&
	    m_outputW[eyeIdx] == outputW && m_outputH[eyeIdx] == outputH)
		return true;

	DestroyOutputTexture(eyeIdx);

	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width            = outputW;
	desc.Height           = outputH;
	desc.MipLevels        = 1;
	desc.ArraySize        = 1;
	desc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage            = D3D11_USAGE_DEFAULT;
	desc.BindFlags        = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

	HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, &m_output[eyeIdx]);
	if (FAILED(hr)) {
		OOVR_LOGF("DLSS: Failed to create output texture for eye %d (hr=0x%08X)", eyeIdx, (unsigned)hr);
		return false;
	}
	m_outputW[eyeIdx] = outputW;
	m_outputH[eyeIdx] = outputH;
	return true;
}

void DlssUpscaler::DestroyOutputTexture(int eyeIdx)
{
	if (m_output[eyeIdx]) { m_output[eyeIdx]->Release(); m_output[eyeIdx] = nullptr; }
	m_outputW[eyeIdx] = 0;
	m_outputH[eyeIdx] = 0;
}

bool DlssUpscaler::EnsureStagingTextures(uint32_t renderW, uint32_t renderH,
    ID3D11Texture2D* colorSrc, ID3D11Texture2D* mvSrc, ID3D11Texture2D* depthSrc)
{
	// Query source formats
	D3D11_TEXTURE2D_DESC colorDesc = {}, mvDesc = {}, depthDesc = {};
	if (colorSrc) colorSrc->GetDesc(&colorDesc);
	if (mvSrc)    mvSrc->GetDesc(&mvDesc);
	if (depthSrc) depthSrc->GetDesc(&depthDesc);

	DXGI_FORMAT newColorFmt = colorSrc ? colorDesc.Format : DXGI_FORMAT_R8G8B8A8_UNORM;
	DXGI_FORMAT newMVFmt    = mvSrc    ? mvDesc.Format    : DXGI_FORMAT_R16G16_FLOAT;
	DXGI_FORMAT newDepthFmt = depthSrc ? depthDesc.Format : DXGI_FORMAT_R32_FLOAT;

	bool sizeMatch  = (m_stagingW == renderW && m_stagingH == renderH);
	bool colorMatch = (m_stagingColorFmt == newColorFmt);
	bool mvMatch    = (m_stagingMVFmt    == newMVFmt);
	bool depthMatch = (m_stagingDepthFmt == newDepthFmt);

	if (sizeMatch && colorMatch && mvMatch && depthMatch
	    && m_stagingColor && m_stagingMV && (!depthSrc || m_stagingDepth))
		return true; // All up-to-date

	DestroyStagingTextures();

	D3D11_TEXTURE2D_DESC sd = {};
	sd.Width            = renderW;
	sd.Height           = renderH;
	sd.MipLevels        = 1;
	sd.ArraySize        = 1;
	sd.SampleDesc.Count = 1;
	sd.Usage            = D3D11_USAGE_DEFAULT;
	sd.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

	// Color staging
	if (colorSrc) {
		sd.Format = newColorFmt;
		if (FAILED(m_device->CreateTexture2D(&sd, nullptr, &m_stagingColor))) {
			OOVR_LOG("DLSS: Failed to create color staging texture");
			return false;
		}
	}
	// MV staging
	if (mvSrc) {
		sd.Format = newMVFmt;
		if (FAILED(m_device->CreateTexture2D(&sd, nullptr, &m_stagingMV))) {
			OOVR_LOG("DLSS: Failed to create MV staging texture");
			return false;
		}
	}
	// Depth staging
	if (depthSrc) {
		sd.Format = newDepthFmt;
		if (FAILED(m_device->CreateTexture2D(&sd, nullptr, &m_stagingDepth))) {
			OOVR_LOG("DLSS: Failed to create depth staging texture");
			return false;
		}
	}

	m_stagingW         = renderW;
	m_stagingH         = renderH;
	m_stagingColorFmt  = newColorFmt;
	m_stagingMVFmt     = newMVFmt;
	m_stagingDepthFmt  = newDepthFmt;
	return true;
}

void DlssUpscaler::DestroyStagingTextures()
{
	if (m_stagingColor) { m_stagingColor->Release(); m_stagingColor = nullptr; }
	if (m_stagingMV)    { m_stagingMV->Release();    m_stagingMV    = nullptr; }
	if (m_stagingDepth) { m_stagingDepth->Release(); m_stagingDepth = nullptr; }
	m_stagingW = 0; m_stagingH = 0;
	m_stagingColorFmt = m_stagingMVFmt = m_stagingDepthFmt = DXGI_FORMAT_UNKNOWN;
}

bool DlssUpscaler::EnsureFeature(int eyeIdx, ID3D11DeviceContext* ctx,
    uint32_t renderW, uint32_t renderH, uint32_t outputW, uint32_t outputH)
{
	if (m_handle[eyeIdx] &&
	    m_renderW[eyeIdx] == renderW && m_renderH[eyeIdx] == renderH &&
	    m_outputW[eyeIdx] == outputW && m_outputH[eyeIdx] == outputH)
		return true;

	if (m_handle[eyeIdx])
		DestroyFeature(eyeIdx, ctx);

	if (!EnsureOutputTexture(eyeIdx, outputW, outputH))
		return false;

	// Map config preset to NGX quality value
	// 0=Quality(67%) 1=Balanced(58%) 2=Performance(50%) 3=UltraPerf(33%)
	static const NVSDK_NGX_PerfQuality_Value presetMap[] = {
		NVSDK_NGX_PerfQuality_Value_MaxQuality,       // 0 = Quality
		NVSDK_NGX_PerfQuality_Value_Balanced,          // 1 = Balanced
		NVSDK_NGX_PerfQuality_Value_MaxPerf,           // 2 = Performance
		NVSDK_NGX_PerfQuality_Value_UltraPerformance,  // 3 = UltraPerf
	};
	int presetIdx = std::max(0, std::min(3, oovr_global_configuration.DlssPreset()));
	NVSDK_NGX_PerfQuality_Value perfQuality = presetMap[presetIdx];

	// Set creation parameters
	NVSDK_NGX_DLSS_Create_Params createParams = {};
	createParams.Feature.InWidth      = renderW;
	createParams.Feature.InHeight     = renderH;
	createParams.Feature.InTargetWidth  = outputW;
	createParams.Feature.InTargetHeight = outputH;
	createParams.Feature.InPerfQualityValue = perfQuality;
	createParams.InFeatureCreateFlags =
		NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
		NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |   // MVs at render res (not display res)
		NVSDK_NGX_DLSS_Feature_Flags_DepthInverted; // Skyrim uses reversed-Z depth

	NVSDK_NGX_Result result = NGX_D3D11_CREATE_DLSS_EXT(ctx, &m_handle[eyeIdx], m_params, &createParams);
	if (NVSDK_NGX_FAILED(result)) {
		OOVR_LOGF("DLSS: CreateFeature failed for eye %d (0x%08X) render=%ux%u output=%ux%u",
		    eyeIdx, (unsigned)result, renderW, renderH, outputW, outputH);
		m_handle[eyeIdx] = nullptr;
		return false;
	}

	m_renderW[eyeIdx] = renderW;  m_renderH[eyeIdx] = renderH;
	OOVR_LOGF("DLSS: Feature created eye=%d render=%ux%u output=%ux%u preset=%d",
	    eyeIdx, renderW, renderH, outputW, outputH, presetIdx);
	return true;
}

void DlssUpscaler::DestroyFeature(int eyeIdx, ID3D11DeviceContext* ctx)
{
	if (!m_handle[eyeIdx]) return;
	NVSDK_NGX_D3D11_ReleaseFeature(m_handle[eyeIdx]);
	m_handle[eyeIdx] = nullptr;
	m_renderW[eyeIdx] = 0; m_renderH[eyeIdx] = 0;
}

// ============================================================================
// Dispatch
// ============================================================================

bool DlssUpscaler::Dispatch(int eyeIdx, ID3D11DeviceContext* ctx, const DispatchParams& params)
{
	if (!m_ready || !m_device || !ctx) return false;

	// Debug bypass: skip DLSS, return false so caller uses fallback
	if (params.debugMode == 2) return false;

	if (!EnsureFeature(eyeIdx, ctx, params.renderWidth, params.renderHeight,
	                   params.outputWidth, params.outputHeight))
		return false;

	// Resolve sub-regions: if inputs are stereo-combined, copy per-eye region to staging
	ID3D11Texture2D* colorIn = params.color;
	ID3D11Texture2D* mvIn    = params.motionVectors;
	ID3D11Texture2D* depthIn = params.depth;

	if (params.colorSourceRegion || params.mvSourceRegion || params.depthSourceRegion) {
		// Need staging textures for sub-region copies
		if (!EnsureStagingTextures(params.renderWidth, params.renderHeight,
		    params.color, params.motionVectors, params.depth))
			return false;

		if (params.color && params.colorSourceRegion) {
			ctx->CopySubresourceRegion(m_stagingColor, 0, 0, 0, 0,
			    params.color, 0, params.colorSourceRegion);
			colorIn = m_stagingColor;
		}
		if (params.motionVectors && params.mvSourceRegion) {
			ctx->CopySubresourceRegion(m_stagingMV, 0, 0, 0, 0,
			    params.motionVectors, 0, params.mvSourceRegion);
			mvIn = m_stagingMV;
		}
		if (params.depth && params.depthSourceRegion) {
			ctx->CopySubresourceRegion(m_stagingDepth, 0, 0, 0, 0,
			    params.depth, 0, params.depthSourceRegion);
			depthIn = m_stagingDepth;
		}
	}

	// Build DLSS evaluate parameters and dispatch
	NVSDK_NGX_D3D11_DLSS_Eval_Params evalParams = {};
	evalParams.Feature.pInColor         = colorIn;
	evalParams.Feature.pInOutput        = m_output[eyeIdx];
	evalParams.InJitterOffsetX          = params.jitterX;
	evalParams.InJitterOffsetY          = params.jitterY;
	evalParams.InReset                  = params.reset ? 1 : 0;
	evalParams.pInDepth                 = depthIn;
	evalParams.pInMotionVectors         = mvIn;
	evalParams.InFrameTimeDeltaInMsec   = std::max(1.0f, params.deltaTimeMs);
	evalParams.InMVScaleX               = params.mvScale;
	evalParams.InMVScaleY               = params.mvScale;
	evalParams.InPreExposure            = 1.0f;
	evalParams.InExposureScale          = 1.0f;
	// Sharpness: DLSS 3.1+ uses a separate RCAS pass; 0.0 = no sharpening
	evalParams.Feature.InSharpness      = params.sharpness;

	NVSDK_NGX_Result result = NGX_D3D11_EVALUATE_DLSS_EXT(ctx, m_handle[eyeIdx], m_params, &evalParams);
	if (NVSDK_NGX_FAILED(result)) {
		static int failCount = 0;
		if (failCount++ < 5)
			OOVR_LOGF("DLSS: EvaluateFeature failed eye=%d (0x%08X)", eyeIdx, (unsigned)result);
		return false;
	}

	{ static bool s = false; if (!s) { s = true;
		OOVR_LOGF("DLSS: First dispatch — render=%ux%u output=%ux%u jitter=%.3f,%.3f",
		    params.renderWidth, params.renderHeight,
		    params.outputWidth, params.outputHeight,
		    params.jitterX, params.jitterY);
	}}
	return true;
}

ID3D11Texture2D* DlssUpscaler::GetOutputDX11(int eyeIdx) const
{
	return m_output[eyeIdx];
}

#endif // OC_HAS_DLSS
