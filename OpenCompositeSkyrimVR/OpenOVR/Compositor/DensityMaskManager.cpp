#include "DensityMaskManager.h"
#include "../logging.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <d3dcompiler.h>
#include <d3d11_1.h>

#pragma comment(lib, "d3dcompiler.lib")

// The radial-density pattern and reconstruction layout are derived from the
// MIT-licensed VRPerfKit_RSF implementation at commit
// b2150204fc9886ee0b6fb8be81bb9f795118d1bc. See LICENSE_RDM_MIT.txt.

namespace {

// Keep complete 2x2 pixel quads alive so derivative-dependent scene shaders
// retain neighbouring lanes. NxN describes density, not a hardware VRS footprint.
// All supported footprints divide an 8x8 cluster; reconstruction stays inside
// that same cluster, even across ring boundaries and asymmetric eye origins.
#define OCU_RDM_RATE_HELPERS R"HLSL(
uint2 RateSize(uint rate) {
    if (rate == 1u) return uint2(1u, 2u);
    if (rate == 2u) return uint2(2u, 1u);
    if (rate == 3u) return uint2(2u, 2u);
    if (rate == 4u) return uint2(2u, 4u);
    if (rate == 5u) return uint2(4u, 2u);
    if (rate == 6u) return uint2(4u, 4u);
    return uint2(1u, 1u);
}
uint2 RingSize(float distanceToCenter) {
    return RateSize(distanceToCenter < radius.x ? ringRates.x :
        (distanceToCenter < radius.y ? ringRates.y : ringRates.z));
}
)HLSL"

constexpr char kMaskShader[] = R"HLSL(
Texture2D<float> SceneDepth : register(t0);
cbuffer MaskCB : register(b0) {
    float depthOut;
    float3 radius;
    float2 invClusterResolution;
    float2 projectionCenter;
    float2 eyeOrigin;
    float compatibilityMode;
    float protectDepthEdges;
    uint4 ringRates;
    float inverseHorizontalScale;
    uint blackoutFlags;
    float blackoutCutoff;
    float blackoutGuardPixels;
};
)HLSL" OCU_RDM_RATE_HELPERS R"HLSL(
// Coverage 0 is untouched, 1 is a reconstructable hole, and 64/255 is an
// intentional omission. No surviving donor exists in an omitted cluster.
static const float BLACKOUT_COVERAGE = 64.0 / 255.0;
bool WholeBlackoutCluster(float2 local) {
    if (blackoutFlags == 0u) return false;
    float2 eyeSize = round(8.0 / invClusterResolution);
    float2 tile = floor(local * 0.125) * 8.0;
    float2 lo = ((tile - blackoutGuardPixels) / eyeSize - projectionCenter) * 2.0;
    float2 hi = ((tile + 8.0 + blackoutGuardPixels) / eyeSize - projectionCenter) * 2.0;
    lo.x *= inverseHorizontalScale; hi.x *= inverseHorizontalScale;
    float2 nearest = clamp(float2(0, 0), lo, hi);
    float2 farthest = max(abs(lo), abs(hi));
    float minimum = dot(nearest, nearest);
    float maximum = dot(farthest, farthest);
    bool middle = (blackoutFlags & 1u) != 0u;
    bool outer = (blackoutFlags & 2u) != 0u;
    bool cutoff = (blackoutFlags & 4u) != 0u;
    // Keep a small numerical margin inside the hidden area at every boundary.
    bool beyondInner = minimum > radius.x * radius.x + 0.00001;
    float cutoffRadius = max(radius.y, blackoutCutoff);
    if (middle && (outer || (cutoff && cutoffRadius <= radius.y)))
        return beyondInner;
    if (middle && beyondInner && maximum < radius.y * radius.y - 0.00001)
        return true;
    float outerBoundary = outer ? radius.y : cutoffRadius;
    return (outer || cutoff) && minimum > outerBoundary * outerBoundary + 0.00001;
}
float4 VS(uint vertexId : SV_VertexID) : SV_POSITION {
    float2 p;
    p.x = (vertexId == 2) ? 3.0 : -1.0;
    p.y = (vertexId == 1) ? -3.0 : 1.0;
    return float4(p, depthOut, 1.0);
}

float4 PS(float4 position : SV_POSITION) : SV_TARGET {
    float2 local = position.xy - eyeOrigin;
    // Partial edge clusters have no guaranteed surviving neighbour in this eye.
    float2 clusterEnd = (floor(local * 0.125) + 1.0) * 8.0;
    if (any(clusterEnd > round(8.0 / invClusterResolution)))
        discard;
    if (protectDepthEdges > 0.5) {
        int2 cluster = int2(local) / 8 + int2(int(protectDepthEdges)-1, 0);
        if (SceneDepth.Load(int3(cluster, 0)) < 0.5) discard;
    }
    if (WholeBlackoutCluster(local)) return BLACKOUT_COVERAGE;
    float2 block = floor(local * 0.125) * invClusterResolution;
    float2 offset = block - projectionCenter;
    offset.x *= inverseHorizontalScale;
    float distanceToCenter = length(offset) * 2.0;
    uint2 halfCoord = uint2(max(local, 0.0) * 0.5);

    if (ringRates.w != 0u) {
        uint2 size = RingSize(distanceToCenter);
        if (all((halfCoord % size) == uint2(0u, 0u))) discard;
        return 1.0;
    }

    // Discarded mask pixels retain the game's clear depth and are rendered.
    // Surviving pixels write the opposite depth and skip later scene shading.
    if (distanceToCenter < radius.x)
        discard;
    if (compatibilityMode > 0.5 || distanceToCenter < radius.y) {
        if ((halfCoord.x & 1u) == (halfCoord.y & 1u))
            discard;
        return 1.0;
    }
    if (distanceToCenter < radius.z) {
        if (!((halfCoord.x & 1u) != 0u || (halfCoord.y & 1u) != 0u))
            discard;
        return 1.0;
    }
    if (!((halfCoord.x & 3u) != 0u || (halfCoord.y & 3u) != 0u))
        discard;
    return 1.0;
}
)HLSL";

constexpr char kReconstructShader[] = R"HLSL(
Texture2D<float4> Source : register(t0);
Texture2D<float> Coverage : register(t1);

cbuffer ReconstructCB : register(b0) {
    float2 eyeOrigin;
    float2 eyeSize;
    float2 projectionCenter;
    float2 projectionPadding;
    float3 radius;
    float compatibilityMode;
    uint4 ringRates;
    float inverseHorizontalScale;
    float2 invClusterResolution;
    float aspectPadding;
};
)HLSL" OCU_RDM_RATE_HELPERS R"HLSL(
float4 VS(uint vertexId : SV_VertexID) : SV_POSITION {
    float2 p;
    p.x = (vertexId == 2) ? 3.0 : -1.0;
    p.y = (vertexId == 1) ? -3.0 : 1.0;
    return float4(p, 0.0, 1.0);
}

int2 ClampToEye(int2 p) {
    int2 lo = int2(eyeOrigin);
    int2 hi = lo + int2(eyeSize) - 1;
    return clamp(p, lo, hi);
}

float4 PS(float4 position : SV_POSITION) : SV_TARGET {
    int2 pixel = int2(position.xy);
    if (projectionPadding.x > 0.5 && Coverage.Load(int3(pixel, 0)) < 0.5)
        return Source.Load(int3(pixel, 0));
    int2 local = pixel - int2(eyeOrigin);
    if (any((floor(float2(local) * 0.125) + 1.0) * 8.0 > eyeSize))
        return Source.Load(int3(ClampToEye(pixel), 0));
    uint2 halfCoord = uint2(max(local, int2(0, 0))) >> 1u;
    float2 block = floor(float2(local) * 0.125) * invClusterResolution;
    float2 offset = block - projectionCenter;
    offset.x *= inverseHorizontalScale;
    float distanceToCenter = length(offset) * 2.0;
    int2 samplePixel = pixel;

    if (ringRates.w != 0u) {
        uint2 size = RingSize(distanceToCenter);
        samplePixel -= int2(halfCoord % size) * 2;
        return Source.Load(int3(ClampToEye(samplePixel), 0));
    }

    if (distanceToCenter >= radius.x) {
        bool halfSampleSurvived = (halfCoord.x & 1u) == (halfCoord.y & 1u);
        if (!halfSampleSurvived) {
            samplePixel.x += ((halfCoord.y & 1u) == 0u) ? -2 : 2;
        }

        if (compatibilityMode < 0.5 && distanceToCenter >= radius.y) {
            int2 quarterOffset;
            quarterOffset.x = ((halfCoord.x & 1u) == 0u) ? 0 : -2;
            quarterOffset.y = ((halfCoord.y & 1u) == 0u) ? 0 : -2;
            samplePixel = pixel + quarterOffset;

            if (distanceToCenter >= radius.z) {
                int2 sixteenthBlock = int2(halfCoord) & 3;
                samplePixel = pixel - sixteenthBlock * 2;
            }
        }
    }

    return Source.Load(int3(ClampToEye(samplePixel), 0));
}
)HLSL";

#undef OCU_RDM_RATE_HELPERS

template <typename T>
void ReleasePtr(T*& value)
{
	if (value) {
		value->Release();
		value = nullptr;
	}
}

bool CompileShader(const char* source, const char* entry, const char* target, ID3DBlob** blob)
{
	ID3DBlob* errors = nullptr;
	const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
	const HRESULT hr = D3DCompile(source, std::strlen(source), "OCU Density Mask", nullptr,
	    nullptr, entry, target, flags, 0, blob, &errors);
	if (FAILED(hr)) {
		if (errors) {
			OOVR_LOGF("DensityMask: shader compile failed (%s/%s): %s", entry, target,
			    static_cast<const char*>(errors->GetBufferPointer()));
			errors->Release();
		}
		return false;
	}
	if (errors)
		errors->Release();
	return true;
}

DXGI_FORMAT TypedColorFormat(DXGI_FORMAT format)
{
	switch (format) {
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		return DXGI_FORMAT_R8G8B8A8_UNORM;
	case DXGI_FORMAT_B8G8R8A8_TYPELESS:
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
		return DXGI_FORMAT_B8G8R8A8_UNORM;
	case DXGI_FORMAT_B8G8R8X8_TYPELESS:
	case DXGI_FORMAT_B8G8R8X8_UNORM:
	case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
		return DXGI_FORMAT_B8G8R8X8_UNORM;
	case DXGI_FORMAT_R10G10B10A2_TYPELESS:
	case DXGI_FORMAT_R10G10B10A2_UNORM:
		return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8_UNORM: return DXGI_FORMAT_R8_UNORM;
    case DXGI_FORMAT_R8G8_UNORM: return DXGI_FORMAT_R8G8_UNORM;
    case DXGI_FORMAT_R16_FLOAT: return DXGI_FORMAT_R16_FLOAT;
    case DXGI_FORMAT_R16_UNORM: return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R11G11B10_FLOAT: return DXGI_FORMAT_R11G11B10_FLOAT;
    case DXGI_FORMAT_R16G16_FLOAT: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R32G32_FLOAT: return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return DXGI_FORMAT_R32G32B32A32_FLOAT;
	case DXGI_FORMAT_R16G16B16A16_TYPELESS:
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
		return DXGI_FORMAT_R16G16B16A16_FLOAT;
	default:
		return DXGI_FORMAT_UNKNOWN;
	}
}

struct PipelineState {
	ID3D11VertexShader* vs = nullptr;
	ID3D11PixelShader* ps = nullptr;
	ID3D11GeometryShader* gs = nullptr;
	ID3D11HullShader* hs = nullptr;
	ID3D11DomainShader* ds = nullptr;
	ID3D11InputLayout* inputLayout = nullptr;
	D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
	ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
	ID3D11UnorderedAccessView* uavs[D3D11_1_UAV_SLOT_COUNT] = {};
	UINT rtvCount = 0;
	UINT uavSlotCount = D3D11_PS_CS_UAV_REGISTER_COUNT;
	ID3D11DepthStencilView* dsv = nullptr;
	ID3D11RasterizerState* rasterizer = nullptr;
	ID3D11DepthStencilState* depthState = nullptr;
	UINT stencilRef = 0;
	ID3D11BlendState* blendState = nullptr;
	FLOAT blendFactor[4] = {};
	UINT sampleMask = 0xffffffffu;
	D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
	UINT viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
	D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
	UINT scissorCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
	ID3D11Buffer* vsCB = nullptr;
	ID3D11Buffer* psCB = nullptr;
    ID3D11DeviceContext1* context1 = nullptr;
    UINT vsFirst = 0, vsCount = 0, psFirst = 0, psCount = 0;
    ID3D11ShaderResourceView* psSRVs[9] = {};
    ID3D11ClassInstance* classes[5][D3D11_SHADER_MAX_INTERFACES]{};
    UINT classCounts[5]{};
    ID3D11Predicate* predicate = nullptr;
    BOOL predicateValue = FALSE;
	ID3D11SamplerState* psSampler = nullptr;

	void Capture(ID3D11DeviceContext* ctx)
	{
        classCounts[0] = D3D11_SHADER_MAX_INTERFACES;
        ctx->VSGetShader(&vs, classes[0], &classCounts[0]);
        classCounts[1] = D3D11_SHADER_MAX_INTERFACES;
        ctx->PSGetShader(&ps, classes[1], &classCounts[1]);
        classCounts[2] = D3D11_SHADER_MAX_INTERFACES;
        ctx->GSGetShader(&gs, classes[2], &classCounts[2]);
        classCounts[3] = D3D11_SHADER_MAX_INTERFACES;
        ctx->HSGetShader(&hs, classes[3], &classCounts[3]);
        classCounts[4] = D3D11_SHADER_MAX_INTERFACES;
        ctx->DSGetShader(&ds, classes[4], &classCounts[4]);
		ctx->IAGetInputLayout(&inputLayout);
		ctx->IAGetPrimitiveTopology(&topology);
		ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
		for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
			if (rtvs[i])
				rtvCount = i + 1;
		ID3D11Device* owner = nullptr;
		ctx->GetDevice(&owner);
		if (owner->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_1)
			uavSlotCount = D3D11_1_UAV_SLOT_COUNT;
		owner->Release();
		if (rtvCount < uavSlotCount)
			ctx->OMGetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr,
			    rtvCount, uavSlotCount - rtvCount, uavs + rtvCount);
		ctx->RSGetState(&rasterizer);
		ctx->OMGetDepthStencilState(&depthState, &stencilRef);
		ctx->OMGetBlendState(&blendState, blendFactor, &sampleMask);
		ctx->RSGetViewports(&viewportCount, viewports);
		ctx->RSGetScissorRects(&scissorCount, scissors);
        ctx->QueryInterface(IID_PPV_ARGS(&context1));
        if (context1) {
            context1->VSGetConstantBuffers1(0, 1, &vsCB, &vsFirst, &vsCount);
            context1->PSGetConstantBuffers1(0, 1, &psCB, &psFirst, &psCount);
        } else {
            ctx->VSGetConstantBuffers(0, 1, &vsCB);
            ctx->PSGetConstantBuffers(0, 1, &psCB);
        }
        ctx->PSGetShaderResources(0, 9, psSRVs);
        ctx->GetPredication(&predicate, &predicateValue);
        ctx->SetPredication(nullptr, FALSE);
		ctx->PSGetSamplers(0, 1, &psSampler);
	}

	void Restore(ID3D11DeviceContext* ctx)
	{
        ID3D11ShaderResourceView* nullSRVs[9]{};
        ctx->PSSetShaderResources(0, 9, nullSRVs);
        ctx->VSSetShader(vs, classes[0], classCounts[0]);
        ctx->PSSetShader(ps, classes[1], classCounts[1]);
        ctx->GSSetShader(gs, classes[2], classCounts[2]);
        ctx->HSSetShader(hs, classes[3], classCounts[3]);
        ctx->DSSetShader(ds, classes[4], classCounts[4]);
		ctx->IASetInputLayout(inputLayout);
		ctx->IASetPrimitiveTopology(topology);
		// RTV and pixel-UAV slots overlap; restoring eight RTVs erases shader UAVs.
		UINT counts[D3D11_1_UAV_SLOT_COUNT];
		std::fill_n(counts, D3D11_1_UAV_SLOT_COUNT, 0xffffffffu);
		if (rtvCount < uavSlotCount)
			ctx->OMSetRenderTargetsAndUnorderedAccessViews(rtvCount, rtvs, dsv,
			    rtvCount, uavSlotCount - rtvCount, uavs + rtvCount, counts);
		else
			// Feature level 11.0 has eight shared slots. StartSlot 8 is invalid
			// even with zero UAVs; eight RTVs already occupy the complete range.
			ctx->OMSetRenderTargets(rtvCount, rtvs, dsv);
		ctx->RSSetState(rasterizer);
		ctx->OMSetDepthStencilState(depthState, stencilRef);
		ctx->OMSetBlendState(blendState, blendFactor, sampleMask);
        ctx->RSSetViewports(viewportCount, viewports);
        ctx->RSSetScissorRects(scissorCount, scissors);
        if (context1) {
            context1->VSSetConstantBuffers1(0, 1, &vsCB, &vsFirst, &vsCount);
            context1->PSSetConstantBuffers1(0, 1, &psCB, &psFirst, &psCount);
        } else {
            ctx->VSSetConstantBuffers(0, 1, &vsCB);
            ctx->PSSetConstantBuffers(0, 1, &psCB);
        }
        ctx->PSSetShaderResources(0, 9, psSRVs);
        ctx->SetPredication(predicate, predicateValue);
		ctx->PSSetSamplers(0, 1, &psSampler);
	}

	~PipelineState()
	{
		ReleasePtr(vs);
		ReleasePtr(ps);
		ReleasePtr(gs);
		ReleasePtr(hs);
		ReleasePtr(ds);
		ReleasePtr(inputLayout);
		for (auto*& rtv : rtvs)
			ReleasePtr(rtv);
		for (auto*& uav : uavs)
			ReleasePtr(uav);
		ReleasePtr(dsv);
		ReleasePtr(rasterizer);
		ReleasePtr(depthState);
		ReleasePtr(blendState);
		ReleasePtr(vsCB);
		ReleasePtr(psCB);
        for (auto*& srv : psSRVs) ReleasePtr(srv);
        for (unsigned s = 0; s < 5; ++s)
            for (UINT i = 0; i < classCounts[s]; ++i) ReleasePtr(classes[s][i]);
        ReleasePtr(predicate);
        ReleasePtr(context1);
		ReleasePtr(psSampler);
	}
};

bool SameRegion(const DensityMaskManager::EyeRegion& a,
    const DensityMaskManager::EyeRegion& b)
{
	return a.left == b.left && a.top == b.top &&
	    a.width == b.width && a.height == b.height;
}

} // namespace

#include "RDMPackedResolve.inl"
#include "RDMDepthGuide.inl"

DensityMaskManager::DensityMaskManager() = default;

DensityMaskManager::~DensityMaskManager()
{
	Shutdown();
}

bool DensityMaskManager::Initialize(ID3D11Device* dev)
{
	if (available)
		return true;
	if (initializationAttempted || !dev)
		return false;
	initializationAttempted = true;
	device = dev;
	device->AddRef();
	device->GetImmediateContext(&context);
	if (context)
		context->QueryInterface(IID_PPV_ARGS(&context1));
	available = context && CreateShadersAndStates();
	if (!available) {
		OOVR_LOG("DensityMask: initialization failed; backend disabled for this device session");
		Shutdown();
		initializationAttempted = true;
		return false;
	}
	OOVR_LOG("DensityMask: cross-vendor D3D11 backend initialized");
	return true;
}

bool DensityMaskManager::CreateShadersAndStates()
{
	ID3DBlob* vsBlob = nullptr;
	ID3DBlob* maskBlob = nullptr;
	ID3DBlob* reconstructBlob = nullptr;
	ID3DBlob* reconstructVSBlob = nullptr;
	if (!CompileShader(kMaskShader, "VS", "vs_5_0", &vsBlob) ||
	    !CompileShader(kMaskShader, "PS", "ps_5_0", &maskBlob) ||
	    !CompileShader(kReconstructShader, "PS", "ps_5_0", &reconstructBlob) ||
	    !CompileShader(kReconstructShader, "VS", "vs_5_0", &reconstructVSBlob)) {
		ReleasePtr(vsBlob);
		ReleasePtr(maskBlob);
		ReleasePtr(reconstructBlob);
		ReleasePtr(reconstructVSBlob);
		return false;
	}
	HRESULT hr = device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &maskVS);
	if (SUCCEEDED(hr))
		hr = device->CreateVertexShader(reconstructVSBlob->GetBufferPointer(), reconstructVSBlob->GetBufferSize(), nullptr, &reconstructVS);
	if (SUCCEEDED(hr))
		hr = device->CreatePixelShader(maskBlob->GetBufferPointer(), maskBlob->GetBufferSize(), nullptr, &maskPS);
	if (SUCCEEDED(hr))
		hr = device->CreatePixelShader(reconstructBlob->GetBufferPointer(), reconstructBlob->GetBufferSize(), nullptr, &reconstructPS);
	ReleasePtr(vsBlob);
	ReleasePtr(maskBlob);
	ReleasePtr(reconstructBlob);
	ReleasePtr(reconstructVSBlob);
	if (FAILED(hr))
		return false;

	D3D11_BUFFER_DESC cbd = {};
	cbd.ByteWidth = sizeof(MaskConstants);
	cbd.Usage = D3D11_USAGE_DYNAMIC;
	cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	if (FAILED(device->CreateBuffer(&cbd, nullptr, &maskCB)))
		return false;
	cbd.ByteWidth = sizeof(ReconstructConstants);
	if (FAILED(device->CreateBuffer(&cbd, nullptr, &reconstructCB)))
		return false;

	D3D11_DEPTH_STENCIL_DESC depth = {};
	depth.DepthEnable = TRUE;
	depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
	depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
	depth.StencilEnable = FALSE;
	if (FAILED(device->CreateDepthStencilState(&depth, &maskDepthState)))
		return false;
	depth.DepthEnable = FALSE;
	depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
	if (FAILED(device->CreateDepthStencilState(&depth, &noDepthState)))
		return false;

	D3D11_RASTERIZER_DESC raster = {};
	raster.FillMode = D3D11_FILL_SOLID;
	raster.CullMode = D3D11_CULL_NONE;
	raster.DepthClipEnable = TRUE;
	raster.ScissorEnable = TRUE;
	if (FAILED(device->CreateRasterizerState(&raster, &rasterizerState)))
		return false;

	D3D11_SAMPLER_DESC sampler = {};
	sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sampler.MaxLOD = D3D11_FLOAT32_MAX;
	return SUCCEEDED(device->CreateSamplerState(&sampler, &samplerState));
}

bool DensityMaskManager::ValidateGeometry(int width, int height,
    const EyeRegion& leftEye, const EyeRegion& rightEye) const
{
	if (width <= 0 || height <= 0 || leftEye.width <= 0 || leftEye.height <= 0 ||
	    rightEye.width <= 0 || rightEye.height <= 0)
		return false;
	auto fits = [width, height](const EyeRegion& r) {
		return r.left >= 0 && r.top >= 0 && r.left + r.width <= width &&
		    r.top + r.height <= height;
	};
	const bool overlap = leftEye.left < rightEye.left + rightEye.width &&
	    leftEye.left + leftEye.width > rightEye.left &&
	    leftEye.top < rightEye.top + rightEye.height &&
	    leftEye.top + leftEye.height > rightEye.top;
	return fits(leftEye) && fits(rightEye) && !overlap;
}

bool DensityMaskManager::PrepareStereoTarget(ID3D11Texture2D* target, int width, int height,
	const EyeRegion& leftEye, const EyeRegion& rightEye)
{
	armed = false;
	if (!available || !target || !ValidateGeometry(width, height, leftEye, rightEye)) {
		OOVR_LOG_LIMITEDF(5000, "DensityMask: invalid stereo target geometry; backend withheld");
		return false;
	}

	D3D11_TEXTURE2D_DESC desc = {};
	target->GetDesc(&desc);
	if (desc.Width != static_cast<UINT>(width) || desc.Height != static_cast<UINT>(height) ||
	    desc.ArraySize != 1 || desc.SampleDesc.Count != 1 || TypedColorFormat(desc.Format) == DXGI_FORMAT_UNKNOWN) {
		if (!unsupportedTargetLogged) {
			unsupportedTargetLogged = true;
			OOVR_LOGF("DensityMask: unsupported scene target %ux%u fmt=%u array=%u samples=%u; no mask applied",
			    desc.Width, desc.Height, desc.Format, desc.ArraySize, desc.SampleDesc.Count);
		}
		return false;
	}

	const bool targetChanged = !sourceCopy || !sourceSRV || !reconstructed || !reconstructedRTV ||
        width != renderWidth || height != renderHeight || desc.Format != resourceFormat;
	if (targetChanged) {
		ReleaseColorResources();
		if (!CreateColorResources(desc))
			return false;
	}

	sceneTarget = target;
	renderWidth = width;
	renderHeight = height;
	eyeRegions[0] = leftEye;
	eyeRegions[1] = rightEye;
	resourceFormat = desc.Format;
	armed = true;
	return true;
}

bool DensityMaskManager::CreateColorResources(const D3D11_TEXTURE2D_DESC& sourceDesc)
{
	const DXGI_FORMAT viewFormat = TypedColorFormat(sourceDesc.Format);
	if (viewFormat == DXGI_FORMAT_UNKNOWN)
		return false;

	D3D11_TEXTURE2D_DESC td = sourceDesc;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.CPUAccessFlags = 0;
	td.MiscFlags = 0;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	if (FAILED(device->CreateTexture2D(&td, nullptr, &sourceCopy)))
		return false;

	D3D11_SHADER_RESOURCE_VIEW_DESC srv = {};
	srv.Format = viewFormat;
	srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srv.Texture2D.MipLevels = 1;
	if (FAILED(device->CreateShaderResourceView(sourceCopy, &srv, &sourceSRV))) {
		ReleaseColorResources();
		return false;
	}

	td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	if (FAILED(device->CreateTexture2D(&td, nullptr, &reconstructed))) {
		ReleaseColorResources();
		return false;
	}
	D3D11_RENDER_TARGET_VIEW_DESC rtv = {};
	rtv.Format = viewFormat;
	rtv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
	if (FAILED(device->CreateRenderTargetView(reconstructed, &rtv, &reconstructedRTV))) {
		ReleaseColorResources();
		return false;
	}
	OOVR_LOGF("DensityMask: reconstruction resources ready for %ux%u fmt=%u",
	    sourceDesc.Width, sourceDesc.Height, sourceDesc.Format);
	return true;
}

void DensityMaskManager::SetPatternSettings(const PatternSettings& settings)
{
	patternSettings = settings;
	patternSettings.horizontalScale = std::isfinite(settings.horizontalScale) ?
	    std::clamp(settings.horizontalScale, 0.5f, 2.0f) : 1.0f;
	if (!std::isfinite(settings.innerRadius) || !std::isfinite(settings.midRadius) ||
	    settings.innerRadius < 0.f || settings.midRadius < settings.innerRadius ||
	    !std::isfinite(settings.blackout.cutoffRadius) || settings.blackout.cutoffRadius < 0.f ||
	    !std::isfinite(settings.blackout.guardPixels) || settings.blackout.guardPixels < 0.f)
		patternSettings.blackout = {};
}

void DensityMaskManager::SetProjectionCenters(float leftX, float leftY, float rightX, float rightY)
{
	projX[0] = std::clamp(leftX, -0.25f, 1.25f);
	projY[0] = std::clamp(leftY, -0.25f, 1.25f);
	projX[1] = std::clamp(rightX, -0.25f, 1.25f);
	projY[1] = std::clamp(rightY, -0.25f, 1.25f);
}

void DensityMaskManager::BeginFrame()
{
	armed = false;
	maskAppliedThisFrame = false;
	reconstructedThisFrame = false;
}

void DensityMaskManager::EndFrameMasking()
{
	armed = false;
}

bool DensityMaskManager::ApplyDepthMask(ID3D11DepthStencilView* dsv, float clearDepth,
    ID3D11RenderTargetView* coverage, ID3D11ShaderResourceView* sceneDepth)
{
	if (!available || !armed || !dsv)
		return false;
	D3D11_DEPTH_STENCIL_VIEW_DESC viewDesc = {};
	dsv->GetDesc(&viewDesc);
	if ((viewDesc.Flags & D3D11_DSV_READ_ONLY_DEPTH) != 0)
		return false;

	ID3D11Resource* resource = nullptr;
	dsv->GetResource(&resource);
	ID3D11Texture2D* depthTexture = nullptr;
	if (resource) {
		resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&depthTexture));
		resource->Release();
	}
	if (!depthTexture)
		return false;
	D3D11_TEXTURE2D_DESC desc = {};
	depthTexture->GetDesc(&desc);
	depthTexture->Release();
	if (desc.Width != static_cast<UINT>(renderWidth) || desc.Height != static_cast<UINT>(renderHeight) ||
	    desc.ArraySize != 1) {
		return false;
	}

	PipelineState state;
	state.Capture(context);
	const bool left = DrawMaskForEye(dsv, 0, clearDepth, coverage, sceneDepth);
	const bool right = DrawMaskForEye(dsv, 1, clearDepth, coverage, sceneDepth);
	state.Restore(context);
	maskAppliedThisFrame = left && right;
	return maskAppliedThisFrame;
}

bool DensityMaskManager::DrawMaskForEye(ID3D11DepthStencilView* dsv, int eye, float clearDepth,
    ID3D11RenderTargetView* coverage, ID3D11ShaderResourceView* sceneDepth)
{
	MaskConstants constants = {};
	constants.inverseHorizontalScale = 1.0f / patternSettings.horizontalScale;
	// Intentional omissions require the same ownership/depth eligibility proof
	// and coverage record as the private scene pass that will resolve this mask.
	if (coverage && sceneDepth && ocu_foveation::ValidBlackoutGeometry(projX[eye], projY[eye],
	        patternSettings.innerRadius, patternSettings.midRadius, patternSettings.horizontalScale)) {
		const auto& blackout = patternSettings.blackout;
		constants.blackoutFlags = (blackout.middle ? 1u : 0u) |
		    (blackout.outer ? 2u : 0u) | (blackout.cutoff ? 4u : 0u);
		constants.blackoutCutoff = float(ocu_foveation::BlackoutCutoff(blackout, patternSettings.midRadius));
		constants.blackoutGuardPixels = blackout.guardPixels;
	}
	constants.ringRates[0] = static_cast<unsigned>(patternSettings.rates.inner);
	constants.ringRates[1] = static_cast<unsigned>(patternSettings.rates.mid);
	constants.ringRates[2] = static_cast<unsigned>(patternSettings.rates.outer);
	constants.ringRates[3] = patternSettings.customEyeRates ? 1u : 0u;
    constants.padding = sceneDepth ? float(1 + (eye == 1 ? (eyeRegions[0].width+7)/8 : 0)) : 0.0f;
	constants.depthOut = clearDepth < 0.5f ? 1.0f : 0.0f;
	constants.radius[0] = patternSettings.innerRadius;
	constants.radius[1] = std::max(constants.radius[0], patternSettings.midRadius);
	constants.radius[2] = std::max(constants.radius[1], 1.0f);
	constants.invClusterResolution[0] = 8.0f / static_cast<float>(eyeRegions[eye].width);
	constants.invClusterResolution[1] = 8.0f / static_cast<float>(eyeRegions[eye].height);
	constants.projectionCenter[0] = projX[eye];
	constants.projectionCenter[1] = projY[eye];
	constants.eyeOrigin[0] = static_cast<float>(eyeRegions[eye].left);
	constants.eyeOrigin[1] = static_cast<float>(eyeRegions[eye].top);
	constants.compatibilityMode = patternSettings.compatibilityMode ? 1.0f : 0.0f;

	D3D11_MAPPED_SUBRESOURCE mapped = {};
	if (FAILED(context->Map(maskCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
		return false;
	std::memcpy(mapped.pData, &constants, sizeof(constants));
	context->Unmap(maskCB, 0);

	context->VSSetShader(maskVS, nullptr, 0);
	context->PSSetShader(maskPS, nullptr, 0);
	context->GSSetShader(nullptr, nullptr, 0);
	context->HSSetShader(nullptr, nullptr, 0);
	context->DSSetShader(nullptr, nullptr, 0);
	context->VSSetConstantBuffers(0, 1, &maskCB);
	context->PSSetConstantBuffers(0, 1, &maskCB);
	context->IASetInputLayout(nullptr);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->OMSetRenderTargets(coverage ? 1 : 0, coverage ? &coverage : nullptr, dsv);
    context->PSSetShaderResources(0, 1, &sceneDepth);
	context->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
	context->OMSetDepthStencilState(maskDepthState, 0);
	context->RSSetState(rasterizerState);

	D3D11_VIEWPORT viewport = {};
	viewport.TopLeftX = static_cast<float>(eyeRegions[eye].left);
	viewport.TopLeftY = static_cast<float>(eyeRegions[eye].top);
	viewport.Width = static_cast<float>(eyeRegions[eye].width);
	viewport.Height = static_cast<float>(eyeRegions[eye].height);
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	context->RSSetViewports(1, &viewport);
	D3D11_RECT scissor = { eyeRegions[eye].left, eyeRegions[eye].top,
		eyeRegions[eye].left + eyeRegions[eye].width,
		eyeRegions[eye].top + eyeRegions[eye].height };
	context->RSSetScissorRects(1, &scissor);
	context->Draw(3, 0);
	return true;
}

ID3D11Texture2D* DensityMaskManager::ReconstructStereo(ID3D11Texture2D* source, int submittedEye)
{
	if (!available || !maskAppliedThisFrame || !source || !sourceCopy || !reconstructed)
		return nullptr;
	if (source != sceneTarget)
		return nullptr;
	if (reconstructedThisFrame && submittedEye == 1)
		return reconstructed;

	context->CopyResource(sourceCopy, source);
	context->CopyResource(reconstructed, sourceCopy);
	PipelineState state;
	state.Capture(context);
	const bool left = DrawReconstructionForEye(0);
	const bool right = DrawReconstructionForEye(1);
	state.Restore(context);
	if (!left || !right) {
		OOVR_LOG("DensityMask: reconstruction draw failed; disabling backend for the session");
		available = false;
		return nullptr;
	}
	reconstructedThisFrame = true;
	return reconstructed;
}

bool DensityMaskManager::DrawReconstructionForEye(int eye, ID3D11ShaderResourceView* coverage)
{
	ReconstructConstants constants = {};
	constants.inverseHorizontalScale = 1.0f / patternSettings.horizontalScale;
	constants.invClusterResolution[0] = 8.0f / static_cast<float>(eyeRegions[eye].width);
	constants.invClusterResolution[1] = 8.0f / static_cast<float>(eyeRegions[eye].height);
	constants.ringRates[0] = static_cast<unsigned>(patternSettings.rates.inner);
	constants.ringRates[1] = static_cast<unsigned>(patternSettings.rates.mid);
	constants.ringRates[2] = static_cast<unsigned>(patternSettings.rates.outer);
	constants.ringRates[3] = patternSettings.customEyeRates ? 1u : 0u;
	constants.projectionPadding[0] = coverage ? 1.0f : 0.0f;
	constants.eyeOrigin[0] = static_cast<float>(eyeRegions[eye].left);
	constants.eyeOrigin[1] = static_cast<float>(eyeRegions[eye].top);
	constants.eyeSize[0] = static_cast<float>(eyeRegions[eye].width);
	constants.eyeSize[1] = static_cast<float>(eyeRegions[eye].height);
	constants.projectionCenter[0] = projX[eye];
	constants.projectionCenter[1] = projY[eye];
	constants.radius[0] = patternSettings.innerRadius;
	constants.radius[1] = std::max(constants.radius[0], patternSettings.midRadius);
	constants.radius[2] = std::max(constants.radius[1], 1.0f);
	constants.compatibilityMode = patternSettings.compatibilityMode ? 1.0f : 0.0f;

	D3D11_MAPPED_SUBRESOURCE mapped = {};
	if (FAILED(context->Map(reconstructCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
		return false;
	std::memcpy(mapped.pData, &constants, sizeof(constants));
	context->Unmap(reconstructCB, 0);

	// This vertex shader has no constant-buffer dependency. Game VS b0 may
	// contain arbitrary camera data and must never control fullscreen depth.
	context->VSSetShader(reconstructVS, nullptr, 0);
	context->PSSetShader(reconstructPS, nullptr, 0);
	context->GSSetShader(nullptr, nullptr, 0);
	context->HSSetShader(nullptr, nullptr, 0);
	context->DSSetShader(nullptr, nullptr, 0);
    ID3D11ShaderResourceView* inputs[2] = {sourceSRV, coverage};
    context->PSSetShaderResources(0, 2, inputs);
	context->PSSetSamplers(0, 1, &samplerState);
	context->PSSetConstantBuffers(0, 1, &reconstructCB);
	context->IASetInputLayout(nullptr);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	context->OMSetRenderTargets(1, &reconstructedRTV, nullptr);
	context->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
	context->OMSetDepthStencilState(noDepthState, 0);
	context->RSSetState(rasterizerState);

	D3D11_VIEWPORT viewport = {};
	viewport.TopLeftX = static_cast<float>(eyeRegions[eye].left);
	viewport.TopLeftY = static_cast<float>(eyeRegions[eye].top);
	viewport.Width = static_cast<float>(eyeRegions[eye].width);
	viewport.Height = static_cast<float>(eyeRegions[eye].height);
	viewport.MinDepth = 0.0f;
	viewport.MaxDepth = 1.0f;
	context->RSSetViewports(1, &viewport);
	D3D11_RECT scissor = { eyeRegions[eye].left, eyeRegions[eye].top,
		eyeRegions[eye].left + eyeRegions[eye].width,
		eyeRegions[eye].top + eyeRegions[eye].height };
	context->RSSetScissorRects(1, &scissor);
	context->Draw(3, 0);
	return true;
}

bool DensityMaskManager::ResolveColor(ID3D11Texture2D* source,
    ID3D11ShaderResourceView* coverage, unsigned eyes)
{
    if (!available || !armed || source != sceneTarget || !coverage) return false;
    PipelineState state;
    state.Capture(context);
    context->CopyResource(sourceCopy, source);
    context->CopyResource(reconstructed, source);
    bool ok = true;
    for (int eye = 0; eye < 2; ++eye)
        if (eyes & (1u << eye)) ok = DrawReconstructionForEye(eye, coverage) && ok;
    if (ok) context->CopyResource(source, reconstructed);
    state.Restore(context);
    return ok;
}

void DensityMaskManager::ReleaseColorResources()
{
	ReleasePtr(sourceCopy);
	ReleasePtr(sourceSRV);
	ReleasePtr(reconstructed);
	ReleasePtr(reconstructedRTV);
	resourceFormat = DXGI_FORMAT_UNKNOWN;
	reconstructedThisFrame = false;
}

void DensityMaskManager::Shutdown()
{
	EndDepthGuide();
	ReleasePtr(guideTexture); ReleasePtr(guideRTV); ReleasePtr(guideSRV);
	ReleasePtr(guidePS); ReleasePtr(guideCB); ReleasePtr(guideInvalidationCB); ReleasePtr(guideInvalidateDepth);
	guideWidth = guideHeight = guideDraws = 0;
	armed = false;
	maskAppliedThisFrame = false;
	reconstructedThisFrame = false;
	sceneTarget = nullptr;
	ReleaseColorResources();
	ReleasePackedResources();
	for (auto*& shader : gatherShaders) ReleasePtr(shader);
	for (auto*& shader : scatterShaders) ReleasePtr(shader);
	for (auto& entry : scatterBlendStates) ReleasePtr(entry.second);
	scatterBlendStates.clear(); scatterBlendState = nullptr;
	ReleasePtr(maskVS);
	ReleasePtr(reconstructVS);
	ReleasePtr(maskPS);
	ReleasePtr(reconstructPS);
	ReleasePtr(maskCB);
	ReleasePtr(reconstructCB);
	ReleasePtr(maskDepthState);
	ReleasePtr(noDepthState);
	ReleasePtr(rasterizerState);
	ReleasePtr(samplerState);
	ReleasePtr(context1);
	ReleasePtr(context);
	ReleasePtr(device);
	available = false;
}
