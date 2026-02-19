#include "stdafx.h"

#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)

#include "dx11compositor.h"
#include "../Reimpl/BaseCompositor.h"

#include "../Misc/Config.h"
#include "../Misc/xr_ext.h"

#include <d3dcompiler.h> // For compiling shaders! D3DCompile
#include <cmath>
#include <string>

#pragma comment(lib, "d3dcompiler.lib")

// AMD FidelityFX FSR 1.0 — CPU-side constant setup functions (FsrEasuCon, FsrRcasCon)
#define A_CPU
#include "fsr/ffx_a.h"
#include "fsr/ffx_fsr1.h"
#undef A_CPU

// Shader HLSL headers are embedded as Win32 resources (avoids MSVC string literal size limits)
#include "../resources.h"

EXTERN_C IMAGE_DOS_HEADER __ImageBase;
#define HINST_THISCOMPONENT ((HINSTANCE)&__ImageBase)

static std::string LoadHLSLResource(int resourceId)
{
	HRSRC hRes = FindResource(HINST_THISCOMPONENT, MAKEINTRESOURCE(resourceId), MAKEINTRESOURCE(RES_T_HLSL));
	if (!hRes) return "";
	HGLOBAL hData = LoadResource(HINST_THISCOMPONENT, hRes);
	if (!hData) return "";
	DWORD size = SizeofResource(HINST_THISCOMPONENT, hRes);
	const char* data = static_cast<const char*>(LockResource(hData));
	return std::string(data, size);
}

constexpr char fs_shader_code[] = R"_(
Texture2D shaderTexture : register(t0);

SamplerState SampleType : register(s0);

struct psIn {
	float4 pos : SV_POSITION;
	float2 tex : TEXCOORD0;
};

psIn vs_fs(uint vI : SV_VERTEXID)
{
	psIn output;
    output.tex = float2(vI&1,vI>>1);
    output.pos = float4((output.tex.x-0.5f)*2,-(output.tex.y-0.5f)*2,0,1);
	output.tex.y = 1.0f - output.tex.y;
	return output;
}

float4 ps_fs(psIn inputPS) : SV_TARGET
{
	float4 textureColor = shaderTexture.Sample(SampleType, inputPS.tex);
	return textureColor;
})_";

// ── DLAA: Directionally Localized Anti-Aliasing ──
// Based on Dmitry Andreev's algorithm (LucasArts, GDC 2011).
// Ported from BlueSkyDefender's ReShade implementation (CC BY 3.0).
// Two-pass post-process: PreFilter detects edges, then short+long edge AA smooths jaggies.
constexpr char dlaa_shader_code[] = R"_(
cbuffer DLAACB : register(b0) {
	float2 rcpFrame;    // 1.0/width, 1.0/height
	float dlaaLambda;   // edge sensitivity (default 3.0)
	float dlaaEpsilon;  // luminance threshold (default 0.1)
};

Texture2D srcTex : register(t0);
Texture2D preTex : register(t1);  // pre-filtered texture (pass 2 only)
SamplerState pointSamp : register(s0);

struct VsOut {
	float4 pos : SV_POSITION;
	float2 uv  : TEXCOORD0;
};

VsOut vs_dlaa(uint id : SV_VERTEXID) {
	VsOut o;
	o.uv  = float2(id & 1, id >> 1);
	o.pos = float4((o.uv.x - 0.5) * 2.0, -(o.uv.y - 0.5) * 2.0, 0, 1);
	return o;
}

// Luminance via green channel (perceptually dominant)
float LI(float3 v) { return dot(v.ggg, float3(0.333, 0.333, 0.333)); }

// Helper: sample source texture at pixel offset
float4 LP(float2 uv, float ox, float oy) {
	return srcTex.SampleLevel(pointSamp, uv + float2(ox, oy) * rcpFrame, 0);
}

// ── Pass 1: PreFilter — compute edge luminance, store in alpha ──
float4 ps_dlaa_pre(VsOut input) : SV_TARGET {
	float4 center = LP(input.uv,  0,  0);
	float4 left   = LP(input.uv, -1,  0);
	float4 right  = LP(input.uv,  1,  0);
	float4 top    = LP(input.uv,  0, -1);
	float4 bottom = LP(input.uv,  0,  1);

	float4 edges = 4.0 * abs((left + right + top + bottom) - 4.0 * center);
	float edgesLum = LI(edges.rgb);

	return float4(center.rgb, edgesLum);
}

// Helper: sample pre-filtered texture at pixel offset
float4 SLP(float2 uv, float ox, float oy) {
	return preTex.SampleLevel(pointSamp, uv + float2(ox, oy) * rcpFrame, 0);
}

// ── Pass 2: DLAA — short edge + long edge anti-aliasing ──
float4 ps_dlaa_main(VsOut input) : SV_TARGET {
	// dlaaLambda and dlaaEpsilon come from DLAACB constant buffer

	// Short edge filter: sample center + 4 neighbors from pre-filtered texture
	float4 Center = SLP(input.uv, 0, 0);
	float4 Left   = SLP(input.uv, -1.0, 0);
	float4 Right  = SLP(input.uv,  1.0, 0);
	float4 Up     = SLP(input.uv,  0, -1.0);
	float4 Down   = SLP(input.uv,  0,  1.0);

	float4 combH = 2.0 * (Left + Right);
	float4 combV = 2.0 * (Up + Down);

	float4 CenterDiffH = abs(combH - 4.0 * Center) / 4.0;
	float4 CenterDiffV = abs(combV - 4.0 * Center) / 4.0;

	float4 blurredH = (combH + 2.0 * Center) / 6.0;
	float4 blurredV = (combV + 2.0 * Center) / 6.0;

	float LumH  = LI(CenterDiffH.rgb);
	float LumV  = LI(CenterDiffV.rgb);
	float LumHB = LI(blurredH.rgb);
	float LumVB = LI(blurredV.rgb);

	float satAmountH = saturate((dlaaLambda * LumH - dlaaEpsilon) / LumVB);
	float satAmountV = saturate((dlaaLambda * LumV - dlaaEpsilon) / LumHB);

	// Apply short edge AA
	float4 DLAA = lerp(Center, blurredH, satAmountV);
	DLAA = lerp(DLAA, blurredV, satAmountH * 0.5);

	// Long edge filter: 16 additional samples along H and V axes
	float4 HNeg  = Left;
	float4 HNegA = SLP(input.uv, -3.5, 0.0);
	float4 HNegB = SLP(input.uv, -5.5, 0.0);
	float4 HNegC = SLP(input.uv, -7.5, 0.0);
	float4 HPos  = Right;
	float4 HPosA = SLP(input.uv,  3.5, 0.0);
	float4 HPosB = SLP(input.uv,  5.5, 0.0);
	float4 HPosC = SLP(input.uv,  7.5, 0.0);

	float4 VNeg  = Up;
	float4 VNegA = SLP(input.uv, 0.0, -3.5);
	float4 VNegB = SLP(input.uv, 0.0, -5.5);
	float4 VNegC = SLP(input.uv, 0.0, -7.5);
	float4 VPos  = Down;
	float4 VPosA = SLP(input.uv, 0.0,  3.5);
	float4 VPosB = SLP(input.uv, 0.0,  5.5);
	float4 VPosC = SLP(input.uv, 0.0,  7.5);

	float4 AvgBlurH = (HNeg + HNegA + HNegB + HNegC + HPos + HPosA + HPosB + HPosC) / 8.0;
	float4 AvgBlurV = (VNeg + VNegA + VNegB + VNegC + VPos + VPosA + VPosB + VPosC) / 8.0;

	// Edge activation from alpha channel (pre-computed edge luminance)
	float EAH = saturate(AvgBlurH.a * 2.0 - 1.0);
	float EAV = saturate(AvgBlurV.a * 2.0 - 1.0);

	float longEdge = abs(EAH - EAV) + abs(LumH + LumV);

	if (longEdge > 0.2) {
		// Re-read original pixels for accurate blending
		float4 left_  = LP(input.uv, -1, 0);
		float4 right_ = LP(input.uv,  1, 0);
		float4 up_    = LP(input.uv,  0, -1);
		float4 down_  = LP(input.uv,  0,  1);

		float LongBlurLumH = LI(AvgBlurH.rgb);
		float LongBlurLumV = LI(AvgBlurV.rgb);

		float centerLI = LI(Center.rgb);
		float leftLI   = LI(left_.rgb);
		float rightLI  = LI(right_.rgb);
		float upLI     = LI(up_.rgb);
		float downLI   = LI(down_.rgb);

		float blurUp    = saturate(0.0 + (LongBlurLumH - upLI)     / (centerLI - upLI + 0.0001));
		float blurLeft  = saturate(0.0 + (LongBlurLumV - leftLI)   / (centerLI - leftLI + 0.0001));
		float blurDown  = saturate(1.0 + (LongBlurLumH - centerLI) / (centerLI - downLI + 0.0001));
		float blurRight = saturate(1.0 + (LongBlurLumV - centerLI) / (centerLI - rightLI + 0.0001));

		float4 UDLR = float4(blurLeft, blurRight, blurUp, blurDown);
		if (UDLR.r == 0 && UDLR.g == 0 && UDLR.b == 0 && UDLR.a == 0)
			UDLR = float4(1, 1, 1, 1);

		float4 V = lerp(left_,  Center, UDLR.x);
		V = lerp(right_, V, UDLR.y);
		float4 H = lerp(up_,    Center, UDLR.z);
		H = lerp(down_,  H, UDLR.w);

		DLAA = lerp(DLAA, V, EAV);
		DLAA = lerp(DLAA, H, EAH);
	}

	return float4(DLAA.rgb, 1.0);
}
)_";

// ── FSR upscale: AMD FidelityFX Super Resolution 1.0 (MIT license) ──
// Official AMD EASU + RCAS shaders, compiled from embedded ffx_a.h + ffx_fsr1.h at runtime.
// Pixel shader wrappers provide Gather/Load callbacks and entry points.

// EASU wrapper: edge-adaptive 12-tap Lanczos upscaler using Gather operations
// VrsRadius: x=projCenterX, y=projCenterY, z=innerRadiusSq, w=flag (>0.5=enabled)
// When VRS radius matching is active:
//   - Inside inner radius: full 12-tap EASU (clean 1x1 pixels)
//   - Beyond inner radius: cheap bilinear (avoids shimmer from VRS-degraded input)
static const char fsr_easu_wrapper[] = R"_(
cbuffer CB : register(b0) { uint4 Const0; uint4 Const1; uint4 Const2; uint4 Const3; float4 VrsRadius; };
Texture2D InputTexture : register(t0);
SamplerState samLinearClamp : register(s0);
AF4 FsrEasuRF(AF2 p) { return InputTexture.GatherRed(samLinearClamp, p); }
AF4 FsrEasuGF(AF2 p) { return InputTexture.GatherGreen(samLinearClamp, p); }
AF4 FsrEasuBF(AF2 p) { return InputTexture.GatherBlue(samLinearClamp, p); }
struct VsOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VsOut vs_fsr(uint id : SV_VERTEXID) {
	VsOut o;
	o.uv = float2(id & 1, id >> 1);
	o.pos = float4((o.uv.x - 0.5) * 2.0, -(o.uv.y - 0.5) * 2.0, 0, 1);
	return o;
}
float4 ps_easu(VsOut input) : SV_TARGET {
	// VRS radius matching: outside the VRS inner radius, use bilinear instead of EASU
	if (VrsRadius.w > 0.5) {
		float2 dc = input.uv - VrsRadius.xy;
		float distSq = dot(dc, dc);
		if (distSq > VrsRadius.z) {
			return InputTexture.SampleLevel(samLinearClamp, input.uv, 0);
		}
	}
	AF3 c;
	FsrEasuF(c, AU2(input.pos.xy), Const0, Const1, Const2, Const3);
	return float4(c, 1.0);
}
)_";

// RCAS wrapper: robust contrast-adaptive sharpening (5-tap cross)
// VrsRadius matching: skip sharpening outside VRS inner radius (sharpening VRS-degraded pixels amplifies artifacts)
static const char fsr_rcas_wrapper[] = R"_(
cbuffer CB : register(b0) { uint4 Const0; uint4 Const1; uint4 Const2; uint4 Const3; float4 VrsRadius; };
Texture2D InputTexture : register(t0);
SamplerState samLinearClamp : register(s0);
AF4 FsrRcasLoadF(ASU2 p) { return InputTexture.Load(int3(p, 0)); }
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
struct VsOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VsOut vs_fsr(uint id : SV_VERTEXID) {
	VsOut o;
	o.uv = float2(id & 1, id >> 1);
	o.pos = float4((o.uv.x - 0.5) * 2.0, -(o.uv.y - 0.5) * 2.0, 0, 1);
	return o;
}
float4 ps_rcas(VsOut input) : SV_TARGET {
	// VRS radius matching: outside the VRS inner radius, skip sharpening (just pass through)
	if (VrsRadius.w > 0.5) {
		float2 dc = input.uv - VrsRadius.xy;
		if (dot(dc, dc) > VrsRadius.z) {
			return InputTexture.Load(int3(input.pos.xy, 0));
		}
	}
	AF1 r, g, b;
	FsrRcasF(r, g, b, AU2(input.pos.xy), Const0);
	return float4(r, g, b, 1.0);
}
)_";

static void XTrace(LPCSTR lpszFormat, ...)
{
	va_list args;
	va_start(args, lpszFormat);
	int nBuf;
	char szBuffer[512]; // get rid of this hard-coded buffer
	nBuf = _vsnprintf_s(szBuffer, 511, lpszFormat, args);
	OutputDebugStringA(szBuffer);
	OOVR_LOG(szBuffer);
	va_end(args);
}

#define ERR(msg)                                                                                                                                       \
	{                                                                                                                                                  \
		std::string str = "Hit DX11-related error " + string(msg) + " at " __FILE__ ":" + std::to_string(__LINE__) + " func " + std::string(__func__); \
		OOVR_LOG(str.c_str());                                                                                                                         \
		OOVR_MESSAGE(str.c_str(), "Errored func!");                                                                                                    \
		/**((int*)NULL) = 0;*/                                                                                                                         \
		throw str;                                                                                                                                     \
	}

void DX11Compositor::ThrowIfFailed(HRESULT test)
{
	if ((test) != S_OK) {
		OOVR_FAILED_DX_ABORT(device->GetDeviceRemovedReason());
		throw "ThrowIfFailed err";
	}
}

ID3DBlob* d3d_compile_shader(const char* hlsl, const char* entrypoint, const char* target)
{
	DWORD flags = D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR | D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS;
#ifdef _DEBUG
	flags |= D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_DEBUG;
#else
	flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

	ID3DBlob *compiled, *errors;
	if (FAILED(D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr, entrypoint, target, flags, 0, &compiled, &errors)))
		OOVR_ABORTF("Error: D3DCompile failed %s", (char*)errors->GetBufferPointer());
	if (errors)
		errors->Release();

	return compiled;
}

ID3D11RenderTargetView* d3d_make_rtv(ID3D11Device* d3d_device, XrBaseInStructure& swapchain_img, const DXGI_FORMAT& format)
{
	ID3D11RenderTargetView* result = nullptr;

	// Get information about the swapchain image that OpenXR made for us
	XrSwapchainImageD3D11KHR& d3d_swapchain_img = (XrSwapchainImageD3D11KHR&)swapchain_img;

	// Create a render target view resource for the swapchain image
	D3D11_RENDER_TARGET_VIEW_DESC target_desc = {};
	target_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
	target_desc.Format = format;
	target_desc.Texture2D.MipSlice = 0;
	OOVR_FAILED_DX_ABORT(d3d_device->CreateRenderTargetView(d3d_swapchain_img.texture, &target_desc, &result));

	return result;
}

DX11Compositor::DX11Compositor(ID3D11Texture2D* initial)
{
	initial->GetDevice(&device);
	device->GetImmediateContext(&context);

	// Shaders for inverting copy
	ID3DBlob* fs_vert_shader_blob = d3d_compile_shader(fs_shader_code, "vs_fs", "vs_5_0");
	ID3DBlob* fs_pixel_shader_blob = d3d_compile_shader(fs_shader_code, "ps_fs", "ps_5_0");
	OOVR_FAILED_DX_ABORT(device->CreateVertexShader(fs_vert_shader_blob->GetBufferPointer(), fs_vert_shader_blob->GetBufferSize(), nullptr, &fs_vshader));
	OOVR_FAILED_DX_ABORT(device->CreatePixelShader(fs_pixel_shader_blob->GetBufferPointer(), fs_pixel_shader_blob->GetBufferSize(), nullptr, &fs_pshader));

	// Create a texture sampler state description.
	D3D11_SAMPLER_DESC samplerDesc;
	samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.MipLODBias = 0.0f;
	samplerDesc.MaxAnisotropy = 4;
	samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
	samplerDesc.BorderColor[0] = 0;
	samplerDesc.BorderColor[1] = 0;
	samplerDesc.BorderColor[2] = 0;
	samplerDesc.BorderColor[3] = 0;
	samplerDesc.MinLOD = 0;
	samplerDesc.MaxLOD = 0;

	// Create the texture sampler state.
	OOVR_FAILED_DX_ABORT(device->CreateSamplerState(&samplerDesc, &quad_sampleState));

	// ── DLAA shader init (two-pass: pre-filter + directional AA) ──
	if (oovr_global_configuration.DlaaEnabled()) {
		ID3DBlob* dlaa_vs_blob = d3d_compile_shader(dlaa_shader_code, "vs_dlaa", "vs_5_0");
		ID3DBlob* dlaa_pre_blob = d3d_compile_shader(dlaa_shader_code, "ps_dlaa_pre", "ps_5_0");
		ID3DBlob* dlaa_main_blob = d3d_compile_shader(dlaa_shader_code, "ps_dlaa_main", "ps_5_0");
		if (dlaa_vs_blob && dlaa_pre_blob && dlaa_main_blob) {
			HRESULT hr1 = device->CreateVertexShader(dlaa_vs_blob->GetBufferPointer(), dlaa_vs_blob->GetBufferSize(), nullptr, &dlaa_vshader);
			HRESULT hr2 = device->CreatePixelShader(dlaa_pre_blob->GetBufferPointer(), dlaa_pre_blob->GetBufferSize(), nullptr, &dlaa_pre_pshader);
			HRESULT hr3 = device->CreatePixelShader(dlaa_main_blob->GetBufferPointer(), dlaa_main_blob->GetBufferSize(), nullptr, &dlaa_main_pshader);
			dlaa_vs_blob->Release();
			dlaa_pre_blob->Release();
			dlaa_main_blob->Release();

			if (SUCCEEDED(hr1) && SUCCEEDED(hr2) && SUCCEEDED(hr3)) {
				// Constant buffer: float2 rcpFrame + float2 pad = 16 bytes
				D3D11_BUFFER_DESC cbd = {};
				cbd.ByteWidth = 16;
				cbd.Usage = D3D11_USAGE_DYNAMIC;
				cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
				cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

				// Point sampler for DLAA (we use SampleLevel with point filtering)
				D3D11_SAMPLER_DESC psd = {};
				psd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
				psd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
				psd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
				psd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
				psd.MaxLOD = 0;

				if (SUCCEEDED(device->CreateBuffer(&cbd, nullptr, &dlaa_cbuffer)) &&
				    SUCCEEDED(device->CreateSamplerState(&psd, &dlaa_pointSampler))) {
					dlaaReady = true;
					OOVR_LOG("DLAA: Shaders compiled and ready");
				}
			}
		} else {
			if (dlaa_vs_blob) dlaa_vs_blob->Release();
			if (dlaa_pre_blob) dlaa_pre_blob->Release();
			if (dlaa_main_blob) dlaa_main_blob->Release();
		}
		if (!dlaaReady) {
			OOVR_LOG("DLAA: Shader compilation failed — falling back to no AA");
		}
	}

	// ── FSR upscale shader init: AMD FidelityFX EASU + RCAS (two-pass) ──
	// Compile shaders when either FSR (EASU upscaling) or CAS (RCAS sharpening) is enabled
	if (oovr_global_configuration.FsrEnabled() || oovr_global_configuration.CasEnabled()) {
		// Load AMD FidelityFX headers from Win32 resources
		std::string ffx_a_src = LoadHLSLResource(RES_O_FFX_A);
		std::string ffx_fsr1_src = LoadHLSLResource(RES_O_FFX_FSR1);
		if (ffx_a_src.empty() || ffx_fsr1_src.empty()) {
			OOVR_LOG("FSR: Failed to load AMD FidelityFX HLSL resources");
		}

		// Build shader sources by concatenating AMD headers + our pixel shader wrappers
		std::string easu_hlsl = std::string("#define A_GPU 1\n#define A_HLSL 1\n#define FSR_EASU_F 1\n")
		    + ffx_a_src + "\n" + ffx_fsr1_src + "\n" + fsr_easu_wrapper;
		std::string rcas_hlsl = std::string("#define A_GPU 1\n#define A_HLSL 1\n#define FSR_RCAS_F 1\n#define FSR_RCAS_DENOISE 1\n")
		    + ffx_a_src + "\n" + ffx_fsr1_src + "\n" + fsr_rcas_wrapper;

		ID3DBlob* fsr_vs_blob = d3d_compile_shader(easu_hlsl.c_str(), "vs_fsr", "vs_5_0");
		ID3DBlob* easu_ps_blob = d3d_compile_shader(easu_hlsl.c_str(), "ps_easu", "ps_5_0");
		ID3DBlob* rcas_ps_blob = d3d_compile_shader(rcas_hlsl.c_str(), "ps_rcas", "ps_5_0");
		if (fsr_vs_blob && easu_ps_blob && rcas_ps_blob) {
			HRESULT hr1 = device->CreateVertexShader(fsr_vs_blob->GetBufferPointer(), fsr_vs_blob->GetBufferSize(), nullptr, &fsr_vshader);
			HRESULT hr2 = device->CreatePixelShader(easu_ps_blob->GetBufferPointer(), easu_ps_blob->GetBufferSize(), nullptr, &fsr_pshader);
			HRESULT hr3 = device->CreatePixelShader(rcas_ps_blob->GetBufferPointer(), rcas_ps_blob->GetBufferSize(), nullptr, &cas_pshader);
			fsr_vs_blob->Release();
			easu_ps_blob->Release();
			rcas_ps_blob->Release();

			if (SUCCEEDED(hr1) && SUCCEEDED(hr2) && SUCCEEDED(hr3)) {
				D3D11_BUFFER_DESC cbd = {};
				cbd.ByteWidth = 80; // AMD FSR: 4x uint4 (64 bytes) + VrsRadius float4 (16 bytes)
				cbd.Usage = D3D11_USAGE_DYNAMIC;
				cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
				cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
				if (SUCCEEDED(device->CreateBuffer(&cbd, nullptr, &fsr_cbuffer))) {
					fsrReady = true;
					OOVR_LOGF("FSR/CAS shaders initialized (fsr=%s scale=%.2f, cas=%s sharpness=%.2f)",
					    oovr_global_configuration.FsrEnabled() ? "on" : "off",
					    oovr_global_configuration.FsrRenderScale(),
					    oovr_global_configuration.CasEnabled() ? "on" : "off",
					    oovr_global_configuration.CasSharpness());
				}
			}
		}
		if (!fsrReady) {
			OOVR_LOG("FSR AMD shader compilation failed — falling back to normal rendering");
		}
	}

	// NOTE: VRS is NOT initialized here — it's lazily initialized in the outer Invoke()
	// only on the dxcomp compositor. This prevents temporary compositors from calling
	// NvAPI_Initialize/Disable/Unload and interfering with the active VRS state.

	// OCUnleashed Reconstruction: initialize if enabled (compiles shaders, creates resources)
	if (oovr_global_configuration.WnEnabled()) {
		if (wonderNutts.Initialize(device)) {
			OOVR_LOG("OCUnleashed Reconstruction: initialized");
		} else {
			OOVR_LOG("OCUnleashed Reconstruction: initialization failed — disabled");
		}
	}

}

DX11Compositor::~DX11Compositor()
{
	// [KB-DIAG] Log which compositor is being destroyed and check proximity to dxcomp
	bool iAmDxcomp = (BaseCompositor::dxcomp == this);
	ID3D11Device* preDtorDev = nullptr;
	if (BaseCompositor::dxcomp && !iAmDxcomp) {
		preDtorDev = BaseCompositor::dxcomp->GetDevice();
	}
	OOVR_LOGF("[KB-DIAG] ~DX11Compositor: this=0x%llX dxcomp=0x%llX iAmDxcomp=%d dev=0x%llX dxcomp->dev=0x%llX chain=0x%llX",
	    (unsigned long long)(uintptr_t)this,
	    (unsigned long long)(uintptr_t)BaseCompositor::dxcomp,
	    (int)iAmDxcomp,
	    (unsigned long long)(uintptr_t)device,
	    (unsigned long long)(uintptr_t)preDtorDev,
	    (unsigned long long)(uintptr_t)chain);

	for (auto&& rtv : swapchain_rtvs)
		rtv->Release();

	swapchain_rtvs.clear();

	for (auto&& tex : resolvedMSAATextures)
		tex->Release();

	resolvedMSAATextures.clear();

	// Cached SRV cleanup
	if (cachedSrcSRV) cachedSrcSRV->Release();
	for (auto&& srv : resolvedMSAA_SRVs)
		if (srv) srv->Release();
	resolvedMSAA_SRVs.clear();

	// DLAA cleanup
	if (dlaa_vshader) dlaa_vshader->Release();
	if (dlaa_pre_pshader) dlaa_pre_pshader->Release();
	if (dlaa_main_pshader) dlaa_main_pshader->Release();
	if (dlaa_cbuffer) dlaa_cbuffer->Release();
	if (dlaaIntermediateRTV) dlaaIntermediateRTV->Release();
	if (dlaaIntermediateSRV) dlaaIntermediateSRV->Release();
	if (dlaaIntermediate) dlaaIntermediate->Release();
	if (dlaaOutputRTV) dlaaOutputRTV->Release();
	if (dlaaOutputSRV) dlaaOutputSRV->Release();
	if (dlaaOutput) dlaaOutput->Release();
	if (dlaa_pointSampler) dlaa_pointSampler->Release();

	// FSR cleanup
	if (fsr_vshader) fsr_vshader->Release();
	if (fsr_pshader) fsr_pshader->Release();
	if (cas_pshader) cas_pshader->Release();
	if (fsr_cbuffer) fsr_cbuffer->Release();
	if (fsrIntermediateRTV) fsrIntermediateRTV->Release();
	if (fsrIntermediateSRV) fsrIntermediateSRV->Release();
	if (fsrIntermediate) fsrIntermediate->Release();

	// VRS cleanup: only the dxcomp compositor should call Shutdown (which calls Disable).
	// Non-dxcomp compositors must NOT call Disable() or it will turn off VRS mid-frame.
	if (iAmDxcomp) {
		vrsManager.Shutdown();
	}

	// OCUnleashed Reconstruction cleanup
	wonderNutts.Shutdown();

	context->Release();
	device->Release();

	// [KB-DIAG] Check dxcomp health after device/context Release
	if (BaseCompositor::dxcomp && !iAmDxcomp) {
		ID3D11Device* postRelDev = BaseCompositor::dxcomp->GetDevice();
		OOVR_LOGF("[KB-DIAG] ~DX11Compositor post-Release: dxcomp->dev=0x%llX (was 0x%llX)",
		    (unsigned long long)(uintptr_t)postRelDev,
		    (unsigned long long)(uintptr_t)preDtorDev);
		if (postRelDev && reinterpret_cast<uintptr_t>(postRelDev) <= 0xFFFF) {
			OOVR_LOGF("[KB-DIAG] *** CORRUPTION in ~DX11Compositor *** dxcomp->dev=0x%llX AFTER device->Release()",
			    (unsigned long long)(uintptr_t)postRelDev);
		}
	}

	// Prevent dangling dxcomp after this compositor is destroyed
	if (iAmDxcomp)
		BaseCompositor::dxcomp = nullptr;
}

void DX11Compositor::CheckCreateSwapChain(const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds, bool cube)
{
	XrSwapchainCreateInfo& desc = createInfo;

	auto* src = (ID3D11Texture2D*)texture->handle;

	D3D11_TEXTURE2D_DESC srcDesc;
	src->GetDesc(&srcDesc);

	if (bounds) {
		if (std::fabs(bounds->uMax - bounds->uMin) > 0.1)
			srcDesc.Width = uint32_t(float(srcDesc.Width) * std::fabs(bounds->uMax - bounds->uMin));
		if (std::fabs(bounds->vMax - bounds->vMin) > 0.1)
			srcDesc.Height = uint32_t(float(srcDesc.Height) * std::fabs(bounds->vMax - bounds->vMin));
	}

	if (cube) {
		// LibOVR can only use square cubemaps, while SteamVR can use any shape
		// Note we use CopySubresourceRegion later on, so this won't cause problems with that
		srcDesc.Height = srcDesc.Width = std::min(srcDesc.Height, srcDesc.Width);
	}

	// ── FSR: determine output dimensions (skip for overlay textures) ──
	bool fsrActive = fsrReady && oovr_global_configuration.FsrEnabled()
	    && oovr_global_configuration.FsrRenderScale() < 0.99f && !cube && !bounds && !isOverlay;
	uint32_t outWidth = srcDesc.Width;
	uint32_t outHeight = srcDesc.Height;
	if (fsrActive) {
		float invScale = 1.0f / std::max(0.5f, oovr_global_configuration.FsrRenderScale());
		outWidth = (uint32_t)(srcDesc.Width * invScale);
		outHeight = (uint32_t)(srcDesc.Height * invScale);
	}

	// Check if existing chain is compatible (compare against OUTPUT dimensions)
	bool usable = false;
	if (chain != NULL) {
		if (fsrActive) {
			// FSR: chain was created at output size, input may differ from chain dims
			usable = (outWidth == createInfo.width && outHeight == createInfo.height
			    && srcDesc.Width == fsrInputWidth && srcDesc.Height == fsrInputHeight
			    && srcDesc.Format == createInfoFormat);
		} else {
			usable = CheckChainCompatible(srcDesc, texture->eColorSpace);
		}
	}

	if (!usable) {
		OOVR_LOG("Generating new swap chain");

		if (bounds)
			OOVR_LOGF("Bounds: uMin %f uMax %f vMin %f vMax %f", bounds->uMin, bounds->uMax, bounds->vMin, bounds->vMax);
		OOVR_LOGF("Texture desc format: %d", srcDesc.Format);
		OOVR_LOGF("Texture desc bind flags: %d", srcDesc.BindFlags);
		OOVR_LOGF("Texture desc MiscFlags: %d", srcDesc.MiscFlags);
		OOVR_LOGF("Texture desc Usage: %d", srcDesc.Usage);
		OOVR_LOGF("Texture desc width: %d", srcDesc.Width);
		OOVR_LOGF("Texture desc height: %d", srcDesc.Height);
		if (fsrActive)
			OOVR_LOGF("FSR output: %dx%d (scale %.2f)", outWidth, outHeight, oovr_global_configuration.FsrRenderScale());

		// First, delete the old chain if necessary
		if (chain) {
			OOVR_FAILED_XR_ABORT(xrDestroySwapchain(chain));
			chain = XR_NULL_HANDLE;
		}

		for (auto&& rtv : swapchain_rtvs)
			rtv->Release();

		swapchain_rtvs.clear();

		for (auto&& tex : resolvedMSAATextures)
			tex->Release();

		resolvedMSAATextures.clear();

		// Invalidate cached game texture SRV
		if (cachedSrcSRV) { cachedSrcSRV->Release(); cachedSrcSRV = nullptr; }
		cachedSrcTex = nullptr;
		for (auto&& srv : resolvedMSAA_SRVs)
			if (srv) srv->Release();
		resolvedMSAA_SRVs.clear();

		// Figure out what format we need to use
		DxgiFormatInfo info = {};
		if (!GetFormatInfo(srcDesc.Format, info)) {
			OOVR_ABORTF("Unknown (by OC) DXGI texture format %d", srcDesc.Format);
		}
		bool useLinearFormat;
		switch (texture->eColorSpace) {
		case vr::ColorSpace_Gamma:
			useLinearFormat = false;
			break;
		case vr::ColorSpace_Linear:
			useLinearFormat = true;
			break;
		default:
			// As per the docs for the auto mode, at eight bits per channel or less it assumes gamma
			// (using such small channels for linear colour would result in significant banding)
			useLinearFormat = info.bpc > 8;
			break;
		}

		DXGI_FORMAT type = useLinearFormat ? info.linear : info.srgb;

		if (type == DXGI_FORMAT_UNKNOWN) {
			OOVR_ABORTF("Invalid DXGI target format found: useLinear=%d type=DXGI_FORMAT_UNKNOWN fmt=%d", useLinearFormat, srcDesc.Format);
		}

		// Set aside the old format for checking later
		createInfoFormat = srcDesc.Format;

		// Track FSR input dimensions for compatibility checks
		fsrInputWidth = srcDesc.Width;
		fsrInputHeight = srcDesc.Height;

		// Make eye render buffer (FSR: output at full resolution)
		desc = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
		// TODO desc.Type = cube ? ovrTexture_Cube : ovrTexture_2D;
		desc.faceCount = cube ? 6 : 1;
		desc.width = outWidth;
		desc.height = outHeight;
		desc.format = type;
		desc.mipCount = srcDesc.MipLevels;
		desc.sampleCount = 1;
		desc.arraySize = 1;
		desc.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;

		XrResult result = xrCreateSwapchain(xr_session.get(), &desc, &chain);
		if (!XR_SUCCEEDED(result))
			OOVR_ABORTF("Cannot create DX texture swap chain: err %d", result);

		// Go through the images and retrieve them - this will be used later in Invoke, since OpenXR doesn't
		// have a convenient way to request one specific image.
		uint32_t imageCount;
		OOVR_FAILED_XR_ABORT(xrEnumerateSwapchainImages(chain, 0, &imageCount, nullptr));

		imagesHandles = std::vector<XrSwapchainImageD3D11KHR>(imageCount, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
		OOVR_FAILED_XR_ABORT(xrEnumerateSwapchainImages(chain,
		    imagesHandles.size(), &imageCount, (XrSwapchainImageBaseHeader*)imagesHandles.data()));

		OOVR_FALSE_ABORT(imageCount == imagesHandles.size());

		swapchain_rtvs.resize(imageCount, nullptr);

		for (uint32_t i = 0; i < imageCount; i++) {
			swapchain_rtvs[i] = d3d_make_rtv(device, (XrBaseInStructure&)imagesHandles[i], type);
		}

		if (srcDesc.SampleDesc.Count > 1) {
			OOVR_LOGF("Creating resolver textures for MSAA source with sample count x%d", srcDesc.SampleDesc.Count);
			D3D11_TEXTURE2D_DESC resDesc = srcDesc;
			resDesc.SampleDesc.Count = 1;

			resolvedMSAATextures.resize(imageCount, nullptr);

			resolvedMSAA_SRVs.resize(imageCount, nullptr);
			for (uint32_t i = 0; i < imageCount; i++) {
				device->CreateTexture2D(&resDesc, nullptr, &resolvedMSAATextures[i]);
				device->CreateShaderResourceView(resolvedMSAATextures[i], nullptr, &resolvedMSAA_SRVs[i]);
			}
		}

		// ── FSR: create intermediate texture for EASU→RCAS pipeline ──
		if (fsrActive) {
			// Release old intermediate if it exists
			if (fsrIntermediateRTV) { fsrIntermediateRTV->Release(); fsrIntermediateRTV = nullptr; }
			if (fsrIntermediateSRV) { fsrIntermediateSRV->Release(); fsrIntermediateSRV = nullptr; }
			if (fsrIntermediate) { fsrIntermediate->Release(); fsrIntermediate = nullptr; }

			D3D11_TEXTURE2D_DESC intDesc = {};
			intDesc.Width = outWidth;
			intDesc.Height = outHeight;
			intDesc.MipLevels = 1;
			intDesc.ArraySize = 1;
			intDesc.Format = type;
			intDesc.SampleDesc.Count = 1;
			intDesc.Usage = D3D11_USAGE_DEFAULT;
			intDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

			HRESULT hr = device->CreateTexture2D(&intDesc, nullptr, &fsrIntermediate);
			if (SUCCEEDED(hr)) {
				device->CreateShaderResourceView(fsrIntermediate, nullptr, &fsrIntermediateSRV);
				D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
				rtvDesc.Format = type;
				rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
				device->CreateRenderTargetView(fsrIntermediate, &rtvDesc, &fsrIntermediateRTV);
				OOVR_LOGF("FSR intermediate texture created: %dx%d fmt=%d", outWidth, outHeight, type);
			} else {
				OOVR_LOG("FSR intermediate texture creation failed — falling back to direct copy");
				fsrReady = false;
			}
		}

		// ── DLAA: create intermediate + output textures (game resolution, skip for overlays) ──
		if (dlaaReady && !isOverlay) {
			// Release old textures
			if (dlaaIntermediateRTV) { dlaaIntermediateRTV->Release(); dlaaIntermediateRTV = nullptr; }
			if (dlaaIntermediateSRV) { dlaaIntermediateSRV->Release(); dlaaIntermediateSRV = nullptr; }
			if (dlaaIntermediate) { dlaaIntermediate->Release(); dlaaIntermediate = nullptr; }
			if (dlaaOutputRTV) { dlaaOutputRTV->Release(); dlaaOutputRTV = nullptr; }
			if (dlaaOutputSRV) { dlaaOutputSRV->Release(); dlaaOutputSRV = nullptr; }
			if (dlaaOutput) { dlaaOutput->Release(); dlaaOutput = nullptr; }

			// DLAA operates at the game's render resolution (srcDesc dimensions)
			uint32_t dw = srcDesc.Width;
			uint32_t dh = srcDesc.Height;

			// Intermediate: RGBA8 (RGB = pre-filtered color, A = edge luminance)
			D3D11_TEXTURE2D_DESC diDesc = {};
			diDesc.Width = dw;
			diDesc.Height = dh;
			diDesc.MipLevels = 1;
			diDesc.ArraySize = 1;
			diDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			diDesc.SampleDesc.Count = 1;
			diDesc.Usage = D3D11_USAGE_DEFAULT;
			diDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

			// Output: same format as game texture
			D3D11_TEXTURE2D_DESC doDesc = diDesc;
			doDesc.Format = srcDesc.Format;

			HRESULT hr1 = device->CreateTexture2D(&diDesc, nullptr, &dlaaIntermediate);
			HRESULT hr2 = device->CreateTexture2D(&doDesc, nullptr, &dlaaOutput);
			if (SUCCEEDED(hr1) && SUCCEEDED(hr2)) {
				device->CreateShaderResourceView(dlaaIntermediate, nullptr, &dlaaIntermediateSRV);
				device->CreateShaderResourceView(dlaaOutput, nullptr, &dlaaOutputSRV);
				D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
				rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
				rtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
				device->CreateRenderTargetView(dlaaIntermediate, &rtvDesc, &dlaaIntermediateRTV);
				rtvDesc.Format = srcDesc.Format;
				device->CreateRenderTargetView(dlaaOutput, &rtvDesc, &dlaaOutputRTV);
				dlaaWidth = dw;
				dlaaHeight = dh;
				OOVR_LOGF("DLAA textures created: %dx%d", dw, dh);
			} else {
				OOVR_LOG("DLAA texture creation failed — disabling DLAA");
				dlaaReady = false;
			}
		}

		// TODO do we need to release the images at some point, or does the swapchain do that for us?
	}
}

// VRS + FSR radius matching: these statics are set by the outer Invoke (with eye param)
// and read by the inner Invoke (FSR code) to pass per-eye projection centers to shaders.
static float s_vrsProjX[2] = { 0.5f, 0.5f };
static float s_vrsProjY[2] = { 0.5f, 0.5f };
static int s_vrsEyeW = 0, s_vrsEyeH = 0;
static bool s_vrsInitialFrameDone = false;
static int s_currentEyeIdx = 0;

void DX11Compositor::Invoke(const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds)
{
	auto* src = (ID3D11Texture2D*)texture->handle;

	// OpenXR swap chain doesn't support weird formats like DXGI_FORMAT_BC1_TYPELESS
	D3D11_TEXTURE2D_DESC srcDesc;
	src->GetDesc(&srcDesc);
	if (srcDesc.Format == DXGI_FORMAT_BC1_TYPELESS) {
		if (chain) {
			OOVR_FAILED_XR_ABORT(xrDestroySwapchain(chain));
			chain = XR_NULL_HANDLE;
		}
		return;
	}

	CheckCreateSwapChain(texture, bounds, false);

	// Update cached game texture SRV (Skyrim VR submits the same texture every frame)
	if (!isOverlay && src != cachedSrcTex) {
		if (cachedSrcSRV) { cachedSrcSRV->Release(); cachedSrcSRV = nullptr; }
		device->CreateShaderResourceView(src, nullptr, &cachedSrcSRV);
		cachedSrcTex = src;
	}

	// First reserve an image from the swapchain
	XrSwapchainImageAcquireInfo acquireInfo{ XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	uint32_t currentIndex = 0;
	OOVR_FAILED_XR_ABORT(xrAcquireSwapchainImage(chain, &acquireInfo, &currentIndex));

	// Wait until the swapchain is ready - this makes sure the compositor isn't writing to it
	// We don't have to pass in currentIndex since it uses the oldest acquired-but-not-waited-on
	// image, so we should be careful with concurrency here.
	// XR_TIMEOUT_EXPIRED is considered successful but swapchain still can't be used so need to handle that
	XrSwapchainImageWaitInfo waitInfo{ XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	waitInfo.timeout = 500000000; // time out in nano seconds - 500ms
	XrResult res;
	OOVR_FAILED_XR_ABORT(res = xrWaitSwapchainImage(chain, &waitInfo));

	if (res == XR_TIMEOUT_EXPIRED)
		OOVR_ABORTF("xrWaitSwapchainImage timeout");

	// Copy the source to the destination image
	D3D11_BOX sourceRegion;
	if (bounds) {
		sourceRegion.left = bounds->uMin == 0.0f ? 0 : createInfo.width;
		sourceRegion.right = bounds->uMin == 0.0f ? createInfo.width : createInfo.width * 2;
	} else {
		sourceRegion.left = 0;
		sourceRegion.right = createInfo.width;
	}
	sourceRegion.top = 0;
	sourceRegion.bottom = createInfo.height;
	sourceRegion.front = 0;
	sourceRegion.back = 1;

	// Bounds describe an inverted image so copy texture using pixel shader inverting on copy
	if (bounds && bounds->vMin > bounds->vMax && oovr_global_configuration.InvertUsingShaders() && !swapchain_rtvs.empty()) {
		auto* src = (ID3D11Texture2D*)texture->handle;

		context->OMSetBlendState(nullptr, nullptr, 0xffffffff);

		UINT numViewPorts = 0;
		context->RSGetViewports(&numViewPorts, nullptr);
		D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
		if (numViewPorts) context->RSGetViewports(&numViewPorts, viewports);

		UINT numScissors = 0;
		context->RSGetScissorRects(&numScissors, nullptr);
		D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
		if (numScissors) context->RSGetScissorRects(&numScissors, scissors);

		ID3D11RasterizerState* pRSState;
		context->RSGetState(&pRSState);
		context->RSSetState(nullptr);

		D3D11_VIEWPORT viewport = CD3D11_VIEWPORT(src, swapchain_rtvs[currentIndex]);
		context->RSSetViewports(1, &viewport);
		D3D11_RECT rects[1];
		rects[0].top = 0;
		rects[0].left = 0;
		rects[0].bottom = createInfo.height;
		rects[0].right = createInfo.width;
		context->RSSetScissorRects(1, rects);

		// Set up for rendering
		context->OMSetRenderTargets(1, &swapchain_rtvs[currentIndex], nullptr);
		float clear_colour[4] = { 0.f, 0.f, 0.f, 0.f };
		context->ClearRenderTargetView(swapchain_rtvs[currentIndex], clear_colour);

		// Set the active shaders and constant buffers.
		context->PSSetShaderResources(0, 1, &cachedSrcSRV);
		context->VSSetShader(fs_vshader, nullptr, 0);
		context->PSSetShader(fs_pshader, nullptr, 0);
		context->PSSetSamplers(0, 1, &quad_sampleState);

		// Set up the mesh's information
		D3D11_PRIMITIVE_TOPOLOGY currTopology;
		context->IAGetPrimitiveTopology(&currTopology);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		context->Draw(4, 0);
		context->IASetPrimitiveTopology(currTopology);

		if (numViewPorts)
			context->RSSetViewports(numViewPorts, viewports);
		if (numScissors)
			context->RSSetScissorRects(numScissors, scissors);

		context->RSSetState(pRSState);
	} else if (fsrReady && oovr_global_configuration.FsrEnabled() && !isOverlay
	    && oovr_global_configuration.FsrRenderScale() < 0.99f && !bounds && !swapchain_rtvs.empty()
	    && fsrIntermediate && fsrIntermediateSRV && fsrIntermediateRTV) {
		// ── FSR two-pass path: EASU (upscale) → intermediate → RCAS (sharpen) → swapchain ──
		ID3D11Texture2D* fsrSrc = src;

		// Resolve MSAA first if needed
		if (srcDesc.SampleDesc.Count > 1 && !resolvedMSAATextures.empty()) {
			context->ResolveSubresource(resolvedMSAATextures[currentIndex], 0, src, 0, srcDesc.Format);
			fsrSrc = resolvedMSAATextures[currentIndex];
		}

		// ── DLAA pre-pass (before FSR): anti-alias at internal resolution ──
		if (dlaaReady && oovr_global_configuration.DlaaEnabled() && dlaaIntermediate && dlaaOutput) {
			ID3D11ShaderResourceView* srcSRV = (fsrSrc == src) ? cachedSrcSRV : resolvedMSAA_SRVs[currentIndex];

			// Update DLAA constant buffer
			D3D11_MAPPED_SUBRESOURCE mapped;
			if (SUCCEEDED(context->Map(dlaa_cbuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
				float cbData[4] = { 1.0f / dlaaWidth, 1.0f / dlaaHeight, oovr_global_configuration.DlaaLambda(), oovr_global_configuration.DlaaEpsilon() };
				memcpy(mapped.pData, cbData, 16);
				context->Unmap(dlaa_cbuffer, 0);
			}

			// Save minimal state
			D3D11_PRIMITIVE_TOPOLOGY prevTopo;
			context->IAGetPrimitiveTopology(&prevTopo);
			context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
			context->OMSetBlendState(nullptr, nullptr, 0xffffffff);

			D3D11_VIEWPORT dv = {};
			dv.Width = (float)dlaaWidth;
			dv.Height = (float)dlaaHeight;
			dv.MaxDepth = 1.0f;
			context->RSSetViewports(1, &dv);
			D3D11_RECT dr = { 0, 0, (LONG)dlaaWidth, (LONG)dlaaHeight };
			context->RSSetScissorRects(1, &dr);

			// Pass 1: PreFilter — game texture → dlaaIntermediate (RGBA8, alpha=edge luma)
			context->OMSetRenderTargets(1, &dlaaIntermediateRTV, nullptr);
			context->PSSetShaderResources(0, 1, &srcSRV);        // t0 = game texture
			context->VSSetShader(dlaa_vshader, nullptr, 0);
			context->PSSetShader(dlaa_pre_pshader, nullptr, 0);
			context->PSSetSamplers(0, 1, &dlaa_pointSampler);
			context->PSSetConstantBuffers(0, 1, &dlaa_cbuffer);
			context->Draw(4, 0);

			// Unbind RTV before using as SRV
			ID3D11RenderTargetView* nullRTV = nullptr;
			context->OMSetRenderTargets(1, &nullRTV, nullptr);

			// Pass 2: Main AA — dlaaIntermediate + game texture → dlaaOutput
			context->OMSetRenderTargets(1, &dlaaOutputRTV, nullptr);
			ID3D11ShaderResourceView* srvs[2] = { srcSRV, dlaaIntermediateSRV };
			context->PSSetShaderResources(0, 2, srvs);           // t0 = game, t1 = pre-filtered
			context->PSSetShader(dlaa_main_pshader, nullptr, 0);
			context->Draw(4, 0);

			// Cleanup: unbind SRVs
			ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
			context->PSSetShaderResources(0, 2, nullSRVs);
			context->OMSetRenderTargets(1, &nullRTV, nullptr);
			context->IASetPrimitiveTopology(prevTopo);

			// Use DLAA output as FSR input instead of game texture
			fsrSrc = dlaaOutput;
		}

		// Use cached SRV: dlaaOutputSRV if DLAA ran, cachedSrcSRV for game tex, or pre-created MSAA SRV
		ID3D11ShaderResourceView* gameSRV;
		if (fsrSrc == dlaaOutput)
			gameSRV = dlaaOutputSRV;
		else if (fsrSrc == src)
			gameSRV = cachedSrcSRV;
		else
			gameSRV = resolvedMSAA_SRVs[currentIndex];

		// ── OCUnleashed Reconstruction: clean VRS artifacts before EASU ──
		bool wnActive = oovr_global_configuration.WnEnabled() && wonderNutts.IsReady()
		    && oovr_global_configuration.VrsEnabled() && s_vrsInitialFrameDone;
		if (wnActive) {
			D3D11_TEXTURE2D_DESC wnSrcDesc;
			fsrSrc->GetDesc(&wnSrcDesc);
			wonderNutts.Apply(context, gameSRV,
			    (int)wnSrcDesc.Width, (int)wnSrcDesc.Height,
			    s_vrsProjX[s_currentEyeIdx], s_vrsProjY[s_currentEyeIdx],
			    oovr_global_configuration.WnCleanRadius(),
			    oovr_global_configuration.WnStrength(),
			    oovr_global_configuration.WnSharpness(),
			    oovr_global_configuration.WnSensitivity());
			// Use reconstruction output as EASU input instead of raw game texture
			gameSRV = wonderNutts.GetOutputSRV();
		}

		UINT numViewPorts = 0;
		context->RSGetViewports(&numViewPorts, nullptr);
		D3D11_VIEWPORT savedViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
		if (numViewPorts) context->RSGetViewports(&numViewPorts, savedViewports);

		UINT numScissors = 0;
		context->RSGetScissorRects(&numScissors, nullptr);
		D3D11_RECT savedScissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
		if (numScissors) context->RSGetScissorRects(&numScissors, savedScissors);

		ID3D11RasterizerState* savedRSState = nullptr;
		context->RSGetState(&savedRSState);
		context->RSSetState(nullptr);

		D3D11_PRIMITIVE_TOPOLOGY savedTopology;
		context->IAGetPrimitiveTopology(&savedTopology);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		context->OMSetBlendState(nullptr, nullptr, 0xffffffff);

		// Get actual source dimensions
		D3D11_TEXTURE2D_DESC fsrSrcDesc;
		fsrSrc->GetDesc(&fsrSrcDesc);

		// ── PASS 1: EASU — upscale game texture → intermediate texture ──
		{
			// Fill constant buffer: AMD FsrEasuCon (64 bytes) + VrsRadius (16 bytes)
			D3D11_MAPPED_SUBRESOURCE mapped;
			if (SUCCEEDED(context->Map(fsr_cbuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
				AU1* con = (AU1*)mapped.pData;
				FsrEasuCon(con, con + 4, con + 8, con + 12,
				    (AF1)fsrSrcDesc.Width, (AF1)fsrSrcDesc.Height,   // input viewport
				    (AF1)fsrSrcDesc.Width, (AF1)fsrSrcDesc.Height,   // input image size
				    (AF1)createInfo.width, (AF1)createInfo.height);   // output size

				// VRS radius matching: tell EASU to use bilinear outside VRS inner radius
				// When reconstruction is active, disable radius matching — artifacts already
				// cleaned, so EASU runs full quality everywhere.
				float* vrsData = (float*)((char*)mapped.pData + 64);
				bool vrsActive = oovr_global_configuration.VrsEnabled() && s_vrsInitialFrameDone && !wnActive;
				if (vrsActive) {
					float innerR = oovr_global_configuration.VrsInnerRadius();
					float halfInner = innerR * 0.5f; // VRS distance = 2*sqrt(distSq), so threshold = (innerR/2)²
					vrsData[0] = s_vrsProjX[s_currentEyeIdx]; // projCenterX
					vrsData[1] = s_vrsProjY[s_currentEyeIdx]; // projCenterY
					vrsData[2] = halfInner * halfInner;        // innerRadiusSq
					vrsData[3] = 1.0f;                         // >0.5 = VRS radius matching enabled
				} else {
					vrsData[0] = vrsData[1] = vrsData[2] = 0.0f;
					vrsData[3] = 0.0f; // disabled — full EASU everywhere
				}

				context->Unmap(fsr_cbuffer, 0);
			}

			// Set viewport to output resolution
			D3D11_VIEWPORT vp = {};
			vp.Width = (float)createInfo.width;
			vp.Height = (float)createInfo.height;
			vp.MaxDepth = 1.0f;
			context->RSSetViewports(1, &vp);

			D3D11_RECT scissorRect = { 0, 0, (LONG)createInfo.width, (LONG)createInfo.height };
			context->RSSetScissorRects(1, &scissorRect);

			bool doCas = oovr_global_configuration.CasEnabled() && oovr_global_configuration.CasSharpness() > 0.0f;

			// Render: game texture → EASU → intermediate (if CAS follows) or swapchain (if no CAS)
			context->OMSetRenderTargets(1, doCas ? &fsrIntermediateRTV : &swapchain_rtvs[currentIndex], nullptr);
			context->PSSetShaderResources(0, 1, &gameSRV);
			context->VSSetShader(fsr_vshader, nullptr, 0);
			context->PSSetShader(fsr_pshader, nullptr, 0);  // EASU pixel shader
			context->PSSetSamplers(0, 1, &quad_sampleState);
			context->PSSetConstantBuffers(0, 1, &fsr_cbuffer);
			context->Draw(4, 0);

			// Unbind game SRV
			ID3D11ShaderResourceView* nullSRV = nullptr;
			context->PSSetShaderResources(0, 1, &nullSRV);
		}

		// ── PASS 2: RCAS — sharpen intermediate → swapchain (only if CAS enabled) ──
		if (oovr_global_configuration.CasEnabled() && oovr_global_configuration.CasSharpness() > 0.0f) {
			// Update constant buffer: AMD FsrRcasCon (64 bytes) + VrsRadius (16 bytes)
			D3D11_MAPPED_SUBRESOURCE mapped2;
			if (SUCCEEDED(context->Map(fsr_cbuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped2))) {
				AU1* con = (AU1*)mapped2.pData;
				memset(con, 0, 80); // Clear full buffer including VrsRadius
				float sharpLin = std::max(0.001f, std::min(1.0f, oovr_global_configuration.CasSharpness()));
				float stops = -log2f(sharpLin);
				FsrRcasCon(con, stops);

				// VRS radius matching: skip sharpening outside VRS inner radius
				// When reconstruction is active, disable radius matching — artifacts already cleaned
				float* vrsData = (float*)((char*)mapped2.pData + 64);
				bool vrsActive = oovr_global_configuration.VrsEnabled() && s_vrsInitialFrameDone && !wnActive;
				if (vrsActive) {
					float innerR = oovr_global_configuration.VrsInnerRadius();
					float halfInner = innerR * 0.5f;
					vrsData[0] = s_vrsProjX[s_currentEyeIdx];
					vrsData[1] = s_vrsProjY[s_currentEyeIdx];
					vrsData[2] = halfInner * halfInner;
					vrsData[3] = 1.0f; // >0.5 = VRS radius matching enabled
				}

				context->Unmap(fsr_cbuffer, 0);
			}

			// Render: intermediate → RCAS → swapchain
			context->OMSetRenderTargets(1, &swapchain_rtvs[currentIndex], nullptr);
			context->PSSetShaderResources(0, 1, &fsrIntermediateSRV);
			context->PSSetShader(cas_pshader, nullptr, 0);  // RCAS pixel shader
			context->Draw(4, 0);

			// Unbind intermediate SRV to avoid hazard warnings
			ID3D11ShaderResourceView* nullSRV = nullptr;
			context->PSSetShaderResources(0, 1, &nullSRV);
		}

		context->IASetPrimitiveTopology(savedTopology);

		// Restore state
		if (numViewPorts)
			context->RSSetViewports(numViewPorts, savedViewports);
		if (numScissors)
			context->RSSetScissorRects(numScissors, savedScissors);
		context->RSSetState(savedRSState);
	} else if (fsrReady && oovr_global_configuration.CasEnabled() && !isOverlay
	    && oovr_global_configuration.CasSharpness() > 0.0f
	    && !bounds && !swapchain_rtvs.empty()) {
		// ── CAS-only path: RCAS sharpening at native resolution (no upscaling) ──
		ID3D11Texture2D* casSrc = src;

		// Resolve MSAA first if needed
		if (srcDesc.SampleDesc.Count > 1 && !resolvedMSAATextures.empty()) {
			context->ResolveSubresource(resolvedMSAATextures[currentIndex], 0, src, 0, srcDesc.Format);
			casSrc = resolvedMSAATextures[currentIndex];
		}

		// ── DLAA pre-pass (before CAS): anti-alias at native resolution ──
		if (dlaaReady && oovr_global_configuration.DlaaEnabled() && dlaaIntermediate && dlaaOutput) {
			ID3D11ShaderResourceView* srcSRV = (casSrc == src) ? cachedSrcSRV : resolvedMSAA_SRVs[currentIndex];

			D3D11_MAPPED_SUBRESOURCE mapped;
			if (SUCCEEDED(context->Map(dlaa_cbuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
				float cbData[4] = { 1.0f / dlaaWidth, 1.0f / dlaaHeight, oovr_global_configuration.DlaaLambda(), oovr_global_configuration.DlaaEpsilon() };
				memcpy(mapped.pData, cbData, 16);
				context->Unmap(dlaa_cbuffer, 0);
			}

			D3D11_PRIMITIVE_TOPOLOGY prevTopo;
			context->IAGetPrimitiveTopology(&prevTopo);
			context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
			context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
			D3D11_VIEWPORT dv = {}; dv.Width = (float)dlaaWidth; dv.Height = (float)dlaaHeight; dv.MaxDepth = 1.0f;
			context->RSSetViewports(1, &dv);
			D3D11_RECT dr = { 0, 0, (LONG)dlaaWidth, (LONG)dlaaHeight };
			context->RSSetScissorRects(1, &dr);

			context->OMSetRenderTargets(1, &dlaaIntermediateRTV, nullptr);
			context->PSSetShaderResources(0, 1, &srcSRV);
			context->VSSetShader(dlaa_vshader, nullptr, 0);
			context->PSSetShader(dlaa_pre_pshader, nullptr, 0);
			context->PSSetSamplers(0, 1, &dlaa_pointSampler);
			context->PSSetConstantBuffers(0, 1, &dlaa_cbuffer);
			context->Draw(4, 0);

			ID3D11RenderTargetView* nullRTV = nullptr;
			context->OMSetRenderTargets(1, &nullRTV, nullptr);

			context->OMSetRenderTargets(1, &dlaaOutputRTV, nullptr);
			ID3D11ShaderResourceView* srvs[2] = { srcSRV, dlaaIntermediateSRV };
			context->PSSetShaderResources(0, 2, srvs);
			context->PSSetShader(dlaa_main_pshader, nullptr, 0);
			context->Draw(4, 0);

			ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
			context->PSSetShaderResources(0, 2, nullSRVs);
			context->OMSetRenderTargets(1, &nullRTV, nullptr);
			context->IASetPrimitiveTopology(prevTopo);

			casSrc = dlaaOutput;
		}

		// Use cached SRV: dlaaOutputSRV if DLAA ran, cachedSrcSRV for game tex, or pre-created MSAA SRV
		ID3D11ShaderResourceView* gameSRV;
		if (casSrc == dlaaOutput)
			gameSRV = dlaaOutputSRV;
		else if (casSrc == src)
			gameSRV = cachedSrcSRV;
		else
			gameSRV = resolvedMSAA_SRVs[currentIndex];

		// ── OCUnleashed Reconstruction: clean VRS artifacts before CAS ──
		bool wnActive = oovr_global_configuration.WnEnabled() && wonderNutts.IsReady()
		    && oovr_global_configuration.VrsEnabled() && s_vrsInitialFrameDone;
		if (wnActive) {
			D3D11_TEXTURE2D_DESC wnSrcDesc;
			casSrc->GetDesc(&wnSrcDesc);
			wonderNutts.Apply(context, gameSRV,
			    (int)wnSrcDesc.Width, (int)wnSrcDesc.Height,
			    s_vrsProjX[s_currentEyeIdx], s_vrsProjY[s_currentEyeIdx],
			    oovr_global_configuration.WnCleanRadius(),
			    oovr_global_configuration.WnStrength(),
			    oovr_global_configuration.WnSharpness(),
			    oovr_global_configuration.WnSensitivity());
			gameSRV = wonderNutts.GetOutputSRV();
		}

		UINT numViewPorts = 0;
		context->RSGetViewports(&numViewPorts, nullptr);
		D3D11_VIEWPORT savedViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
		if (numViewPorts) context->RSGetViewports(&numViewPorts, savedViewports);

		UINT numScissors = 0;
		context->RSGetScissorRects(&numScissors, nullptr);
		D3D11_RECT savedScissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
		if (numScissors) context->RSGetScissorRects(&numScissors, savedScissors);

		ID3D11RasterizerState* savedRSState = nullptr;
		context->RSGetState(&savedRSState);
		context->RSSetState(nullptr);

		D3D11_PRIMITIVE_TOPOLOGY savedTopology;
		context->IAGetPrimitiveTopology(&savedTopology);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		context->OMSetBlendState(nullptr, nullptr, 0xffffffff);

		D3D11_TEXTURE2D_DESC casSrcDesc;
		casSrc->GetDesc(&casSrcDesc);

		// Update constant buffer for RCAS-only pass (AMD FsrRcasCon)
		D3D11_MAPPED_SUBRESOURCE mapped;
		if (SUCCEEDED(context->Map(fsr_cbuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			AU1* con = (AU1*)mapped.pData;
			memset(con, 0, 80); // Clear full buffer including VrsRadius
			float sharpLin = std::max(0.001f, std::min(1.0f, oovr_global_configuration.CasSharpness()));
			float stops = -log2f(sharpLin);
			FsrRcasCon(con, stops);
			context->Unmap(fsr_cbuffer, 0);
		}

		D3D11_VIEWPORT vp = {};
		vp.Width = (float)createInfo.width;
		vp.Height = (float)createInfo.height;
		vp.MaxDepth = 1.0f;
		context->RSSetViewports(1, &vp);

		D3D11_RECT scissorRect = { 0, 0, (LONG)createInfo.width, (LONG)createInfo.height };
		context->RSSetScissorRects(1, &scissorRect);

		// Single pass: game texture → RCAS → swapchain
		context->OMSetRenderTargets(1, &swapchain_rtvs[currentIndex], nullptr);
		context->PSSetShaderResources(0, 1, &gameSRV);
		context->VSSetShader(fsr_vshader, nullptr, 0);
		context->PSSetShader(cas_pshader, nullptr, 0);
		context->PSSetConstantBuffers(0, 1, &fsr_cbuffer);
		context->Draw(4, 0);

		ID3D11ShaderResourceView* nullSRV = nullptr;
		context->PSSetShaderResources(0, 1, &nullSRV);

		context->IASetPrimitiveTopology(savedTopology);

		if (numViewPorts)
			context->RSSetViewports(numViewPorts, savedViewports);
		if (numScissors)
			context->RSSetScissorRects(numScissors, savedScissors);
		context->RSSetState(savedRSState);
	} else if (dlaaReady && oovr_global_configuration.DlaaEnabled() && !isOverlay
	    && dlaaIntermediate && dlaaOutput && !bounds && !swapchain_rtvs.empty()) {
		// ── DLAA-only path (no FSR/CAS) ──
		ID3D11Texture2D* dlaaSrc = src;
		if (srcDesc.SampleDesc.Count > 1 && !resolvedMSAATextures.empty()) {
			context->ResolveSubresource(resolvedMSAATextures[currentIndex], 0, src, 0, srcDesc.Format);
			dlaaSrc = resolvedMSAATextures[currentIndex];
		}

		ID3D11ShaderResourceView* srcSRV = (dlaaSrc == src) ? cachedSrcSRV : resolvedMSAA_SRVs[currentIndex];

		// ── OCUnleashed Reconstruction: clean VRS artifacts before DLAA ──
		bool wnActive = oovr_global_configuration.WnEnabled() && wonderNutts.IsReady()
		    && oovr_global_configuration.VrsEnabled() && s_vrsInitialFrameDone;
		if (wnActive) {
			D3D11_TEXTURE2D_DESC wnSrcDesc;
			dlaaSrc->GetDesc(&wnSrcDesc);
			wonderNutts.Apply(context, srcSRV,
			    (int)wnSrcDesc.Width, (int)wnSrcDesc.Height,
			    s_vrsProjX[s_currentEyeIdx], s_vrsProjY[s_currentEyeIdx],
			    oovr_global_configuration.WnCleanRadius(),
			    oovr_global_configuration.WnStrength(),
			    oovr_global_configuration.WnSharpness(),
			    oovr_global_configuration.WnSensitivity());
			srcSRV = wonderNutts.GetOutputSRV();
		}

		D3D11_MAPPED_SUBRESOURCE mapped;
		if (SUCCEEDED(context->Map(dlaa_cbuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			float cbData[4] = { 1.0f / dlaaWidth, 1.0f / dlaaHeight, 0, 0 };
			memcpy(mapped.pData, cbData, 16);
			context->Unmap(dlaa_cbuffer, 0);
		}

		UINT numViewPorts = 0;
		context->RSGetViewports(&numViewPorts, nullptr);
		D3D11_VIEWPORT savedVPs[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
		if (numViewPorts) context->RSGetViewports(&numViewPorts, savedVPs);
		UINT numScissors = 0;
		context->RSGetScissorRects(&numScissors, nullptr);
		D3D11_RECT savedSR[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
		if (numScissors) context->RSGetScissorRects(&numScissors, savedSR);
		ID3D11RasterizerState* savedRS = nullptr;
		context->RSGetState(&savedRS);
		context->RSSetState(nullptr);

		D3D11_PRIMITIVE_TOPOLOGY savedTopo;
		context->IAGetPrimitiveTopology(&savedTopo);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		context->OMSetBlendState(nullptr, nullptr, 0xffffffff);

		D3D11_VIEWPORT dv = {}; dv.Width = (float)dlaaWidth; dv.Height = (float)dlaaHeight; dv.MaxDepth = 1.0f;
		context->RSSetViewports(1, &dv);
		D3D11_RECT dr = { 0, 0, (LONG)dlaaWidth, (LONG)dlaaHeight };
		context->RSSetScissorRects(1, &dr);

		// Pass 1: PreFilter
		context->OMSetRenderTargets(1, &dlaaIntermediateRTV, nullptr);
		context->PSSetShaderResources(0, 1, &srcSRV);
		context->VSSetShader(dlaa_vshader, nullptr, 0);
		context->PSSetShader(dlaa_pre_pshader, nullptr, 0);
		context->PSSetSamplers(0, 1, &dlaa_pointSampler);
		context->PSSetConstantBuffers(0, 1, &dlaa_cbuffer);
		context->Draw(4, 0);

		ID3D11RenderTargetView* nullRTV = nullptr;
		context->OMSetRenderTargets(1, &nullRTV, nullptr);

		// Pass 2: Main AA → swapchain directly
		D3D11_VIEWPORT sv = {}; sv.Width = (float)createInfo.width; sv.Height = (float)createInfo.height; sv.MaxDepth = 1.0f;
		context->RSSetViewports(1, &sv);
		D3D11_RECT sr = { 0, 0, (LONG)createInfo.width, (LONG)createInfo.height };
		context->RSSetScissorRects(1, &sr);

		context->OMSetRenderTargets(1, &swapchain_rtvs[currentIndex], nullptr);
		ID3D11ShaderResourceView* srvs[2] = { srcSRV, dlaaIntermediateSRV };
		context->PSSetShaderResources(0, 2, srvs);
		context->PSSetShader(dlaa_main_pshader, nullptr, 0);
		context->Draw(4, 0);

		ID3D11ShaderResourceView* nullSRVs[2] = { nullptr, nullptr };
		context->PSSetShaderResources(0, 2, nullSRVs);

		context->IASetPrimitiveTopology(savedTopo);
		if (numViewPorts) context->RSSetViewports(numViewPorts, savedVPs);
		if (numScissors) context->RSSetScissorRects(numScissors, savedSR);
		context->RSSetState(savedRS);
	} else {
		// ── Normal copy path (no FSR/CAS/DLAA) ──
		if (srcDesc.SampleDesc.Count > 1) {
			D3D11_TEXTURE2D_DESC resDesc = srcDesc;
			resDesc.SampleDesc.Count = 1;
			context->ResolveSubresource(resolvedMSAATextures[currentIndex], 0, src, 0, resDesc.Format);
			context->CopySubresourceRegion(imagesHandles[currentIndex].texture, 0, 0, 0, 0, resolvedMSAATextures[currentIndex], 0, &sourceRegion);
		} else {
			context->CopySubresourceRegion(imagesHandles[currentIndex].texture, 0, 0, 0, 0, src, 0, &sourceRegion);
		}
	}

	// Release the swapchain - OpenXR will use the last-released image in a swapchain
	XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	OOVR_FAILED_XR_ABORT(xrReleaseSwapchainImage(chain, &releaseInfo));
}

void DX11Compositor::InvokeCubemap(const vr::Texture_t* textures)
{
	CheckCreateSwapChain(&textures[0], nullptr, true);

#ifdef OC_XR_PORT
	ID3D11Texture2D* tex = nullptr;
	ERR("TODO cubemap");
#else
	int currentIndex = 0;
	OOVR_FAILED_OVR_ABORT(ovr_GetTextureSwapChainCurrentIndex(OVSS, chain, &currentIndex));

	OOVR_FAILED_OVR_ABORT(ovr_GetTextureSwapChainBufferDX(OVSS, chain, currentIndex, IID_PPV_ARGS(&tex)));
#endif

	ID3D11Texture2D* faceSrc;

	// Front
	faceSrc = (ID3D11Texture2D*)textures[0].handle;
	context->CopySubresourceRegion(tex, 5, 0, 0, 0, faceSrc, 0, nullptr);

	// Back
	faceSrc = (ID3D11Texture2D*)textures[1].handle;
	context->CopySubresourceRegion(tex, 4, 0, 0, 0, faceSrc, 0, nullptr);

	// Left
	faceSrc = (ID3D11Texture2D*)textures[2].handle;
	context->CopySubresourceRegion(tex, 0, 0, 0, 0, faceSrc, 0, nullptr);

	// Right
	faceSrc = (ID3D11Texture2D*)textures[3].handle;
	context->CopySubresourceRegion(tex, 1, 0, 0, 0, faceSrc, 0, nullptr);

	// Top
	faceSrc = (ID3D11Texture2D*)textures[4].handle;
	context->CopySubresourceRegion(tex, 2, 0, 0, 0, faceSrc, 0, nullptr);

	// Bottom
	faceSrc = (ID3D11Texture2D*)textures[5].handle;
	context->CopySubresourceRegion(tex, 3, 0, 0, 0, faceSrc, 0, nullptr);

	tex->Release();
}

void DX11Compositor::Invoke(XruEye eye, const vr::Texture_t* texture, const vr::VRTextureBounds_t* ptrBounds,
    vr::EVRSubmitFlags submitFlags, XrCompositionLayerProjectionView& layer)
{

	// ── VRS: lazy-init on dxcomp only, then disable before our post-processing shaders ──
	VRSManager* vrsMgr = nullptr;
	if (BaseCompositor::dxcomp && oovr_global_configuration.VrsEnabled()) {
		vrsMgr = BaseCompositor::dxcomp->GetVRSManager();
		// Lazy-init: only initialize VRS on the dxcomp compositor (not temporary compositors)
		if (vrsMgr && !vrsMgr->IsAvailable()) {
			if (vrsMgr->Initialize(BaseCompositor::dxcomp->GetDevice())) {
				OOVR_LOG("VRS: NVIDIA Variable Rate Shading initialized (lazy, on dxcomp)");
			} else {
				OOVR_LOG("VRS: Not available (requires NVIDIA RTX or GTX 16xx series)");
			}
		}
		if (vrsMgr->IsAvailable()) {
			vrsMgr->Disable();
		} else {
			vrsMgr = nullptr; // Not available, skip all VRS code below
		}
	}

	// Set current eye index for FSR radius matching (inner Invoke reads this)
	s_currentEyeIdx = (eye == XruEyeLeft) ? 0 : 1;

	// Copy the texture across
	Invoke(texture, ptrBounds);

	// ── VRS: update projection centers and re-enable for the NEXT eye's rendering ──
	if (vrsMgr && vrsMgr->IsAvailable() && oovr_global_configuration.VrsEnabled()) {
		int eyeIdx = (eye == XruEyeLeft) ? 0 : 1;

		// Extract projection center from this eye's FOV
		float tanL = tanf(layer.fov.angleLeft);
		float tanR = tanf(layer.fov.angleRight);
		float tanU = tanf(layer.fov.angleUp);
		float tanD = tanf(layer.fov.angleDown);
		s_vrsProjX[eyeIdx] = (-tanL) / (tanR - tanL);
		s_vrsProjY[eyeIdx] = tanU / (tanU - tanD);

		// On left eye, record the single-eye texture dimensions
		if (eyeIdx == 0) {
			auto* src = (ID3D11Texture2D*)texture->handle;
			D3D11_TEXTURE2D_DESC desc;
			src->GetDesc(&desc);
			s_vrsEyeW = ptrBounds ? desc.Width / 2 : desc.Width;
			s_vrsEyeH = desc.Height;
		}

		// After right eye (frame complete): update patterns for both eyes
		if (eyeIdx == 1 && s_vrsEyeW > 0 && s_vrsEyeH > 0) {
			vrsMgr->SetProjectionCenters(s_vrsProjX[0], s_vrsProjY[0], s_vrsProjX[1], s_vrsProjY[1]);
			vrsMgr->UpdatePatterns(s_vrsEyeW, s_vrsEyeH);

			if (!s_vrsInitialFrameDone) {
				OOVR_LOGF("VRS: First frame complete — eye %dx%d, projL=(%.3f,%.3f) projR=(%.3f,%.3f)",
				    s_vrsEyeW, s_vrsEyeH,
				    s_vrsProjX[0], s_vrsProjY[0], s_vrsProjX[1], s_vrsProjY[1]);
				s_vrsInitialFrameDone = true;
			}
		}

		// After left eye submit: apply RIGHT eye VRS pattern for game's right eye rendering
		// After right eye submit: apply LEFT eye VRS pattern for next frame's left eye rendering
		if (s_vrsInitialFrameDone) {
			int nextEye = (eyeIdx == 0) ? 1 : 0;
			vrsMgr->ApplyForEye(nextEye);
		}
	}

	// Set the viewport up
	// TODO deduplicate with dx11compositor, and use for all compositors
	XrSwapchainSubImage& subImage = layer.subImage;
	subImage.swapchain = chain;
	subImage.imageArrayIndex = 0; // This is *not* the swapchain index
	XrRect2Di& viewport = subImage.imageRect;
	if (ptrBounds) {
		vr::VRTextureBounds_t bounds = *ptrBounds;

		if (bounds.vMin > bounds.vMax && !oovr_global_configuration.InvertUsingShaders()) {
			std::swap(layer.fov.angleUp, layer.fov.angleDown);
			std::swap(bounds.vMin, bounds.vMax);
		}

		viewport.offset.x = 0;
		viewport.offset.y = 0;
		viewport.extent.width = createInfo.width;
		viewport.extent.height = createInfo.height;
	} else {
		viewport.offset.x = viewport.offset.y = 0;
		viewport.extent.width = createInfo.width;
		viewport.extent.height = createInfo.height;
	}

}

bool DX11Compositor::CheckChainCompatible(D3D11_TEXTURE2D_DESC& inputDesc, vr::EColorSpace colourSpace)
{
	bool usable = true;
#define FAIL(name)                             \
	do {                                       \
		usable = false;                        \
		OOVR_LOG("Resource mismatch: " #name); \
	} while (0);
#define CHECK(name, chainName)                  \
	if (inputDesc.name != createInfo.chainName) \
		FAIL(name);

	CHECK(Width, width)
	CHECK(Height, height)
	CHECK(MipLevels, mipCount)

	if (inputDesc.Format != createInfoFormat) {
		FAIL("Format");
	}

	// CHECK_ADV(SampleDesc.Count, SampleCount);
	// CHECK_ADV(SampleDesc.Quality);
#undef CHECK
#undef FAIL

	return usable;
}

bool DX11Compositor::GetFormatInfo(DXGI_FORMAT format, DX11Compositor::DxgiFormatInfo& out)
{
#define DEF_FMT_BASE(typeless, linear, srgb, bpp, bpc, channels)            \
	{                                                                       \
		out = DxgiFormatInfo{ srgb, linear, typeless, bpp, bpc, channels }; \
		return true;                                                        \
	}

#define DEF_FMT_NOSRGB(name, bpp, bpc, channels) \
	case name##_TYPELESS:                        \
	case name##_UNORM:                           \
		DEF_FMT_BASE(name##_TYPELESS, name##_UNORM, DXGI_FORMAT_UNKNOWN, bpp, bpc, channels)

#define DEF_FMT(name, bpp, bpc, channels) \
	case name##_TYPELESS:                 \
	case name##_UNORM:                    \
	case name##_UNORM_SRGB:               \
		DEF_FMT_BASE(name##_TYPELESS, name##_UNORM, name##_UNORM_SRGB, bpp, bpc, channels)

#define DEF_FMT_UNORM(linear, bpp, bpc, channels) \
	case linear:                                  \
		DEF_FMT_BASE(DXGI_FORMAT_UNKNOWN, linear, DXGI_FORMAT_UNKNOWN, bpp, bpc, channels)

	// Note that this *should* have pretty much all the types we'll ever see in games
	// Filtering out the non-typeless and non-unorm/srgb types, this is all we're left with
	// (note that types that are only typeless and don't have unorm/srgb variants are dropped too)
	switch (format) {
		// The relatively traditional 8bpp 32-bit types
		DEF_FMT(DXGI_FORMAT_R8G8B8A8, 32, 8, 4)
		DEF_FMT(DXGI_FORMAT_B8G8R8A8, 32, 8, 4)
		DEF_FMT(DXGI_FORMAT_B8G8R8X8, 32, 8, 3)

		// Some larger linear-only types
		DEF_FMT_NOSRGB(DXGI_FORMAT_R16G16B16A16, 64, 16, 4)
		DEF_FMT_NOSRGB(DXGI_FORMAT_R10G10B10A2, 32, 10, 4)

		// A jumble of other weird types
		DEF_FMT_UNORM(DXGI_FORMAT_B5G6R5_UNORM, 16, 5, 3)
		DEF_FMT_UNORM(DXGI_FORMAT_B5G5R5A1_UNORM, 16, 5, 4)
		DEF_FMT_UNORM(DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM, 32, 10, 4)
		DEF_FMT_UNORM(DXGI_FORMAT_B4G4R4A4_UNORM, 16, 4, 4)
		DEF_FMT(DXGI_FORMAT_BC1, 64, 16, 4)

	default:
		// Unknown type
		return false;
	}

#undef DEF_FMT
#undef DEF_FMT_NOSRGB
#undef DEF_FMT_BASE
#undef DEF_FMT_UNORM
}

#endif
