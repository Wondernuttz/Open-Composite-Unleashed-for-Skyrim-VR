#include "stdafx.h"

#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)

#include "WonderNutts.h"
#include "../logging.h"

#include <d3dcompiler.h>
#include <cstring>

#pragma comment(lib, "d3dcompiler.lib")

// ═══════════════════════════════════════════════════════════════════════════════
// OCUnleashed Custom Reconstruction Shader
// ═══════════════════════════════════════════════════════════════════════════════
//
// Edge-aware bilateral filter that cleans VRS tile artifacts.
// Runs at render resolution (before any upscaling or post-processing).
//
// The algorithm knows where the VRS zones are (via projection center + radii)
// and applies increasing reconstruction strength from center to periphery.
//
// Center zone: optional CAS-style sharpening (clean 1x1 VRS pixels → crisp)
// Periphery:   bilateral smoothing that preserves real edges while smoothing
//              VRS block boundaries (2x1, 2x2 artifacts)
//
// Total cost: 5 texture samples per pixel (9 in center with sharpening).
// ═══════════════════════════════════════════════════════════════════════════════

static const char s_wonderNuttsShader[] = R"_(
cbuffer WonderNuttsCB : register(b0) {
	float2 projCenter;     // Eye projection center (0-1 normalized UV)
	float2 texelSize;      // 1.0/width, 1.0/height
	float  cleanRadiusSq;  // Center zone: no reconstruction (squared, in UV-distance units)
	float  fadeEndSq;      // Outer limit: full reconstruction (squared)
	float  strength;       // Max reconstruction blend (0-1)
	float  sharpness;      // Center zone CAS sharpening (0-1, 0=off)
	float  sensitivity;    // Bilateral edge sensitivity (1-20, lower = more smoothing)
	float3 _pad;
};

Texture2D InputTex : register(t0);
SamplerState PointSamp : register(s0);

struct VsOut {
	float4 pos : SV_POSITION;
	float2 uv  : TEXCOORD0;
};

VsOut vs_wn(uint id : SV_VERTEXID) {
	VsOut o;
	o.uv  = float2(id & 1, id >> 1);
	o.pos = float4((o.uv.x - 0.5) * 2.0, -(o.uv.y - 0.5) * 2.0, 0, 1);
	return o;
}

// Perceptual luminance (BT.709)
float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

float4 ps_wn(VsOut input) : SV_TARGET {
	float2 uv = input.uv;

	// Distance from projection center (scaled so 1.0 ≈ screen half-diagonal)
	float2 dc = (uv - projCenter) * 2.0;
	float distSq = dot(dc, dc);

	// Center pixel (always needed)
	float4 center = InputTex.SampleLevel(PointSamp, uv, 0);

	// ── Center zone: CAS-style sharpening only ──
	if (distSq < cleanRadiusSq) {
		if (sharpness < 0.001)
			return center;

		// 5-tap cross sharpening (like RCAS but simpler)
		float4 n = InputTex.SampleLevel(PointSamp, uv + float2(0, -texelSize.y), 0);
		float4 s = InputTex.SampleLevel(PointSamp, uv + float2(0,  texelSize.y), 0);
		float4 w = InputTex.SampleLevel(PointSamp, uv + float2(-texelSize.x, 0), 0);
		float4 e = InputTex.SampleLevel(PointSamp, uv + float2( texelSize.x, 0), 0);

		// Adaptive sharpening: sharpen less where there's already high contrast
		float lC = Luma(center.rgb);
		float lMin = min(min(Luma(n.rgb), Luma(s.rgb)), min(Luma(w.rgb), Luma(e.rgb)));
		float lMax = max(max(Luma(n.rgb), Luma(s.rgb)), max(Luma(w.rgb), Luma(e.rgb)));
		float contrast = lMax - lMin;

		// Reduce sharpening in high-contrast areas to avoid ringing
		float adaptiveSharp = sharpness * saturate(1.0 - contrast * 3.0);

		float4 avg = (n + s + w + e) * 0.25;
		float4 sharpened = center + (center - avg) * adaptiveSharp;
		return clamp(sharpened, 0, 1);
	}

	// ── Transition + periphery: edge-aware reconstruction ──

	// Smoothstep fade from 0 (at cleanRadius) to 1 (at fadeEnd)
	float t = saturate((distSq - cleanRadiusSq) / max(fadeEndSq - cleanRadiusSq, 0.0001));
	float fade = t * t * (3.0 - 2.0 * t); // smoothstep
	fade *= strength;

	// Sample 4 cross neighbors
	float4 n = InputTex.SampleLevel(PointSamp, uv + float2(0, -texelSize.y), 0);
	float4 s = InputTex.SampleLevel(PointSamp, uv + float2(0,  texelSize.y), 0);
	float4 w = InputTex.SampleLevel(PointSamp, uv + float2(-texelSize.x, 0), 0);
	float4 e = InputTex.SampleLevel(PointSamp, uv + float2( texelSize.x, 0), 0);

	// Luminance for edge detection
	float lC = Luma(center.rgb);
	float lN = Luma(n.rgb);
	float lS = Luma(s.rgb);
	float lW = Luma(w.rgb);
	float lE = Luma(e.rgb);

	// Neighbor luminance range
	float lMin = min(min(lN, lS), min(lW, lE));
	float lMax = max(max(lN, lS), max(lW, lE));
	float range = lMax - lMin;

	// Luminance outlier rejection: if center pixel is much brighter than ALL
	// neighbors, it's a VRS specular anomaly (GPU sampled a specular highlight
	// in one pixel of a 2x2 block, making the whole block flash white).
	// Clamp center toward neighbor average before reconstruction.
	float nAvgLuma = (lN + lS + lW + lE) * 0.25;
	float outlierExcess = lC - lMax;  // how far above the brightest neighbor
	if (outlierExcess > 0.05) {
		// Blend center toward neighbor average, proportional to how extreme the outlier is
		float clampStrength = saturate(outlierExcess * 4.0); // ramps 0→1 over excess 0.05→0.30
		float4 nAvg = (n + s + w + e) * 0.25;
		center = lerp(center, nAvg, clampStrength * fade);
		lC = Luma(center.rgb);
	}

	// Edge-aware weight:
	// High when center luminance is within neighbor range (VRS block boundary → smooth it)
	// Low when center differs from all neighbors (real scene edge → preserve it)
	float centerDeviation = abs(lC - (lMin + lMax) * 0.5);
	float edgeWeight = 1.0 - saturate(centerDeviation / max(range, 0.01));

	// Bilateral weights: neighbors closer in luminance to center get more influence.
	// This preserves edges while smoothing flat VRS blocks.
	// sensitivity controls how aggressively edges are detected (lower = more smoothing).
	float wN = 1.0 / (1.0 + abs(lC - lN) * sensitivity);
	float wS = 1.0 / (1.0 + abs(lC - lS) * sensitivity);
	float wW = 1.0 / (1.0 + abs(lC - lW) * sensitivity);
	float wE = 1.0 / (1.0 + abs(lC - lE) * sensitivity);
	float wSum = wN + wS + wW + wE;

	// Weighted reconstruction
	float4 reconstructed = (n * wN + s * wS + w * wW + e * wE) / wSum;

	// Minimum blend floor: in the periphery, always apply at least 15% smoothing
	float minBlend = fade * 0.15;
	return lerp(center, reconstructed, max(fade * edgeWeight, minBlend));
}
)_";

// ─── Constant buffer layout (must match shader cbuffer) ───
struct WonderNuttsCBData {
	float projCenterX;    // offset 0
	float projCenterY;    // offset 4
	float texelSizeX;     // offset 8
	float texelSizeY;     // offset 12
	float cleanRadiusSq;  // offset 16
	float fadeEndSq;      // offset 20
	float strength;       // offset 24
	float sharpness;      // offset 28
	float sensitivity;    // offset 32
	float _pad0;          // offset 36
	float _pad1;          // offset 40
	float _pad2;          // offset 44
};
static_assert(sizeof(WonderNuttsCBData) == 48, "CB must be 48 bytes (3x16 aligned)");

// ─── Helper: compile shader ───
static ID3DBlob* CompileShader(const char* hlsl, const char* entry, const char* target)
{
	DWORD flags = D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR | D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_WARNINGS_ARE_ERRORS;
#ifdef _DEBUG
	flags |= D3DCOMPILE_SKIP_OPTIMIZATION | D3DCOMPILE_DEBUG;
#else
	flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

	ID3DBlob* compiled = nullptr;
	ID3DBlob* errors = nullptr;
	HRESULT hr = D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr, entry, target, flags, 0, &compiled, &errors);
	if (FAILED(hr)) {
		const char* errMsg = errors ? (const char*)errors->GetBufferPointer() : "unknown error";
		OOVR_LOGF("OCUnleashed Reconstruction: Shader compile failed: %s", errMsg);
		if (errors) errors->Release();
		return nullptr;
	}
	if (errors) errors->Release();
	return compiled;
}

// ═══════════════════════════════════════════════════════════════════════════════
// Implementation
// ═══════════════════════════════════════════════════════════════════════════════

WonderNutts::~WonderNutts()
{
	Shutdown();
}

bool WonderNutts::Initialize(ID3D11Device* dev)
{
	if (ready)
		return true;

	if (!dev) {
		OOVR_LOG("OCUnleashed Reconstruction: No D3D11 device provided");
		return false;
	}

	OOVR_LOG("OCUnleashed Reconstruction: Initializing...");
	device = dev;

	// Compile vertex shader
	ID3DBlob* vsBlob = CompileShader(s_wonderNuttsShader, "vs_wn", "vs_5_0");
	if (!vsBlob) {
		OOVR_LOG("OCUnleashed Reconstruction: Failed to compile vertex shader");
		return false;
	}

	HRESULT hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vertexShader);
	vsBlob->Release();
	if (FAILED(hr)) {
		OOVR_LOGF("OCUnleashed Reconstruction: CreateVertexShader failed: 0x%08X", hr);
		return false;
	}

	// Compile pixel shader
	ID3DBlob* psBlob = CompileShader(s_wonderNuttsShader, "ps_wn", "ps_5_0");
	if (!psBlob) {
		OOVR_LOG("OCUnleashed Reconstruction: Failed to compile pixel shader");
		Shutdown();
		return false;
	}

	hr = device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &pixelShader);
	psBlob->Release();
	if (FAILED(hr)) {
		OOVR_LOGF("OCUnleashed Reconstruction: CreatePixelShader failed: 0x%08X", hr);
		Shutdown();
		return false;
	}

	// Create constant buffer (32 bytes)
	D3D11_BUFFER_DESC cbd = {};
	cbd.ByteWidth = sizeof(WonderNuttsCBData);
	cbd.Usage = D3D11_USAGE_DYNAMIC;
	cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

	hr = device->CreateBuffer(&cbd, nullptr, &constantBuffer);
	if (FAILED(hr)) {
		OOVR_LOGF("OCUnleashed Reconstruction: CreateBuffer (CB) failed: 0x%08X", hr);
		Shutdown();
		return false;
	}

	// Create point sampler (exact pixel values, no interpolation)
	D3D11_SAMPLER_DESC sd = {};
	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;

	hr = device->CreateSamplerState(&sd, &pointSampler);
	if (FAILED(hr)) {
		OOVR_LOGF("OCUnleashed Reconstruction: CreateSamplerState failed: 0x%08X", hr);
		Shutdown();
		return false;
	}

	ready = true;
	OOVR_LOG("OCUnleashed Reconstruction: Initialized successfully");
	return true;
}

void WonderNutts::EnsureOutputTexture(int width, int height, DXGI_FORMAT format)
{
	if (outputTex && texWidth == width && texHeight == height && texFormat == format)
		return;

	ReleaseOutputTexture();

	D3D11_TEXTURE2D_DESC td = {};
	td.Width = width;
	td.Height = height;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = format;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

	HRESULT hr = device->CreateTexture2D(&td, nullptr, &outputTex);
	if (FAILED(hr)) {
		OOVR_LOGF("OCUnleashed Reconstruction: CreateTexture2D failed: 0x%08X (format=%d)", hr, (int)format);
		return;
	}

	hr = device->CreateRenderTargetView(outputTex, nullptr, &outputRTV);
	if (FAILED(hr)) {
		OOVR_LOGF("OCUnleashed Reconstruction: CreateRenderTargetView failed: 0x%08X", hr);
		ReleaseOutputTexture();
		return;
	}

	hr = device->CreateShaderResourceView(outputTex, nullptr, &outputSRV);
	if (FAILED(hr)) {
		OOVR_LOGF("OCUnleashed Reconstruction: CreateShaderResourceView failed: 0x%08X", hr);
		ReleaseOutputTexture();
		return;
	}

	texWidth = width;
	texHeight = height;
	texFormat = format;
	OOVR_LOGF("OCUnleashed Reconstruction: Created output texture %dx%d (format=%d)", width, height, (int)format);
}

void WonderNutts::ReleaseOutputTexture()
{
	if (outputSRV) { outputSRV->Release(); outputSRV = nullptr; }
	if (outputRTV) { outputRTV->Release(); outputRTV = nullptr; }
	if (outputTex) { outputTex->Release(); outputTex = nullptr; }
	texWidth = 0;
	texHeight = 0;
}

void WonderNutts::Apply(ID3D11DeviceContext* ctx,
    ID3D11ShaderResourceView* inputSRV,
    int width, int height,
    float projCenterX, float projCenterY,
    float cleanRadius, float strength, float sharpness, float sensitivity)
{
	if (!ready || !ctx || !inputSRV)
		return;

	// Auto-detect texture format from input SRV
	DXGI_FORMAT inputFormat = DXGI_FORMAT_R8G8B8A8_UNORM; // fallback
	{
		ID3D11Resource* res = nullptr;
		inputSRV->GetResource(&res);
		if (res) {
			ID3D11Texture2D* tex2d = nullptr;
			if (SUCCEEDED(res->QueryInterface(&tex2d))) {
				D3D11_TEXTURE2D_DESC desc;
				tex2d->GetDesc(&desc);
				inputFormat = desc.Format;
				tex2d->Release();
			}
			res->Release();
		}
	}

	// Ensure output texture matches render resolution and format
	EnsureOutputTexture(width, height, inputFormat);
	if (!outputRTV)
		return;

	// Update constant buffer
	D3D11_MAPPED_SUBRESOURCE mapped;
	HRESULT hr = ctx->Map(constantBuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
	if (SUCCEEDED(hr)) {
		WonderNuttsCBData* cb = static_cast<WonderNuttsCBData*>(mapped.pData);
		cb->projCenterX = projCenterX;
		cb->projCenterY = projCenterY;
		cb->texelSizeX = 1.0f / (float)width;
		cb->texelSizeY = 1.0f / (float)height;

		// Convert cleanRadius to squared UV-distance (same scale as shader)
		// Shader uses: dc = (uv - projCenter) * 2.0; distSq = dot(dc, dc);
		// So a radius of R in "screen units" becomes (R*0.5)² in UV² after the *2.0
		float halfClean = cleanRadius * 0.5f;
		cb->cleanRadiusSq = halfClean * halfClean;

		// Fade ends at distance 1.0 in screen units (half the screen)
		cb->fadeEndSq = 0.25f; // (1.0 * 0.5)² = 0.25

		cb->strength = strength;
		cb->sharpness = sharpness;
		cb->sensitivity = sensitivity;
		cb->_pad0 = 0;
		cb->_pad1 = 0;
		cb->_pad2 = 0;

		ctx->Unmap(constantBuffer, 0);
	}

	// Set viewport to render resolution
	D3D11_VIEWPORT vp = {};
	vp.Width = (float)width;
	vp.Height = (float)height;
	vp.MaxDepth = 1.0f;
	ctx->RSSetViewports(1, &vp);

	// Bind output as render target
	ctx->OMSetRenderTargets(1, &outputRTV, nullptr);

	// Bind input texture and sampler
	ctx->PSSetShaderResources(0, 1, &inputSRV);
	ctx->PSSetSamplers(0, 1, &pointSampler);
	ctx->PSSetConstantBuffers(0, 1, &constantBuffer);

	// Bind shaders
	ctx->VSSetShader(vertexShader, nullptr, 0);
	ctx->PSSetShader(pixelShader, nullptr, 0);

	// Draw fullscreen quad (4 vertices, generated from SV_VERTEXID)
	ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	ctx->IASetInputLayout(nullptr);
	ctx->Draw(4, 0);

	// Unbind input SRV to avoid read/write hazard
	ID3D11ShaderResourceView* nullSRV = nullptr;
	ctx->PSSetShaderResources(0, 1, &nullSRV);
}

void WonderNutts::Shutdown()
{
	ReleaseOutputTexture();

	if (pointSampler) { pointSampler->Release(); pointSampler = nullptr; }
	if (constantBuffer) { constantBuffer->Release(); constantBuffer = nullptr; }
	if (pixelShader) { pixelShader->Release(); pixelShader = nullptr; }
	if (vertexShader) { vertexShader->Release(); vertexShader = nullptr; }

	device = nullptr;
	ready = false;
	OOVR_LOG("OCUnleashed Reconstruction: Shutdown complete");
}

#endif // SUPPORT_DX && SUPPORT_DX11
