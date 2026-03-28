#include "stdafx.h"

#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)

#include "../Reimpl/BaseCompositor.h"
#include "dx11compositor.h"


#include "../Misc/Config.h"
#include "../Misc/xr_ext.h"

#include <cmath>
#include <d3dcompiler.h> // For compiling shaders! D3DCompile
#include <string>


#pragma comment(lib, "d3dcompiler.lib")

// AMD FidelityFX FSR 1.0 — CPU-side constant setup functions (FsrEasuCon, FsrRcasCon)
#define A_CPU
#include "fsr/ffx_a.h"
#include "fsr/ffx_fsr1.h"
#undef A_CPU

#ifdef OC_HAS_FSR3
#include "Fsr3Upscaler.h"
#endif

#ifdef OC_HAS_DLSS
#include "DlssUpscaler.h"
#endif

#include "../../DrvOpenXR/ASWProvider.h"
#include "../../DrvOpenXR/SpaceWarpProvider.h"

// ============================================================================
// SKSE Render Target Bridge — shared memory for motion vectors + depth
// ============================================================================
#pragma pack(push, 1)
struct OCRenderTargetBridge {
	static constexpr uint32_t MAGIC = 0x56544F4D; // 'MOTV'
	static constexpr uint32_t VERSION = 1;

	uint32_t magic;
	uint32_t version;
	uint32_t status; // 0=not ready, 1=ready, 2=error

	uint64_t mvTexture; // ID3D11Texture2D*
	uint64_t mvSRV; // ID3D11ShaderResourceView*
	uint64_t mvUAV; // ID3D11UnorderedAccessView*

	uint64_t depthTexture; // ID3D11Texture2D*
	uint64_t depthSRV; // ID3D11ShaderResourceView*

	uint64_t d3dDevice; // ID3D11Device*
	uint64_t d3dContext; // ID3D11DeviceContext*

	// Camera data for locomotion-aware motion vectors (added v1.1)
	uint64_t worldToCamPtr; // float* → NiCamera::worldToCam[0][0] (row-major 4x4, 64 bytes)
	uint64_t playerPosPtr; // float* → PlayerCamera::pos.x (3 floats: x, y, z)
	uint64_t playerYawPtr; // float* → PlayerCamera::yaw (1 float, radians)
	uint8_t isMainMenu; // 1 = main menu active, 0 = gameplay
	uint8_t isLoadingScreen; // 1 = loading screen active, 0 = gameplay
	uint8_t _pad1[6]; // padding to align next uint64_t
	uint64_t viewFrustumPtr; // float* → NiFrustum (L,R,T,B,Near,Far + bool ortho)

	// RendererShadowState base address — compositor reads VP matrices at known offsets
	uint64_t rssBasePtr; // uintptr_t → BSGraphics::RendererShadowState singleton

	// Actor position for stick locomotion correction (moves only with stick, not head tracking)
	uint64_t actorPosPtr; // float* → PlayerCharacter::data.location.x (NiPoint3: x, y, z)
	uint64_t actorYawPtr; // float* → PlayerCharacter::data.angle.z (actor heading, radians)

	// Camera world position — includes actorPos + eye height + walk-cycle bob + HMD tracking
	uint64_t cameraPosPtr; // float* → NiCamera::world.translate.x (NiPoint3: x, y, z)

	// Actor MV data — pointers to NiAVObject root nodes for nearby actors.
	// OC reads world/previousWorld transforms directly via known offsets each frame.
	// NiAVObject offsets (VR): world.translate=+0xA0, previousWorld.translate=+0xD4,
	// worldBound.center=+0xE4, worldBound.radius=+0xF0
	static constexpr uint32_t MAX_ACTOR_MV = 32;
	uint32_t actorMvCount; // Number of valid entries
	uint32_t actorMvRefreshSeq; // Incremented on re-enumeration (SKSE writes)
	uint64_t actorMvRootPtrs[MAX_ACTOR_MV]; // NiAVObject* root node pointers
	uint32_t actorMvRequestRefresh; // OC sets to 1 to request SKSE re-enumerate
	uint32_t _padActorMv;
};
#pragma pack(pop)

static HANDLE s_hBridgeMap = nullptr;
static OCRenderTargetBridge* s_pBridge = nullptr;
static bool s_bridgeTried = false;
static void OpenRenderTargetBridge()
{
	if (s_pBridge)
		return;

	// Retry every ~2 seconds (assuming ~90fps, every 180 frames)
	static int retryCounter = 0;
	if (s_bridgeTried && (++retryCounter % 180) != 0)
		return;
	s_bridgeTried = true;

	s_hBridgeMap = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE,
	    L"Local\\OpenCompositeRenderTargets");
	if (!s_hBridgeMap)
		return;

	s_pBridge = static_cast<OCRenderTargetBridge*>(
	    MapViewOfFile(s_hBridgeMap, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(OCRenderTargetBridge)));
	if (!s_pBridge) {
		CloseHandle(s_hBridgeMap);
		s_hBridgeMap = nullptr;
		return;
	}

	// Validate
	if (s_pBridge->magic != OCRenderTargetBridge::MAGIC || s_pBridge->version != OCRenderTargetBridge::VERSION) {
		OOVR_LOG("RT Bridge: Invalid magic/version — wrong SKSE plugin version?");
		UnmapViewOfFile(s_pBridge);
		s_pBridge = nullptr;
		CloseHandle(s_hBridgeMap);
		s_hBridgeMap = nullptr;
		return;
	}

	OOVR_LOG("RT Bridge: Connected to SKSE shared memory");
}

// OCU ASW — PC-side Asynchronous SpaceWarp (global g_aswProvider in ASWProvider.h)

#if defined(OC_HAS_FSR3) || defined(OC_HAS_DLSS)
#ifdef OC_HAS_FSR3
static Fsr3Upscaler* s_fsr3Upscaler = nullptr;
static std::chrono::steady_clock::time_point s_fsr3LastFrameTime;
static float s_fsr3CameraFovY = 1.57f; // Radians, updated from XR view each frame
#endif
static bool s_fsr3FirstDispatch = true;
static float s_fsr3RenderJitterX = 0.0f; // Jitter that was applied to current frame's rendering
static float s_fsr3RenderJitterY = 0.0f;
static uint32_t s_fsr3ViewportW = 0; // FSR3 output viewport (for crop when swapchain > output)
static uint32_t s_fsr3ViewportH = 0;

#if defined(OC_HAS_FSR3) || defined(OC_HAS_DLSS)
// ── Reactive mask resources (depth-edge detection for FSR3/DLSS ghosting reduction) ──
// Used as FSR3 reactive mask and DLSS pInBiasCurrentColorMask to reduce
// temporal accumulation at depth edges (foliage silhouettes, thin geometry).
static ID3D11ComputeShader* s_reactiveMaskCS = nullptr;
static ID3D11Texture2D* s_reactiveMaskTex = nullptr;
static ID3D11UnorderedAccessView* s_reactiveMaskUAV = nullptr;
static ID3D11ShaderResourceView* s_reactiveMaskDepthSRV = nullptr;
static ID3D11Texture2D* s_reactiveMaskDepthSRVTex = nullptr;
static uint32_t s_reactiveMaskW = 0;
static uint32_t s_reactiveMaskH = 0;
#endif

// ── Depth extraction resources ──
// Skyrim VR's main depth-stencil is R24G8_TYPELESS (24-bit depth + 8-bit stencil).
// CopySubresourceRegion from R24G8 → R32F silently fails (different format families).
// We use a compute shader to read depth via R24_UNORM_X8_TYPELESS SRV → write R32_FLOAT.
static ID3D11ComputeShader* s_depthExtractCS = nullptr;
static ID3D11Texture2D* s_depthR32F = nullptr; // Full-size R32F copy of bridge depth
static ID3D11UnorderedAccessView* s_depthR32FUAV = nullptr;
static ID3D11ShaderResourceView* s_depthBridgeSRV = nullptr; // SRV on bridge depth (R24_UNORM_X8_TYPELESS)
static ID3D11Texture2D* s_depthBridgeSRVTex = nullptr; // Cached: which tex the SRV was created for
static uint32_t s_depthR32FWidth = 0;
static uint32_t s_depthR32FHeight = 0;

static constexpr char s_depthExtractHLSL[] = R"HLSL(
Texture2D<float>   DepthIn  : register(t0);  // R24_UNORM_X8_TYPELESS view of depth-stencil
RWTexture2D<float> DepthOut : register(u0);  // R32_FLOAT output

[numthreads(8, 8, 1)]
void CS_DepthExtract(uint3 id : SV_DispatchThreadID)
{
    uint w, h;
    DepthOut.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    DepthOut[id.xy] = DepthIn.Load(int3(id.xy, 0));
}
)HLSL";

#if defined(OC_HAS_FSR3) || defined(OC_HAS_DLSS)
// ── Reactive mask compute shader ──
// Depth-edge detection: adds reactiveness at object silhouettes (tree branches
// against sky) to reduce FSR3/DLSS temporal ghosting on thin geometry.
static constexpr char s_reactiveMaskHLSL[] = R"HLSL(
Texture2D<float>       DepthIn     : register(t0);
RWTexture2D<float>     ReactiveOut : register(u0);

cbuffer ReactiveCB : register(b0) {
    float baseReactiveness;  // Global minimum
    float edgeBoost;         // Extra reactiveness at depth edges
    float edgeThreshold;     // Depth diff below which edge = 0
    float edgeScale;         // Ramp speed for edge detection
    float depthFalloffStart; // Depth value where distance falloff begins (standard-Z)
    float depthFalloffEnd;   // Depth value where bias reaches zero
    float pad0, pad1;
};

[numthreads(8, 8, 1)]
void CS_ReactiveMask(uint3 id : SV_DispatchThreadID)
{
    uint w, h;
    ReactiveOut.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;

    float center = DepthIn.Load(int3(id.xy, 0));
    float maxDiff = 0.0;
    static const int2 offsets[8] = {
        int2(-1, 0), int2(1, 0), int2(0,-1), int2(0, 1),
        int2(-3, 0), int2(3, 0), int2(0,-3), int2(0, 3),
    };
    [unroll] for (int i = 0; i < 8; i++) {
        int2 coord = clamp(int2(id.xy) + offsets[i], int2(0,0), int2(w-1, h-1));
        maxDiff = max(maxDiff, abs(center - DepthIn.Load(int3(coord, 0))));
    }
    float edge = saturate((maxDiff - edgeThreshold) * edgeScale) * edgeBoost;

    // Distance falloff: reduce bias for distant pixels so the upscaler trusts
    // history more, counteracting jitter-induced wobble on mountains/landscapes.
    // Standard-Z: higher depth = farther. Falloff ramps from 1→0 between start..end.
    float distFade = (depthFalloffEnd > depthFalloffStart)
        ? 1.0 - saturate((center - depthFalloffStart) / (depthFalloffEnd - depthFalloffStart))
        : 1.0;

    ReactiveOut[id.xy] = min((baseReactiveness + edge) * distFade, 0.95);
}
)HLSL";
static ID3D11Buffer* s_reactiveMaskCB = nullptr;

#endif // OC_HAS_FSR3 || OC_HAS_DLSS (reactive mask)

// ── Camera MV resources (compute per-pixel camera motion from depth + pose deltas) ──
// Replaces Skyrim's zero-valued MVs for static geometry during character locomotion.
// Uses depth buffer + current/previous view-projection matrices to compute per-pixel
// screen-space motion in UV space — directly usable by FSR3 temporal upscaler.
static ID3D11ComputeShader* s_cameraMVCS = nullptr;
static ID3D11Texture2D* s_cameraMVTex = nullptr;
static ID3D11UnorderedAccessView* s_cameraMVUAV = nullptr;
static ID3D11ShaderResourceView* s_cameraMVDepthSRV = nullptr;
static ID3D11Texture2D* s_cameraMVDepthSRVTex = nullptr;
static ID3D11Buffer* s_cameraMVCB = nullptr;
static uint32_t s_cameraMVW = 0, s_cameraMVH = 0;

// Per-eye previous VP matrix for camera MV computation (column-major float[16])
static float s_prevVP[2][16] = {};
static bool s_hasPrevVP[2] = { false, false };

// Per-eye previous jitter (pixel-space, for computing jitter delta in camera MVs)
static float s_prevJitterX[2] = { 0.0f, 0.0f };
static float s_prevJitterY[2] = { 0.0f, 0.0f };

// Per-eye previous warp pose for computing warp-to-warp MVs (separate DLSS temporal history).
static XrPosef s_prevWarpPose[2] = {};
static bool s_hasPrevWarpPose[2] = { false, false };

// Per-eye warp DLSS jitter index (independent from game jitter)
static int s_warpJitterIndex[2] = { 0, 0 };

// Locomotion injection for camera MVs: world-space deltas.
// Skyrim uses camera-relative rendering (VP origin shifts with player each frame),
// so prevVP * inv(curVP) captures rotation but NOT camera translation.
// We inject the delta into curVP before computing clipToClip.
//
// Horizontal (dx, dy): from actorPos (PlayerCharacter::data.location) — captures
// stick locomotion. HMD horizontal tracking is small enough to ignore.
//
// Vertical (dz): from NiCamera::world.translate.z — the engine updates this each
// frame with the full camera world position: actorPos + eye height + walk-cycle
// camera bob + HMD physical tracking. Direct float read — no matrix extraction.
static float s_cmvPrevActorPos[3] = {};
static bool s_cmvHasPrevActorPos = false;
static float s_cmvPrevCamZ = 0.0f;  // Previous NiCamera::world.translate.z
static bool s_cmvHasPrevCamZ = false;
static float s_cmvLocoDx = 0.0f, s_cmvLocoDy = 0.0f, s_cmvLocoDz = 0.0f; // Shared: computed on eye 0, reused for eye 1

// Inject locomotion translation into a column-major VP matrix.
// adjustedVP = VP * T^{-1} where T = translation by (dx, dy, dz).
// In column-major column-vector convention, only column 3 changes:
//   VP[12+i] -= dx*VP[0+i] + dy*VP[4+i] + dz*VP[8+i]  for i in {0,1,2,3}
static void InjectLocoIntoVP(float vp[16], float dx, float dy, float dz)
{
	for (int i = 0; i < 4; i++) {
		vp[12 + i] -= dx * vp[0 + i] + dy * vp[4 + i] + dz * vp[8 + i];
	}
}

// Extract camera world position from NiCamera worldToCam (4x4 row-major with uniform scale).
// worldToCam = [s·R^T | -s·R^T·pos; row3]  where s = 1/worldScale (≈2 in Skyrim VR).
// pos[j] = -Σᵢ(M[i][j] · t[i]) / ||row0||²
static void ExtractCamPosFromW2C(const float* w, float outPos[3])
{
	// w is 16 floats row-major: w[row*4+col]
	float s2 = w[0] * w[0] + w[1] * w[1] + w[2] * w[2]; // ||row0||²
	if (s2 < 1e-6f) {
		outPos[0] = outPos[1] = outPos[2] = 0;
		return;
	}
	for (int j = 0; j < 3; j++) {
		// M^T column j dotted with translation column: Σᵢ M[i][j] * M[i][3]
		outPos[j] = -(w[0 * 4 + j] * w[3] + w[1 * 4 + j] * w[7] + w[2 * 4 + j] * w[11]) / s2;
	}
}

// Per-eye pose/fov captured in outer Invoke, consumed by camera MV in inner Invoke
static XrPosef s_fsr3EyePose[2] = {};
static XrFovf s_fsr3EyeFov[2] = {};

static constexpr char s_cameraMVHLSL[] = R"HLSL(
Texture2D<float>       DepthIn   : register(t0);
RWTexture2D<float2>    MVOut     : register(u0);
Texture2D<float2>      GameMVIn  : register(t1);

cbuffer CameraMVCB : register(b0) {
    column_major float4x4 clipToClip;        // prevVP * inv(curVP) WITH loco
    float2 renderSize;
    int2   depthOffset;
    float2 jitterDeltaUV;
    float2 currJitterUV;
    int2   gameMVOffset;
    uint   useGameMV;
    float  _pad;
};

[numthreads(8, 8, 1)]
void CS_CameraMV(uint3 id : SV_DispatchThreadID)
{
    uint w, h;
    MVOut.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;

    float depth = DepthIn.Load(int3(int2(id.xy) + depthOffset, 0));

    if (depth < 0.0001) {
        MVOut[id.xy] = float2(0, 0);
        return;
    }

    float2 uv = (float2(id.xy) + 0.5) / renderSize;
    float2 uvUnjittered = uv - currJitterUV;
    float2 ndc = float2(uvUnjittered.x * 2.0 - 1.0, 1.0 - uvUnjittered.y * 2.0);
    float4 clipPos = float4(ndc, depth, 1.0);

    // Full camera MV (rotation + head tracking + locomotion) — always correct for static world
    float4 prevClipFull = mul(clipToClip, clipPos);
    float2 prevUVFull = float2(prevClipFull.x / prevClipFull.w * 0.5 + 0.5,
                               0.5 - prevClipFull.y / prevClipFull.w * 0.5);
    float2 fullCameraMV = prevUVFull - uvUnjittered;

    float2 mv = fullCameraMV;

    if (useGameMV) {
        // Use game MVs directly (rotation + head tracking + NPC animation).
        // Locomotion will be injected in the dilation pass using foreground depth,
        // which correctly handles alpha-tested foliage (sky-depth pixels get
        // foreground depth from nearest neighbor → correct locomotion parallax).
        float2 gameMV = GameMVIn.Load(int3(int2(id.xy) + gameMVOffset, 0));
        mv = gameMV;
    }

    mv.x += jitterDeltaUV.x;
    mv.y -= jitterDeltaUV.y;

    MVOut[id.xy] = mv;
}
)HLSL";

// ── MV Dilation shader (3x3 closest-depth) ──
// Standard technique for temporal upscaling with alpha-tested geometry.
// For each pixel, find the neighbor in a 3x3 kernel with the CLOSEST depth
// (nearest to camera) and use its motion vector. This ensures:
// - Foliage pixels with sky depth get foreground MVs from nearby bark/leaf pixels
// - Object edges get foreground MVs (prevents background bleed-through in temporal reprojection)
// - Depth-independent motion (head rotation) is unaffected (all neighbors have similar MVs)
// - Depth-dependent motion (locomotion translation) is corrected for thin geometry
static constexpr char s_mvDilateHLSL[] = R"HLSL(
Texture2D<float2>      MVIn     : register(t0);
Texture2D<float>       DepthIn  : register(t1);
RWTexture2D<float2>    MVOut    : register(u0);

cbuffer DilateCB : register(b0) {
    int2   depthOffset;
    uint2  resolution;
};

[numthreads(8, 8, 1)]
void CS_MVDilate(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= resolution.x || id.y >= resolution.y) return;

    // Find the closest (nearest-to-camera) depth in a 5x5 neighborhood.
    // Wider kernel propagates foreground MVs further into sky-adjacent pixels,
    // reducing trailing edge ghosts during fast locomotion (FSR3 disocclusion).
    float bestDepth = 1.0;
    int2 bestCoord = int2(id.xy);

    [unroll] for (int dy = -2; dy <= 2; dy++) {
        [unroll] for (int dx = -2; dx <= 2; dx++) {
            int2 coord = clamp(int2(id.xy) + int2(dx, dy), int2(0,0), int2(resolution) - 1);
            float d = DepthIn.Load(int3(coord + depthOffset, 0));
            if (d < bestDepth) {
                bestDepth = d;
                bestCoord = coord;
            }
        }
    }

    // Use the MV from the closest-depth neighbor
    MVOut[id.xy] = MVIn.Load(int3(bestCoord, 0));
}
)HLSL";

static ID3D11ComputeShader* s_mvDilateCS = nullptr;
static ID3D11Texture2D* s_mvDilateTex = nullptr;         // Dilated MV output
static ID3D11UnorderedAccessView* s_mvDilateUAV = nullptr;
static ID3D11ShaderResourceView* s_mvDilateMVSRV = nullptr;   // SRV for undilated MVs (s_cameraMVTex)
static ID3D11ShaderResourceView* s_mvDilateDepthSRV = nullptr;
static ID3D11Texture2D* s_mvDilateDepthSRVTex = nullptr;
static ID3D11Buffer* s_mvDilateCB = nullptr;
static uint32_t s_mvDilateW = 0, s_mvDilateH = 0;

// Lazily compile the depth extraction CS and create output texture + UAV.
// Returns true if depth extraction is available.
static bool EnsureDepthExtractResources(ID3D11Device* device, uint32_t depthW, uint32_t depthH)
{
	// Compile CS once
	if (!s_depthExtractCS) {
		ID3DBlob* blob = nullptr;
		ID3DBlob* errors = nullptr;
		HRESULT hr = D3DCompile(s_depthExtractHLSL, sizeof(s_depthExtractHLSL) - 1,
		    "DepthExtract", nullptr, nullptr, "CS_DepthExtract", "cs_5_0", 0, 0, &blob, &errors);
		if (FAILED(hr)) {
			if (errors) {
				OOVR_LOGF("DepthExtract CS compile failed: %s", (char*)errors->GetBufferPointer());
				errors->Release();
			}
			return false;
		}
		if (errors)
			errors->Release();
		hr = device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &s_depthExtractCS);
		blob->Release();
		if (FAILED(hr)) {
			OOVR_LOGF("DepthExtract: CreateComputeShader failed (hr=0x%08X)", hr);
			return false;
		}
		OOVR_LOG("DepthExtract: Compute shader compiled OK");
	}

	// Create/recreate R32F output texture if dimensions changed
	if (!s_depthR32F || s_depthR32FWidth != depthW || s_depthR32FHeight != depthH) {
		if (s_depthR32FUAV) {
			s_depthR32FUAV->Release();
			s_depthR32FUAV = nullptr;
		}
		if (s_depthR32F) {
			s_depthR32F->Release();
			s_depthR32F = nullptr;
		}

		D3D11_TEXTURE2D_DESC td = {};
		td.Width = depthW;
		td.Height = depthH;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R32_FLOAT;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

		HRESULT hr = device->CreateTexture2D(&td, nullptr, &s_depthR32F);
		if (FAILED(hr)) {
			OOVR_LOGF("DepthExtract: CreateTexture2D(%ux%u R32F) failed (hr=0x%08X)", depthW, depthH, hr);
			return false;
		}

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
		uavDesc.Format = DXGI_FORMAT_R32_FLOAT;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		hr = device->CreateUnorderedAccessView(s_depthR32F, &uavDesc, &s_depthR32FUAV);
		if (FAILED(hr)) {
			OOVR_LOGF("DepthExtract: CreateUAV failed (hr=0x%08X)", hr);
			s_depthR32F->Release();
			s_depthR32F = nullptr;
			return false;
		}

		s_depthR32FWidth = depthW;
		s_depthR32FHeight = depthH;
		OOVR_LOGF("DepthExtract: R32F output %ux%u created", depthW, depthH);
	}

	return true;
}

// Create an SRV on the bridge depth texture with R24_UNORM_X8_TYPELESS format.
// The SRV is cached and recreated when the bridge texture pointer changes.
static ID3D11ShaderResourceView* GetOrCreateDepthSRV(ID3D11Device* device, ID3D11Texture2D* depthTex,
    DXGI_FORMAT depthFmt, uint32_t depthW, uint32_t depthH)
{
	if (s_depthBridgeSRVTex == depthTex && s_depthBridgeSRV)
		return s_depthBridgeSRV;

	// Texture changed — release old SRV
	if (s_depthBridgeSRV) {
		s_depthBridgeSRV->Release();
		s_depthBridgeSRV = nullptr;
	}
	s_depthBridgeSRVTex = nullptr;

	// Determine the SRV format based on the depth texture format
	DXGI_FORMAT srvFmt = DXGI_FORMAT_UNKNOWN;
	if (depthFmt == DXGI_FORMAT_R24G8_TYPELESS || depthFmt == DXGI_FORMAT_D24_UNORM_S8_UINT)
		srvFmt = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
	else if (depthFmt == DXGI_FORMAT_R32G8X24_TYPELESS || depthFmt == DXGI_FORMAT_D32_FLOAT_S8X24_UINT)
		srvFmt = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
	else if (depthFmt == DXGI_FORMAT_R32_TYPELESS || depthFmt == DXGI_FORMAT_D32_FLOAT)
		srvFmt = DXGI_FORMAT_R32_FLOAT;
	else {
		OOVR_LOGF("DepthExtract: Unsupported depth format %u — cannot create SRV", depthFmt);
		return nullptr;
	}

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Format = srvFmt;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = 1;

	HRESULT hr = device->CreateShaderResourceView(depthTex, &srvDesc, &s_depthBridgeSRV);
	if (FAILED(hr)) {
		OOVR_LOGF("DepthExtract: CreateSRV(fmt=%u on depth fmt=%u) failed (hr=0x%08X) — trying bridge SRV",
		    srvFmt, depthFmt, hr);
		// Fall back: use the SKSE bridge's pre-created depthSRV
		if (s_pBridge && s_pBridge->depthSRV) {
			s_depthBridgeSRV = reinterpret_cast<ID3D11ShaderResourceView*>(s_pBridge->depthSRV);
			// Note: NOT calling AddRef — bridge SRV is owned by the game
			s_depthBridgeSRVTex = depthTex;
			OOVR_LOG("DepthExtract: Using bridge depthSRV as fallback");
			return s_depthBridgeSRV;
		}
		return nullptr;
	}

	s_depthBridgeSRVTex = depthTex;
	OOVR_LOGF("DepthExtract: SRV created (srvFmt=%u depthFmt=%u %ux%u)", srvFmt, depthFmt, depthW, depthH);
	return s_depthBridgeSRV;
}

// Run depth extraction CS: bridge depth → R32F output texture
static bool ExtractDepthToR32F(ID3D11DeviceContext* context, ID3D11ShaderResourceView* depthSRV,
    uint32_t width, uint32_t height)
{
	// Save current CS state
	ID3D11ComputeShader* oldCS = nullptr;
	ID3D11ShaderResourceView* oldSRV = nullptr;
	ID3D11UnorderedAccessView* oldUAV = nullptr;
	context->CSGetShader(&oldCS, nullptr, nullptr);
	context->CSGetShaderResources(0, 1, &oldSRV);
	context->CSGetUnorderedAccessViews(0, 1, &oldUAV);

	// Set shader + resources
	context->CSSetShader(s_depthExtractCS, nullptr, 0);
	context->CSSetShaderResources(0, 1, &depthSRV);
	context->CSSetUnorderedAccessViews(0, 1, &s_depthR32FUAV, nullptr);

	// Dispatch
	uint32_t groupsX = (width + 7) / 8;
	uint32_t groupsY = (height + 7) / 8;
	context->Dispatch(groupsX, groupsY, 1);

	// Restore CS state
	context->CSSetShader(oldCS, nullptr, 0);
	context->CSSetShaderResources(0, 1, &oldSRV);
	context->CSSetUnorderedAccessViews(0, 1, &oldUAV, nullptr);
	if (oldCS)
		oldCS->Release();
	if (oldSRV)
		oldSRV->Release();
	if (oldUAV)
		oldUAV->Release();

	return true;
}

// ── Reactive mask generation ──

static bool EnsureReactiveMaskResources(ID3D11Device* device, uint32_t w, uint32_t h)
{
	if (!s_reactiveMaskCS) {
		ID3DBlob* blob = nullptr;
		ID3DBlob* errors = nullptr;
		HRESULT hr = D3DCompile(s_reactiveMaskHLSL, sizeof(s_reactiveMaskHLSL) - 1,
		    "ReactiveMask", nullptr, nullptr, "CS_ReactiveMask", "cs_5_0", 0, 0, &blob, &errors);
		if (FAILED(hr)) {
			if (errors) {
				OOVR_LOGF("ReactiveMask CS compile failed: %s", (char*)errors->GetBufferPointer());
				errors->Release();
			}
			return false;
		}
		if (errors)
			errors->Release();
		hr = device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &s_reactiveMaskCS);
		blob->Release();
		if (FAILED(hr)) {
			OOVR_LOGF("ReactiveMask: CreateComputeShader failed (hr=0x%08X)", hr);
			return false;
		}
	}

	if (!s_reactiveMaskTex || s_reactiveMaskW != w || s_reactiveMaskH != h) {
		if (s_reactiveMaskUAV) {
			s_reactiveMaskUAV->Release();
			s_reactiveMaskUAV = nullptr;
		}
		if (s_reactiveMaskTex) {
			s_reactiveMaskTex->Release();
			s_reactiveMaskTex = nullptr;
		}

		D3D11_TEXTURE2D_DESC td = {};
		td.Width = w;
		td.Height = h;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R8_UNORM;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		HRESULT hr = device->CreateTexture2D(&td, nullptr, &s_reactiveMaskTex);
		if (FAILED(hr)) {
			OOVR_LOGF("ReactiveMask: CreateTexture2D(%ux%u) failed (hr=0x%08X)", w, h, hr);
			return false;
		}

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
		uavDesc.Format = DXGI_FORMAT_R8_UNORM;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		hr = device->CreateUnorderedAccessView(s_reactiveMaskTex, &uavDesc, &s_reactiveMaskUAV);
		if (FAILED(hr)) {
			s_reactiveMaskTex->Release();
			s_reactiveMaskTex = nullptr;
			return false;
		}
		s_reactiveMaskW = w;
		s_reactiveMaskH = h;
	}

	if (!s_reactiveMaskCB) {
		D3D11_BUFFER_DESC bd = {};
		bd.ByteWidth = 32;
		bd.Usage = D3D11_USAGE_DYNAMIC;
		bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		HRESULT hr = device->CreateBuffer(&bd, nullptr, &s_reactiveMaskCB);
		if (FAILED(hr))
			return false;
	}
	return true;
}

static ID3D11ShaderResourceView* GetOrCreateReactiveMaskDepthSRV(ID3D11Device* device, ID3D11Texture2D* depthR32F)
{
	if (s_reactiveMaskDepthSRVTex == depthR32F && s_reactiveMaskDepthSRV)
		return s_reactiveMaskDepthSRV;
	if (s_reactiveMaskDepthSRV) {
		s_reactiveMaskDepthSRV->Release();
		s_reactiveMaskDepthSRV = nullptr;
	}
	s_reactiveMaskDepthSRVTex = nullptr;

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = 1;
	HRESULT hr = device->CreateShaderResourceView(depthR32F, &srvDesc, &s_reactiveMaskDepthSRV);
	if (FAILED(hr))
		return nullptr;
	s_reactiveMaskDepthSRVTex = depthR32F;
	return s_reactiveMaskDepthSRV;
}

static bool GenerateReactiveMask(ID3D11DeviceContext* context, ID3D11ShaderResourceView* depthSRV,
    uint32_t width, uint32_t height,
    float baseReactiveness, float edgeBoost, float edgeThreshold, float edgeScale,
    float depthFalloffStart = 0.0f, float depthFalloffEnd = 0.0f)
{
	D3D11_MAPPED_SUBRESOURCE mapped;
	if (SUCCEEDED(context->Map(s_reactiveMaskCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
		float* cb = (float*)mapped.pData;
		cb[0] = baseReactiveness;
		cb[1] = edgeBoost;
		cb[2] = edgeThreshold;
		cb[3] = edgeScale;
		cb[4] = depthFalloffStart;
		cb[5] = depthFalloffEnd;
		cb[6] = 0.0f;
		cb[7] = 0.0f;
		context->Unmap(s_reactiveMaskCB, 0);
	}

	ID3D11ComputeShader* oldCS = nullptr;
	ID3D11ShaderResourceView* oldSRV = nullptr;
	ID3D11UnorderedAccessView* oldUAV = nullptr;
	ID3D11Buffer* oldCB = nullptr;
	context->CSGetShader(&oldCS, nullptr, nullptr);
	context->CSGetShaderResources(0, 1, &oldSRV);
	context->CSGetUnorderedAccessViews(0, 1, &oldUAV);
	context->CSGetConstantBuffers(0, 1, &oldCB);

	context->CSSetShader(s_reactiveMaskCS, nullptr, 0);
	context->CSSetShaderResources(0, 1, &depthSRV);
	context->CSSetUnorderedAccessViews(0, 1, &s_reactiveMaskUAV, nullptr);
	context->CSSetConstantBuffers(0, 1, &s_reactiveMaskCB);
	context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);

	context->CSSetShader(oldCS, nullptr, 0);
	context->CSSetShaderResources(0, 1, &oldSRV);
	context->CSSetUnorderedAccessViews(0, 1, &oldUAV, nullptr);
	context->CSSetConstantBuffers(0, 1, &oldCB);
	if (oldCS)
		oldCS->Release();
	if (oldSRV)
		oldSRV->Release();
	if (oldUAV)
		oldUAV->Release();
	if (oldCB)
		oldCB->Release();
	return true;
}

// ── Camera MV matrix helpers + generation ──
// All matrices are column-major float[16]: m[col*4 + row]

// Double-precision 4x4 matrix multiply: out = A * B (column-major)
// Used on the CPU to compute clip-to-clip reprojection matrix without float32 cancellation.
static void Mat4Mul_d(const double A[16], const double B[16], double out[16])
{
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++) {
			double s = 0;
			for (int k = 0; k < 4; k++)
				s += A[k * 4 + r] * B[c * 4 + k];
			out[c * 4 + r] = s;
		}
}

// Double-precision 4x4 matrix inverse (column-major, cofactor method)
static bool Mat4Inv_d(const double m[16], double out[16])
{
	double a00 = m[0], a01 = m[1], a02 = m[2], a03 = m[3];
	double a10 = m[4], a11 = m[5], a12 = m[6], a13 = m[7];
	double a20 = m[8], a21 = m[9], a22 = m[10], a23 = m[11];
	double a30 = m[12], a31 = m[13], a32 = m[14], a33 = m[15];
	double b00 = a00 * a11 - a01 * a10, b01 = a00 * a12 - a02 * a10, b02 = a00 * a13 - a03 * a10;
	double b03 = a01 * a12 - a02 * a11, b04 = a01 * a13 - a03 * a11, b05 = a02 * a13 - a03 * a12;
	double b06 = a20 * a31 - a21 * a30, b07 = a20 * a32 - a22 * a30, b08 = a20 * a33 - a23 * a30;
	double b09 = a21 * a32 - a22 * a31, b10 = a21 * a33 - a23 * a31, b11 = a22 * a33 - a23 * a32;
	double det = b00 * b11 - b01 * b10 + b02 * b09 + b03 * b08 - b04 * b07 + b05 * b06;
	if (fabs(det) < 1e-20)
		return false;
	double id = 1.0 / det;
	out[0] = (a11 * b11 - a12 * b10 + a13 * b09) * id;
	out[1] = (-a01 * b11 + a02 * b10 - a03 * b09) * id;
	out[2] = (a31 * b05 - a32 * b04 + a33 * b03) * id;
	out[3] = (-a21 * b05 + a22 * b04 - a23 * b03) * id;
	out[4] = (-a10 * b11 + a12 * b08 - a13 * b07) * id;
	out[5] = (a00 * b11 - a02 * b08 + a03 * b07) * id;
	out[6] = (-a30 * b05 + a32 * b02 - a33 * b01) * id;
	out[7] = (a20 * b05 - a22 * b02 + a23 * b01) * id;
	out[8] = (a10 * b10 - a11 * b08 + a13 * b06) * id;
	out[9] = (-a00 * b10 + a01 * b08 - a03 * b06) * id;
	out[10] = (a30 * b04 - a31 * b02 + a33 * b00) * id;
	out[11] = (-a20 * b04 + a21 * b02 - a23 * b00) * id;
	out[12] = (-a10 * b09 + a11 * b07 - a12 * b06) * id;
	out[13] = (a00 * b09 - a01 * b07 + a02 * b06) * id;
	out[14] = (-a30 * b03 + a31 * b01 - a32 * b00) * id;
	out[15] = (a20 * b03 - a21 * b01 + a22 * b00) * id;
	return true;
}

// Compute clip-to-clip reprojection matrix M = prevVP * inv(curVP) in double precision.
// The resulting float32 matrix M ≈ Identity (for small frame-to-frame changes), so the
// shader's float32 multiply M * clipPos has no catastrophic cancellation.
static bool ComputeClipToClip(const float curVP[16], const float prevVP[16], float out[16])
{
	double curVP_d[16], invCurVP_d[16], prevVP_d[16], M_d[16];
	for (int i = 0; i < 16; i++) {
		curVP_d[i] = (double)curVP[i];
		prevVP_d[i] = (double)prevVP[i];
	}
	if (!Mat4Inv_d(curVP_d, invCurVP_d))
		return false;
	Mat4Mul_d(prevVP_d, invCurVP_d, M_d);
	for (int i = 0; i < 16; i++)
		out[i] = (float)M_d[i];
	return true;
}

static bool EnsureCameraMVResources(ID3D11Device* device, uint32_t w, uint32_t h)
{
	// Compile CS once
	if (!s_cameraMVCS) {
		ID3DBlob* blob = nullptr;
		ID3DBlob* errors = nullptr;
		HRESULT hr = D3DCompile(s_cameraMVHLSL, sizeof(s_cameraMVHLSL) - 1,
		    "CameraMV", nullptr, nullptr, "CS_CameraMV", "cs_5_0", 0, 0, &blob, &errors);
		if (FAILED(hr)) {
			if (errors) {
				OOVR_LOGF("CameraMV CS compile failed: %s", (char*)errors->GetBufferPointer());
				errors->Release();
			}
			return false;
		}
		if (errors)
			errors->Release();
		hr = device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &s_cameraMVCS);
		blob->Release();
		if (FAILED(hr)) {
			OOVR_LOGF("CameraMV: CreateComputeShader failed (hr=0x%08X)", hr);
			return false;
		}
		OOVR_LOG("CameraMV: Compute shader compiled OK");
	}

	// Create/recreate R16G16_FLOAT output texture if dimensions changed
	if (!s_cameraMVTex || s_cameraMVW != w || s_cameraMVH != h) {
		if (s_cameraMVUAV) {
			s_cameraMVUAV->Release();
			s_cameraMVUAV = nullptr;
		}
		if (s_cameraMVTex) {
			s_cameraMVTex->Release();
			s_cameraMVTex = nullptr;
		}

		D3D11_TEXTURE2D_DESC td = {};
		td.Width = w;
		td.Height = h;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R16G16_FLOAT;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		HRESULT hr = device->CreateTexture2D(&td, nullptr, &s_cameraMVTex);
		if (FAILED(hr)) {
			OOVR_LOGF("CameraMV: CreateTexture2D(%ux%u) failed (hr=0x%08X)", w, h, hr);
			return false;
		}

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
		uavDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		hr = device->CreateUnorderedAccessView(s_cameraMVTex, &uavDesc, &s_cameraMVUAV);
		if (FAILED(hr)) {
			OOVR_LOGF("CameraMV: CreateUAV failed (hr=0x%08X)", hr);
			s_cameraMVTex->Release();
			s_cameraMVTex = nullptr;
			return false;
		}
		s_cameraMVW = w;
		s_cameraMVH = h;
		OOVR_LOGF("CameraMV: R16G16_FLOAT output %ux%u created", w, h);
	}

	// Create constant buffer once (112 bytes = 7 * 16)
	if (!s_cameraMVCB) {
		D3D11_BUFFER_DESC bd = {};
		bd.ByteWidth = 112; // clipToClip(64) + renderSize(8) + depthOffset(8) + jitterDelta(8) + currJitter(8) + gameMVOffset(8) + useGameMV(4) + pad(4) = 112
		bd.Usage = D3D11_USAGE_DYNAMIC;
		bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		HRESULT hr = device->CreateBuffer(&bd, nullptr, &s_cameraMVCB);
		if (FAILED(hr)) {
			OOVR_LOGF("CameraMV: CreateBuffer(CB) failed (hr=0x%08X)", hr);
			return false;
		}
	}
	return true;
}

static ID3D11ShaderResourceView* GetOrCreateCameraMVDepthSRV(ID3D11Device* device, ID3D11Texture2D* depthR32F)
{
	if (s_cameraMVDepthSRVTex == depthR32F && s_cameraMVDepthSRV)
		return s_cameraMVDepthSRV;
	if (s_cameraMVDepthSRV) {
		s_cameraMVDepthSRV->Release();
		s_cameraMVDepthSRV = nullptr;
	}
	s_cameraMVDepthSRVTex = nullptr;
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = 1;
	HRESULT hr = device->CreateShaderResourceView(depthR32F, &srvDesc, &s_cameraMVDepthSRV);
	if (FAILED(hr)) {
		OOVR_LOGF("CameraMV: CreateSRV failed (hr=0x%08X)", hr);
		return nullptr;
	}
	s_cameraMVDepthSRVTex = depthR32F;
	return s_cameraMVDepthSRV;
}

// Generate camera-derived motion vectors from depth + clip-to-clip reprojection matrix.
static bool GenerateCameraMVs(ID3D11DeviceContext* context, ID3D11ShaderResourceView* depthSRV,
    uint32_t outputW, uint32_t outputH,
    const float clipToClip[16],
    int depthOffsetX, int depthOffsetY, float jitterDeltaUVx, float jitterDeltaUVy,
    float currJitterUVx, float currJitterUVy,
    ID3D11ShaderResourceView* gameMVSRV, int gameMVOffsetX, int gameMVOffsetY,
    bool useGameMV)
{
	// Update constant buffer: clipToClip + params = 112 bytes
	D3D11_MAPPED_SUBRESOURCE mapped;
	if (SUCCEEDED(context->Map(s_cameraMVCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
		float* cb = (float*)mapped.pData;
		int32_t* cbi = (int32_t*)mapped.pData;
		uint32_t* cbu = (uint32_t*)mapped.pData;
		memcpy(cb, clipToClip, 64);           // offset 0: clipToClip
		cb[16] = (float)outputW;              // offset 64: renderSize
		cb[17] = (float)outputH;
		cbi[18] = depthOffsetX;               // offset 72: depthOffset
		cbi[19] = depthOffsetY;
		cb[20] = jitterDeltaUVx;              // offset 80: jitterDeltaUV
		cb[21] = jitterDeltaUVy;
		cb[22] = currJitterUVx;               // offset 88: currJitterUV
		cb[23] = currJitterUVy;
		cbi[24] = gameMVOffsetX;              // offset 96: gameMVOffset
		cbi[25] = gameMVOffsetY;
		cbu[26] = useGameMV ? 1u : 0u;       // offset 104: useGameMV
		cbu[27] = 0u;                         // offset 108: pad
		context->Unmap(s_cameraMVCB, 0);
	}

	// Save current CS state
	ID3D11ComputeShader* oldCS = nullptr;
	ID3D11ShaderResourceView* oldSRVs[2] = { nullptr, nullptr };
	ID3D11UnorderedAccessView* oldUAV = nullptr;
	ID3D11Buffer* oldCB = nullptr;
	context->CSGetShader(&oldCS, nullptr, nullptr);
	context->CSGetShaderResources(0, 2, oldSRVs);
	context->CSGetUnorderedAccessViews(0, 1, &oldUAV);
	context->CSGetConstantBuffers(0, 1, &oldCB);

	context->CSSetShader(s_cameraMVCS, nullptr, 0);
	ID3D11ShaderResourceView* srvs[2] = { depthSRV, gameMVSRV };
	context->CSSetShaderResources(0, 2, srvs);
	context->CSSetUnorderedAccessViews(0, 1, &s_cameraMVUAV, nullptr);
	context->CSSetConstantBuffers(0, 1, &s_cameraMVCB);
	context->Dispatch((outputW + 7) / 8, (outputH + 7) / 8, 1);

	// Restore CS state
	context->CSSetShader(oldCS, nullptr, 0);
	context->CSSetShaderResources(0, 2, oldSRVs);
	context->CSSetUnorderedAccessViews(0, 1, &oldUAV, nullptr);
	context->CSSetConstantBuffers(0, 1, &oldCB);
	if (oldCS) oldCS->Release();
	if (oldSRVs[0]) oldSRVs[0]->Release();
	if (oldSRVs[1]) oldSRVs[1]->Release();
	if (oldUAV) oldUAV->Release();
	if (oldCB) oldCB->Release();
	return true;
}

// ── MV Dilation (3x3 closest-depth) ──

static bool EnsureMVDilateResources(ID3D11Device* device, uint32_t w, uint32_t h)
{
	if (!s_mvDilateCS) {
		ID3DBlob* blob = nullptr;
		ID3DBlob* errors = nullptr;
		HRESULT hr = D3DCompile(s_mvDilateHLSL, sizeof(s_mvDilateHLSL) - 1,
		    "MVDilate", nullptr, nullptr, "CS_MVDilate", "cs_5_0", 0, 0, &blob, &errors);
		if (FAILED(hr)) {
			if (errors) {
				OOVR_LOGF("MVDilate CS compile failed: %s", (char*)errors->GetBufferPointer());
				errors->Release();
			}
			return false;
		}
		if (errors) errors->Release();
		hr = device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &s_mvDilateCS);
		blob->Release();
		if (FAILED(hr)) return false;
	}

	if (!s_mvDilateTex || s_mvDilateW != w || s_mvDilateH != h) {
		if (s_mvDilateUAV) { s_mvDilateUAV->Release(); s_mvDilateUAV = nullptr; }
		if (s_mvDilateMVSRV) { s_mvDilateMVSRV->Release(); s_mvDilateMVSRV = nullptr; }
		if (s_mvDilateTex) { s_mvDilateTex->Release(); s_mvDilateTex = nullptr; }

		D3D11_TEXTURE2D_DESC td = {};
		td.Width = w; td.Height = h;
		td.MipLevels = 1; td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R16G16_FLOAT;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		if (FAILED(device->CreateTexture2D(&td, nullptr, &s_mvDilateTex)))
			return false;

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
		uavDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		if (FAILED(device->CreateUnorderedAccessView(s_mvDilateTex, &uavDesc, &s_mvDilateUAV))) {
			s_mvDilateTex->Release(); s_mvDilateTex = nullptr;
			return false;
		}

		s_mvDilateW = w; s_mvDilateH = h;
	}

	if (!s_mvDilateCB) {
		D3D11_BUFFER_DESC bd = {};
		bd.ByteWidth = 16; // depthOffset(8) + resolution(8)
		bd.Usage = D3D11_USAGE_DYNAMIC;
		bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		if (FAILED(device->CreateBuffer(&bd, nullptr, &s_mvDilateCB)))
			return false;
	}

	return true;
}

// Create SRV for undilated camera MVs (s_cameraMVTex) — needed as input to dilation
static ID3D11ShaderResourceView* GetOrCreateMVDilateMVSRV(ID3D11Device* device, ID3D11Texture2D* mvTex)
{
	// Recreate if source texture changed
	if (s_mvDilateMVSRV) {
		ID3D11Resource* res = nullptr;
		s_mvDilateMVSRV->GetResource(&res);
		bool same = (res == mvTex);
		if (res) res->Release();
		if (same) return s_mvDilateMVSRV;
		s_mvDilateMVSRV->Release();
		s_mvDilateMVSRV = nullptr;
	}

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = 1;
	if (FAILED(device->CreateShaderResourceView(mvTex, &srvDesc, &s_mvDilateMVSRV)))
		return nullptr;
	return s_mvDilateMVSRV;
}

static ID3D11ShaderResourceView* GetOrCreateMVDilateDepthSRV(ID3D11Device* device, ID3D11Texture2D* depthR32F)
{
	if (s_mvDilateDepthSRVTex == depthR32F && s_mvDilateDepthSRV)
		return s_mvDilateDepthSRV;
	if (s_mvDilateDepthSRV) { s_mvDilateDepthSRV->Release(); s_mvDilateDepthSRV = nullptr; }
	s_mvDilateDepthSRVTex = nullptr;

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = 1;
	if (FAILED(device->CreateShaderResourceView(depthR32F, &srvDesc, &s_mvDilateDepthSRV)))
		return nullptr;
	s_mvDilateDepthSRVTex = depthR32F;
	return s_mvDilateDepthSRV;
}

static bool DilateCameraMVs(ID3D11DeviceContext* context,
    ID3D11ShaderResourceView* mvSRV, ID3D11ShaderResourceView* depthSRV,
    uint32_t w, uint32_t h, int depthOffsetX, int depthOffsetY)
{
	// Update constant buffer
	D3D11_MAPPED_SUBRESOURCE mapped;
	if (SUCCEEDED(context->Map(s_mvDilateCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
		int32_t* cb = (int32_t*)mapped.pData;
		cb[0] = depthOffsetX;
		cb[1] = depthOffsetY;
		cb[2] = (int32_t)w;
		cb[3] = (int32_t)h;
		context->Unmap(s_mvDilateCB, 0);
	}

	// Save state
	ID3D11ComputeShader* oldCS = nullptr;
	ID3D11ShaderResourceView* oldSRV[2] = {};
	ID3D11UnorderedAccessView* oldUAV = nullptr;
	ID3D11Buffer* oldCB = nullptr;
	context->CSGetShader(&oldCS, nullptr, nullptr);
	context->CSGetShaderResources(0, 2, oldSRV);
	context->CSGetUnorderedAccessViews(0, 1, &oldUAV);
	context->CSGetConstantBuffers(0, 1, &oldCB);

	// Dispatch
	context->CSSetShader(s_mvDilateCS, nullptr, 0);
	ID3D11ShaderResourceView* srvs[2] = { mvSRV, depthSRV };
	context->CSSetShaderResources(0, 2, srvs);
	context->CSSetUnorderedAccessViews(0, 1, &s_mvDilateUAV, nullptr);
	context->CSSetConstantBuffers(0, 1, &s_mvDilateCB);
	context->Dispatch((w + 7) / 8, (h + 7) / 8, 1);

	// Restore state
	context->CSSetShader(oldCS, nullptr, 0);
	context->CSSetShaderResources(0, 2, oldSRV);
	context->CSSetUnorderedAccessViews(0, 1, &oldUAV, nullptr);
	context->CSSetConstantBuffers(0, 1, &oldCB);
	if (oldCS) oldCS->Release();
	if (oldSRV[0]) oldSRV[0]->Release();
	if (oldSRV[1]) oldSRV[1]->Release();
	if (oldUAV) oldUAV->Release();
	if (oldCB) oldCB->Release();
	return true;
}

#ifdef OC_HAS_FSR3
// ── Debug visualization shaders ──
// Mode 3: Depth → grayscale (reversed-Z: near=1=white, far=0=black)
// Mode 4: Motion vectors → RG color (red=horizontal, green=vertical, abs scaled)
static ID3D11PixelShader* s_debugDepthPS = nullptr;
static ID3D11PixelShader* s_debugMvPS = nullptr;

// Shared constant buffer layout for debug viz: maps screen UV to a sub-region of the source texture
static constexpr char s_debugCBHLSL[] = R"HLSL(
cbuffer DebugCB : register(b0) {
    float2 uvMin;   // top-left UV of the sub-region in the source texture
    float2 uvMax;   // bottom-right UV of the sub-region
};
)HLSL";

static constexpr char s_debugDepthHLSL[] = R"HLSL(
cbuffer DebugCB : register(b0) { float2 uvMin; float2 uvMax; };
Texture2D<float> DepthTex : register(t0);
struct VsOut { float4 pos : SV_POSITION; float2 tex : TEXCOORD0; };
float4 PS_DebugDepth(VsOut input) : SV_TARGET
{
    float2 sampleUV = uvMin + input.tex * (uvMax - uvMin);
    uint w, h;
    DepthTex.GetDimensions(w, h);
    float d = DepthTex.Load(int3(sampleUV * float2(w, h), 0));
    // Reversed-Z: 1=near, 0=far → display as-is (white=near, black=far)
    return float4(d, d, d, 1.0);
}
)HLSL";

static constexpr char s_debugMvHLSL[] = R"HLSL(
cbuffer DebugCB : register(b0) { float2 uvMin; float2 uvMax; };
Texture2D<float2> MVTex : register(t0);
struct VsOut { float4 pos : SV_POSITION; float2 tex : TEXCOORD0; };
float4 PS_DebugMV(VsOut input) : SV_TARGET
{
    float2 sampleUV = uvMin + input.tex * (uvMax - uvMin);
    uint w, h;
    MVTex.GetDimensions(w, h);
    float2 mv = MVTex.Load(int3(sampleUV * float2(w, h), 0));
    // Signed display: 0.5=zero, >0.5=positive (bright), <0.5=negative (dark)
    // Scale: UV-space values ~0.001-0.025 → amplify 20x around 0.5 midpoint
    float r = saturate(0.5 + mv.x * 20.0);
    float g = saturate(0.5 + mv.y * 20.0);
    // Blue channel: magnitude (helps see any non-zero MVs when standing still)
    float b = saturate(length(mv) * 100.0);
    return float4(r, g, b, 1.0);
}
)HLSL";

static ID3D11Buffer* s_debugCB = nullptr;

static void EnsureDebugShaders(ID3D11Device* device)
{
	auto compilePS = [&](const char* src, size_t len, const char* entry, ID3D11PixelShader** out) {
		if (*out)
			return;
		ID3DBlob* blob = nullptr;
		ID3DBlob* errors = nullptr;
		HRESULT hr = D3DCompile(src, len, entry, nullptr, nullptr, entry, "ps_5_0", 0, 0, &blob, &errors);
		if (FAILED(hr)) {
			if (errors) {
				OOVR_LOGF("Debug %s compile failed: %s", entry, (char*)errors->GetBufferPointer());
				errors->Release();
			}
			return;
		}
		if (errors)
			errors->Release();
		device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, out);
		blob->Release();
	};
	compilePS(s_debugDepthHLSL, sizeof(s_debugDepthHLSL) - 1, "PS_DebugDepth", &s_debugDepthPS);
	compilePS(s_debugMvHLSL, sizeof(s_debugMvHLSL) - 1, "PS_DebugMV", &s_debugMvPS);

	if (!s_debugCB) {
		D3D11_BUFFER_DESC cbd = {};
		cbd.ByteWidth = 16; // float2 uvMin + float2 uvMax = 4 floats = 16 bytes
		cbd.Usage = D3D11_USAGE_DYNAMIC;
		cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		device->CreateBuffer(&cbd, nullptr, &s_debugCB);
	}
}
#endif // OC_HAS_FSR3 (debug viz)

// Validate a raw texture pointer from the SKSE shared-memory bridge.
// The bridge stores void* pointers to game render targets WITHOUT AddRef —
// when the game releases the RT (VD session restart, resolution change, etc.),
// the NVIDIA driver decommits the backing pages. VirtualQuery detects this
// before we dereference the stale pointer and crash the driver.
static bool ValidateBridgeTexture(const void* ptr, const char* name)
{
	if (!ptr)
		return false;
	MEMORY_BASIC_INFORMATION mbi = {};
	if (!VirtualQuery(ptr, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) {
		static int count = 0;
		if (count++ < 10 || count % 500 == 0)
			OOVR_LOGF("FSR3: %s texture at %p is STALE (state=0x%lX) — skipping FSR3 dispatch",
			    name, ptr, mbi.State);
		return false;
	}
	return true;
}

// Safely call GetDesc on a bridge texture pointer, catching access violations
// from TOCTOU race conditions (pointer valid at VirtualQuery but freed before
// GetDesc). Must be in a separate function — __try/__except cannot coexist
// with C++ objects that have destructors (MSVC error C2712).
static bool SafeGetTextureDesc(ID3D11Texture2D* tex, D3D11_TEXTURE2D_DESC* outDesc)
{
	__try {
		tex->GetDesc(outDesc);
		return true;
	} __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
	        ? EXCEPTION_EXECUTE_HANDLER
	        : EXCEPTION_CONTINUE_SEARCH) {
		return false;
	}
}

// Safely call CopySubresourceRegion with a bridge texture as source.
// Closes the TOCTOU gap: texture can be freed between VirtualQuery/GetDesc
// and the actual copy. The D3D11 runtime dereferences the source texture
// internally, so a stale pointer causes an AV here too.
static bool SafeCopyFromBridgeTexture(ID3D11DeviceContext* ctx,
    ID3D11Resource* dst, UINT dstSub, UINT dstX, UINT dstY, UINT dstZ,
    ID3D11Resource* src, UINT srcSub, const D3D11_BOX* srcBox)
{
	__try {
		ctx->CopySubresourceRegion(dst, dstSub, dstX, dstY, dstZ, src, srcSub, srcBox);
		return true;
	} __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
	        ? EXCEPTION_EXECUTE_HANDLER
	        : EXCEPTION_CONTINUE_SEARCH) {
		static int count = 0;
		if (count++ < 10)
			OOVR_LOG("TOCTOU: Bridge texture freed during CopySubresourceRegion — skipping");
		return false;
	}
}

// Global jitter state — accessed by XrHMD.cpp for projection matrix injection
int g_fsr3FrameIndex = 0;
float g_fsr3JitterX = 0.0f;
float g_fsr3JitterY = 0.0f;
bool g_fsr3JitterEnabled = false;
int g_fsr3JitterPhaseCount = 0;
// Camera near/far — captured from game's GetProjectionMatrix in XrHMD.cpp
float g_fsr3CameraNear = 5.0f;
float g_fsr3CameraFar = 100000.0f;
#endif // defined(OC_HAS_FSR3) || defined(OC_HAS_DLSS)

#ifdef OC_HAS_DLSS
static DlssUpscaler* s_dlssUpscaler = nullptr;

/// Callback for ASWProvider::SubmitWarpedOutput — runs DLSS spatial upscaling on
/// the render-res warp output so every displayed frame goes through DLSS.
/// Uses separate warp NGX handles (indices 2-3) with reset=true to avoid
/// corrupting the game DLSS temporal accumulation history.
// ── Warp DLSS: VP matrix construction from OpenXR pose + FOV ──

// Build a column-major 4x4 view-projection matrix from XrPosef + XrFovf.
// Convention: clip = VP * v_world (column-major, column-vector).
// Projection uses standard DirectX Z mapping: Z_ndc = 0 at near, 1 at far.
static void BuildVPFromPoseFov(const XrPosef& pose, const XrFovf& fov,
    float nearZ, float farZ, float outVP[16])
{
	// View matrix = inverse of the pose (world→view)
	float qx = pose.orientation.x, qy = pose.orientation.y;
	float qz = pose.orientation.z, qw = pose.orientation.w;
	float px = pose.position.x, py = pose.position.y, pz = pose.position.z;

	// Rotation matrix from quaternion
	float r00 = 1-2*(qy*qy+qz*qz), r01 = 2*(qx*qy-qz*qw), r02 = 2*(qx*qz+qy*qw);
	float r10 = 2*(qx*qy+qz*qw), r11 = 1-2*(qx*qx+qz*qz), r12 = 2*(qy*qz-qx*qw);
	float r20 = 2*(qx*qz-qy*qw), r21 = 2*(qy*qz+qx*qw), r22 = 1-2*(qx*qx+qy*qy);

	// V = [R^T | -R^T * t] (column-major storage)
	float V[16];
	V[0]=r00; V[4]=r01; V[8] =r02; V[12]=-(r00*px+r01*py+r02*pz);
	V[1]=r10; V[5]=r11; V[9] =r12; V[13]=-(r10*px+r11*py+r12*pz);
	V[2]=r20; V[6]=r21; V[10]=r22; V[14]=-(r20*px+r21*py+r22*pz);
	V[3]=0;   V[7]=0;   V[11]=0;   V[15]=1;

	// Projection from asymmetric FOV (column-major)
	float tanL = tanf(fov.angleLeft), tanR = tanf(fov.angleRight);
	float tanU = tanf(fov.angleUp),   tanD = tanf(fov.angleDown);
	float w = tanR - tanL, h = tanU - tanD;

	float P[16] = {};
	P[0]  = 2.0f/w;             // P[0][0]
	P[5]  = 2.0f/h;             // P[1][1]
	P[8]  = (tanR+tanL)/w;      // P[2][0] — asymmetric X offset
	P[9]  = (tanU+tanD)/h;      // P[2][1] — asymmetric Y offset
	P[10] = -farZ/(farZ-nearZ); // P[2][2]
	P[14] = -(farZ*nearZ)/(farZ-nearZ); // P[3][2]
	P[11] = -1.0f;              // P[2][3] — perspective divide

	// VP = P * V (column-major)
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++) {
			float sum = 0;
			for (int k = 0; k < 4; k++)
				sum += P[k*4+r] * V[c*4+k];
			outVP[c*4+r] = sum;
		}
}

// General 4x4 matrix inverse (column-major). Returns false if singular.
static bool InvertMatrix4x4(const float m[16], float inv[16])
{
	float A[16];
	// Cofactor expansion
	A[0]  =  m[5]*(m[10]*m[15]-m[11]*m[14]) - m[9]*(m[6]*m[15]-m[7]*m[14]) + m[13]*(m[6]*m[11]-m[7]*m[10]);
	A[1]  = -(m[1]*(m[10]*m[15]-m[11]*m[14]) - m[9]*(m[2]*m[15]-m[3]*m[14]) + m[13]*(m[2]*m[11]-m[3]*m[10]));
	A[2]  =  m[1]*(m[6]*m[15]-m[7]*m[14]) - m[5]*(m[2]*m[15]-m[3]*m[14]) + m[13]*(m[2]*m[7]-m[3]*m[6]);
	A[3]  = -(m[1]*(m[6]*m[11]-m[7]*m[10]) - m[5]*(m[2]*m[11]-m[3]*m[10]) + m[9]*(m[2]*m[7]-m[3]*m[6]));
	A[4]  = -(m[4]*(m[10]*m[15]-m[11]*m[14]) - m[8]*(m[6]*m[15]-m[7]*m[14]) + m[12]*(m[6]*m[11]-m[7]*m[10]));
	A[5]  =  m[0]*(m[10]*m[15]-m[11]*m[14]) - m[8]*(m[2]*m[15]-m[3]*m[14]) + m[12]*(m[2]*m[11]-m[3]*m[10]);
	A[6]  = -(m[0]*(m[6]*m[15]-m[7]*m[14]) - m[4]*(m[2]*m[15]-m[3]*m[14]) + m[12]*(m[2]*m[7]-m[3]*m[6]));
	A[7]  =  m[0]*(m[6]*m[11]-m[7]*m[10]) - m[4]*(m[2]*m[11]-m[3]*m[10]) + m[8]*(m[2]*m[7]-m[3]*m[6]);
	A[8]  =  m[4]*(m[9]*m[15]-m[11]*m[13]) - m[8]*(m[5]*m[15]-m[7]*m[13]) + m[12]*(m[5]*m[11]-m[7]*m[9]);
	A[9]  = -(m[0]*(m[9]*m[15]-m[11]*m[13]) - m[8]*(m[1]*m[15]-m[3]*m[13]) + m[12]*(m[1]*m[11]-m[3]*m[9]));
	A[10] =  m[0]*(m[5]*m[15]-m[7]*m[13]) - m[4]*(m[1]*m[15]-m[3]*m[13]) + m[12]*(m[1]*m[7]-m[3]*m[5]);
	A[11] = -(m[0]*(m[5]*m[11]-m[7]*m[9]) - m[4]*(m[1]*m[11]-m[3]*m[9]) + m[8]*(m[1]*m[7]-m[3]*m[5]));
	A[12] = -(m[4]*(m[9]*m[14]-m[10]*m[13]) - m[8]*(m[5]*m[14]-m[6]*m[13]) + m[12]*(m[5]*m[10]-m[6]*m[9]));
	A[13] =  m[0]*(m[9]*m[14]-m[10]*m[13]) - m[8]*(m[1]*m[14]-m[2]*m[13]) + m[12]*(m[1]*m[10]-m[2]*m[9]);
	A[14] = -(m[0]*(m[5]*m[14]-m[6]*m[13]) - m[4]*(m[1]*m[14]-m[2]*m[13]) + m[12]*(m[1]*m[6]-m[2]*m[5]));
	A[15] =  m[0]*(m[5]*m[10]-m[6]*m[9]) - m[4]*(m[1]*m[10]-m[2]*m[9]) + m[8]*(m[1]*m[6]-m[2]*m[5]);

	float det = m[0]*A[0] + m[4]*A[1] + m[8]*A[2] + m[12]*A[3];
	if (fabsf(det) < 1e-12f) return false;
	float invDet = 1.0f / det;
	for (int i = 0; i < 16; i++) inv[i] = A[i] * invDet;
	return true;
}

// Column-major 4x4 multiply: out = A * B
static void MulMatrix4x4(const float A[16], const float B[16], float out[16])
{
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++) {
			float sum = 0;
			for (int k = 0; k < 4; k++)
				sum += A[k*4+r] * B[c*4+k];
			out[c*4+r] = sum;
		}
}

static bool DlssWarpUpscaleCallback(const ASWProvider::WarpUpscaleParams& p, ID3D11Texture2D** outResult)
{
	if (!s_dlssUpscaler || !s_dlssUpscaler->IsReady() || !p.warpedColor || !outResult)
		return false;

	ID3D11Device* dev = nullptr;
	p.ctx->GetDevice(&dev);
	if (!dev) return false;

	// 1. Build clipToClip for warp-to-warp temporal alignment.
	//    Warp DLSS has its own temporal history from previous warp frames.
	//    MVs describe: where was each pixel of the current warp in the previous warp?
	//    Built from OpenXR VP matrices (convention-independent for clipToClip since Z-flip cancels).
	float clipToClip[16];
	bool hasValidMVs = false;

	if (s_hasPrevWarpPose[p.eye]) {
		// Compute deltaV = V_prev * inv(V_cur) directly from poses.
		// This gives a view-space transform (cur view → prev view) using only
		// the relative pose change, avoiding large absolute positions.
		//
		// V = [R^T | -R^T*p; 0|1], inv(V) = [R | p; 0|1]
		// deltaV = V_prev * inv(V_cur)
		//        = [Rp^T * Rc | Rp^T * (pc - pp); 0|1]
		const auto& prev = s_prevWarpPose[p.eye];
		const auto& cur  = p.warpPose;

		// Prev rotation matrix (from quaternion)
		float pqx=prev.orientation.x, pqy=prev.orientation.y, pqz=prev.orientation.z, pqw=prev.orientation.w;
		float Rp00=1-2*(pqy*pqy+pqz*pqz), Rp01=2*(pqx*pqy-pqz*pqw), Rp02=2*(pqx*pqz+pqy*pqw);
		float Rp10=2*(pqx*pqy+pqz*pqw), Rp11=1-2*(pqx*pqx+pqz*pqz), Rp12=2*(pqy*pqz-pqx*pqw);
		float Rp20=2*(pqx*pqz-pqy*pqw), Rp21=2*(pqy*pqz+pqx*pqw), Rp22=1-2*(pqx*pqx+pqy*pqy);

		// Cur rotation matrix
		float cqx=cur.orientation.x, cqy=cur.orientation.y, cqz=cur.orientation.z, cqw=cur.orientation.w;
		float Rc00=1-2*(cqy*cqy+cqz*cqz), Rc01=2*(cqx*cqy-cqz*cqw), Rc02=2*(cqx*cqz+cqy*cqw);
		float Rc10=2*(cqx*cqy+cqz*cqw), Rc11=1-2*(cqx*cqx+cqz*cqz), Rc12=2*(cqy*cqz-cqx*cqw);
		float Rc20=2*(cqx*cqz-cqy*cqw), Rc21=2*(cqy*cqz+cqx*cqw), Rc22=1-2*(cqx*cqx+cqy*cqy);

		// deltaRot = Rp^T * Rc (3x3)
		float d00=Rp00*Rc00+Rp10*Rc10+Rp20*Rc20, d01=Rp00*Rc01+Rp10*Rc11+Rp20*Rc21, d02=Rp00*Rc02+Rp10*Rc12+Rp20*Rc22;
		float d10=Rp01*Rc00+Rp11*Rc10+Rp21*Rc20, d11=Rp01*Rc01+Rp11*Rc11+Rp21*Rc21, d12=Rp01*Rc02+Rp11*Rc12+Rp21*Rc22;
		float d20=Rp02*Rc00+Rp12*Rc10+Rp22*Rc20, d21=Rp02*Rc01+Rp12*Rc11+Rp22*Rc21, d22=Rp02*Rc02+Rp12*Rc12+Rp22*Rc22;

		// deltaTranslation = Rp^T * (pc - pp)
		float dpx=cur.position.x-prev.position.x, dpy=cur.position.y-prev.position.y, dpz=cur.position.z-prev.position.z;
		float dt0=Rp00*dpx+Rp10*dpy+Rp20*dpz;
		float dt1=Rp01*dpx+Rp11*dpy+Rp21*dpz;
		float dt2=Rp02*dpx+Rp12*dpy+Rp22*dpz;

		// deltaV in column-major: maps cur view → prev view
		float dV[16] = {};
		dV[0]=d00; dV[4]=d01; dV[8] =d02; dV[12]=dt0;
		dV[1]=d10; dV[5]=d11; dV[9] =d12; dV[13]=dt1;
		dV[2]=d20; dV[6]=d21; dV[10]=d22; dV[14]=dt2;
		dV[3]=0;   dV[7]=0;   dV[11]=0;   dV[15]=1;

		// clipToClip = P * deltaV * P^{-1}
		float tanL = tanf(p.cachedFov.angleLeft), tanR = tanf(p.cachedFov.angleRight);
		float tanU = tanf(p.cachedFov.angleUp),   tanD = tanf(p.cachedFov.angleDown);
		float w = tanR - tanL, h = tanU - tanD;
		float n = p.nearZ, f = p.farZ;

		float P[16] = {};
		P[0]=2.0f/w;  P[8]=(tanR+tanL)/w;
		P[5]=2.0f/h;  P[9]=(tanU+tanD)/h;
		P[10]=-f/(f-n); P[14]=-(f*n)/(f-n);
		P[11]=-1.0f;

		float Pi[16] = {};
		Pi[0]=w/2.0f;  Pi[12]=(tanR+tanL)/2.0f;
		Pi[5]=h/2.0f;  Pi[13]=(tanU+tanD)/2.0f;
		Pi[14]=-1.0f;
		Pi[11]=-(f-n)/(f*n); Pi[15]=1.0f/n;

		float temp[16];
		MulMatrix4x4(dV, Pi, temp);
		MulMatrix4x4(P, temp, clipToClip);
		hasValidMVs = true;
	}

	// First warp frame or inversion failed — use identity (zero MVs, reset DLSS)
	if (!hasValidMVs) {
		memset(clipToClip, 0, sizeof(clipToClip));
		clipToClip[0] = clipToClip[5] = clipToClip[10] = clipToClip[15] = 1.0f;
	}

	// Store current warp pose for next frame's warp-to-warp MVs
	s_prevWarpPose[p.eye] = p.warpPose;
	s_hasPrevWarpPose[p.eye] = true;

	// Warp jitter: zero. The warp shader doesn't apply sub-pixel jitter to its output,
	// so telling DLSS about fake jitter would cause it to misalign temporal history.
	// Warp DLSS accumulates center-sampled frames (no sub-pixel diversity, but cleaner
	// than reset=true spatial-only since it can average noise across frames).
	float warpJitterX = 0.0f, warpJitterY = 0.0f;

	// 2. Generate warp MVs using the camera MV compute shader
	if (!EnsureCameraMVResources(dev, p.renderW, p.renderH)) {
		dev->Release();
		return false;
	}

	ID3D11ShaderResourceView* depthSRV = GetOrCreateCameraMVDepthSRV(dev, p.cachedDepth);
	if (!depthSRV) {
		dev->Release();
		return false;
	}

	// No jitter in warp output → zero jitter delta and zero current jitter
	GenerateCameraMVs(p.ctx, depthSRV, p.renderW, p.renderH,
	    clipToClip,
	    0, 0,           // depthOffset: 0 (per-eye texture)
	    0.0f, 0.0f,     // jitterDeltaUV: 0 (no jitter in warp)
	    0.0f, 0.0f,     // currJitterUV: 0 (no jitter in warp)
	    nullptr, 0, 0,  // no game MVs
	    false);

	// 3. Dispatch DLSS — warp handles, independent temporal accumulation
	DlssUpscaler::DispatchParams params = {};
	params.color = p.warpedColor;
	params.colorSourceRegion = nullptr;
	params.motionVectors = s_cameraMVTex;
	params.mvSourceRegion = nullptr;
	params.depth = p.cachedDepth;
	params.depthSourceRegion = nullptr;
	params.jitterX = warpJitterX;
	params.jitterY = warpJitterY;
	params.deltaTimeMs = 11.1f;
	params.renderWidth = p.renderW;
	params.renderHeight = p.renderH;
	params.outputWidth = p.outputW;
	params.outputHeight = p.outputH;
	params.cameraNear = p.nearZ;
	params.cameraFar = p.farZ;
	params.sharpness = oovr_global_configuration.DlssSharpness();
	params.reset = !hasValidMVs;  // reset on first warp frame (no previous pose), temporal after
	params.mvScaleX = (float)p.renderW;
	params.mvScaleY = (float)p.renderH;
	params.biasMask = nullptr;
	params.biasMaskSourceRegion = nullptr;
	params.debugMode = 0;

	dev->Release();

	// Use separate warp DLSS handles (2-3) — independent temporal history from game.
	if (!s_dlssUpscaler->DispatchWarp(p.eye, p.ctx, params)) {
		static int s_warpFail = 0;
		if (s_warpFail++ < 5)
			OOVR_LOGF("DLSS warp: DispatchWarp failed eye=%d render=%ux%u output=%ux%u",
			    p.eye, p.renderW, p.renderH, p.outputW, p.outputH);
		return false;
	}

	*outResult = s_dlssUpscaler->GetWarpOutputDX11(p.eye);
	return (*outResult != nullptr);
}
#endif

// Shader HLSL headers are embedded as Win32 resources (avoids MSVC string literal size limits)
#include "../resources.h"

EXTERN_C IMAGE_DOS_HEADER __ImageBase;
#define HINST_THISCOMPONENT ((HINSTANCE) & __ImageBase)

static std::string LoadHLSLResource(int resourceId)
{
	HRSRC hRes = FindResource(HINST_THISCOMPONENT, MAKEINTRESOURCE(resourceId), MAKEINTRESOURCE(RES_T_HLSL));
	if (!hRes)
		return "";
	HGLOBAL hData = LoadResource(HINST_THISCOMPONENT, hRes);
	if (!hData)
		return "";
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

				if (SUCCEEDED(device->CreateBuffer(&cbd, nullptr, &dlaa_cbuffer)) && SUCCEEDED(device->CreateSamplerState(&psd, &dlaa_pointSampler))) {
					dlaaReady = true;
					OOVR_LOG("DLAA: Shaders compiled and ready");
				}
			}
		} else {
			if (dlaa_vs_blob)
				dlaa_vs_blob->Release();
			if (dlaa_pre_blob)
				dlaa_pre_blob->Release();
			if (dlaa_main_blob)
				dlaa_main_blob->Release();
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

	// Alpha-fix shader: forces alpha=1.0 on swapchain after FSR3 output.
	// VirtualDesktop-OpenXR doesn't clear alpha for layer 0 (assumes app writes alpha=1.0),
	// but FSR3 preserves the game's alpha values which can be < 1.0 in Creation Engine.
	// The OVR compositor uses premultiplied alpha, so alpha < 1.0 = semi-transparent ghosting.
	if (!alphaFix_pshader) {
		static const char* alphaFixSrc = "float4 main() : SV_Target { return float4(0, 0, 0, 1); }";
		ID3DBlob* blob = d3d_compile_shader(alphaFixSrc, "main", "ps_5_0");
		if (blob) {
			device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &alphaFix_pshader);
			blob->Release();
		}
	}
	if (!alphaFix_blendState) {
		D3D11_BLEND_DESC bd = {};
		bd.RenderTarget[0].BlendEnable = FALSE;
		bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALPHA;
		device->CreateBlendState(&bd, &alphaFix_blendState);
	}

	// NOTE: VRS is NOT initialized here — it's lazily initialized in the outer Invoke()
	// only on the dxcomp compositor. This prevents temporary compositors from calling
	// NvAPI_Initialize/Disable/Unload and interfering with the active VRS state.
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

	// ClearState unbinds all pipeline references, then Flush drains pending commands.
	// Without this, the NVIDIA driver retains stale internal pointers to our textures.
	if (context) {
		context->ClearState();
		context->Flush();
	}

	for (auto&& rtv : swapchain_rtvs)
		rtv->Release();

	swapchain_rtvs.clear();

	for (auto&& tex : resolvedMSAATextures)
		tex->Release();

	resolvedMSAATextures.clear();

	// Cached SRV cleanup
	if (cachedSrcSRV)
		cachedSrcSRV->Release();
	for (auto&& srv : resolvedMSAA_SRVs)
		if (srv)
			srv->Release();
	resolvedMSAA_SRVs.clear();

	// DLAA cleanup
	if (dlaa_vshader)
		dlaa_vshader->Release();
	if (dlaa_pre_pshader)
		dlaa_pre_pshader->Release();
	if (dlaa_main_pshader)
		dlaa_main_pshader->Release();
	if (dlaa_cbuffer)
		dlaa_cbuffer->Release();
	if (dlaaIntermediateRTV)
		dlaaIntermediateRTV->Release();
	if (dlaaIntermediateSRV)
		dlaaIntermediateSRV->Release();
	if (dlaaIntermediate)
		dlaaIntermediate->Release();
	if (dlaaOutputRTV)
		dlaaOutputRTV->Release();
	if (dlaaOutputSRV)
		dlaaOutputSRV->Release();
	if (dlaaOutput)
		dlaaOutput->Release();
	if (dlaa_pointSampler)
		dlaa_pointSampler->Release();

	// FSR cleanup
	if (fsr_vshader)
		fsr_vshader->Release();
	if (fsr_pshader)
		fsr_pshader->Release();
	if (cas_pshader)
		cas_pshader->Release();
	if (fsr_cbuffer)
		fsr_cbuffer->Release();
	if (alphaFix_pshader)
		alphaFix_pshader->Release();
	if (alphaFix_blendState)
		alphaFix_blendState->Release();

	// VRS cleanup: only the dxcomp compositor should call Shutdown (which calls Disable).
	// Non-dxcomp compositors must NOT call Disable() or it will turn off VRS mid-frame.
	if (iAmDxcomp) {
		vrsManager.Shutdown();
	}

#ifdef OC_HAS_FSR3
	// FSR3 cleanup: destroy upscaler when the dxcomp compositor dies (session restart).
	// The upscaler holds shared DX11↔DX12 textures tied to THIS device — they become
	// stale pointers after the device is released. Must be recreated with the new
	// session's device. Matches VRS cleanup pattern (dxcomp-only).
	if (iAmDxcomp && s_fsr3Upscaler) {
		OOVR_LOG("FSR3: Shutting down upscaler (dxcomp destroyed — VR session restart)");
		delete s_fsr3Upscaler;
		s_fsr3Upscaler = nullptr;
		s_fsr3FirstDispatch = true;
		s_fsr3ViewportW = 0;
		s_fsr3ViewportH = 0;
		s_hasPrevVP[0] = s_hasPrevVP[1] = false;
		s_cmvHasPrevCamZ = false;
	}
#endif
#ifdef OC_HAS_DLSS
	if (iAmDxcomp && s_dlssUpscaler) {
		OOVR_LOG("DLSS: Shutting down upscaler (dxcomp destroyed — VR session restart)");
		delete s_dlssUpscaler;
		s_dlssUpscaler = nullptr;
		s_fsr3FirstDispatch = true;
		s_fsr3ViewportW = 0;
		s_fsr3ViewportH = 0;
		s_hasPrevVP[0] = s_hasPrevVP[1] = false;
		s_cmvHasPrevCamZ = false;
	}
#endif

	// OCU ASW cleanup: compute shader + XR swapchains tied to this session
	if (iAmDxcomp && g_aswProvider) {
		OOVR_LOG("ASW: Shutting down (dxcomp destroyed — VR session restart)");
		delete g_aswProvider;
		g_aswProvider = nullptr;
	}

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
	    && oovr_global_configuration.FsrRenderScale() < 0.99f && !cube && !isOverlay;
#ifdef OC_HAS_FSR3
	// FSR 3 can handle stereo-combined textures (bounds present); FSR 1 cannot
	if (fsrActive && bounds) {
		fsrActive = s_fsr3Upscaler && s_fsr3Upscaler->IsReady()
		    && s_pBridge && s_pBridge->status == 1 && s_pBridge->mvTexture
		    && oovr_global_configuration.MotionVectorsEnabled();
	}
#else
	if (bounds)
		fsrActive = false;
#endif
	// DLSS also needs swapchain inflation (same logic as FSR)
#ifdef OC_HAS_DLSS
	bool dlssNeedsInflation = !fsrActive && s_dlssUpscaler && s_dlssUpscaler->IsReady()
	    && oovr_global_configuration.DlssEnabled()
	    && oovr_global_configuration.FsrRenderScale() < 0.99f
	    && !cube && !isOverlay;
#else
	bool dlssNeedsInflation = false;
#endif

	uint32_t outWidth = srcDesc.Width;
	uint32_t outHeight = srcDesc.Height;
	if (fsrActive || dlssNeedsInflation) {
		// FSR / DLSS: inflate swapchain to display resolution so the
		// upscaler has room to write the full-res output.
		float invScale = 1.0f / std::max(0.5f, oovr_global_configuration.FsrRenderScale());
		outWidth = (uint32_t)(srcDesc.Width * invScale);
		outHeight = (uint32_t)(srcDesc.Height * invScale);
	}
	bool fsrConfigured = fsrActive || dlssNeedsInflation;

	// Check if existing chain is compatible (compare against OUTPUT dimensions)
	bool usable = false;
	if (chain != NULL) {
		if (fsrConfigured) {
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
		if (fsrActive || dlssNeedsInflation)
			OOVR_LOGF("%s output: %dx%d (scale %.2f)", dlssNeedsInflation ? "DLSS" : "FSR",
			    outWidth, outHeight, oovr_global_configuration.FsrRenderScale());

		// ClearState unbinds all SRVs/RTVs/UAVs from the pipeline, releasing the
		// NVIDIA driver's internal tracking references to our textures. Without
		// this, destroying the swapchain leaves stale pointers in the driver's
		// descriptor cache — crash on next frame at the same deterministic address.
		// Flush then drains any remaining GPU commands that reference those resources.
		context->ClearState();
		context->Flush();

#ifdef OC_HAS_FSR3
		// If FSR3 is active, drain its DX12 queue too — shared textures cross both APIs
		if (s_fsr3Upscaler && s_fsr3Upscaler->IsReady()) {
			s_fsr3Upscaler->Shutdown();
			delete s_fsr3Upscaler;
			s_fsr3Upscaler = nullptr;
			s_fsr3FirstDispatch = true;
			s_fsr3ViewportW = 0;
			s_fsr3ViewportH = 0;
			s_hasPrevVP[0] = s_hasPrevVP[1] = false;
			s_cmvHasPrevCamZ = false;
		}
#endif
#ifdef OC_HAS_DLSS
		if (s_dlssUpscaler && s_dlssUpscaler->IsReady()) {
			delete s_dlssUpscaler;
			s_dlssUpscaler = nullptr;
			s_fsr3FirstDispatch = true;
			s_fsr3ViewportW = 0;
			s_fsr3ViewportH = 0;
			s_hasPrevVP[0] = s_hasPrevVP[1] = false;
			s_cmvHasPrevCamZ = false;
		}
#endif

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
		if (cachedSrcSRV) {
			cachedSrcSRV->Release();
			cachedSrcSRV = nullptr;
		}
		cachedSrcTex = nullptr;
		for (auto&& srv : resolvedMSAA_SRVs)
			if (srv)
				srv->Release();
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

		// ── DLAA: create intermediate + output textures (game resolution, skip for overlays) ──
		if (dlaaReady && !isOverlay) {
			// Release old textures
			if (dlaaIntermediateRTV) {
				dlaaIntermediateRTV->Release();
				dlaaIntermediateRTV = nullptr;
			}
			if (dlaaIntermediateSRV) {
				dlaaIntermediateSRV->Release();
				dlaaIntermediateSRV = nullptr;
			}
			if (dlaaIntermediate) {
				dlaaIntermediate->Release();
				dlaaIntermediate = nullptr;
			}
			if (dlaaOutputRTV) {
				dlaaOutputRTV->Release();
				dlaaOutputRTV = nullptr;
			}
			if (dlaaOutputSRV) {
				dlaaOutputSRV->Release();
				dlaaOutputSRV = nullptr;
			}
			if (dlaaOutput) {
				dlaaOutput->Release();
				dlaaOutput = nullptr;
			}

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
		if (cachedSrcSRV) {
			cachedSrcSRV->Release();
			cachedSrcSRV = nullptr;
		}
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
	// Use SOURCE texture dimensions for region (not swapchain — they differ when FSR upscales)
	D3D11_BOX sourceRegion;
	if (bounds) {
		uint32_t srcEyeW = (uint32_t)(srcDesc.Width * std::fabs(bounds->uMax - bounds->uMin));
		uint32_t srcEyeH = srcDesc.Height;
		sourceRegion.left = (uint32_t)(bounds->uMin * srcDesc.Width);
		sourceRegion.right = sourceRegion.left + srcEyeW;
		sourceRegion.top = 0;
		sourceRegion.bottom = srcEyeH;
	} else {
		sourceRegion.left = 0;
		sourceRegion.right = srcDesc.Width;
		sourceRegion.top = 0;
		sourceRegion.bottom = srcDesc.Height;
	}
	sourceRegion.front = 0;
	sourceRegion.back = 1;

	// Bounds describe an inverted image so copy texture using pixel shader inverting on copy
	if (bounds && bounds->vMin > bounds->vMax && oovr_global_configuration.InvertUsingShaders() && !swapchain_rtvs.empty()) {
		auto* src = (ID3D11Texture2D*)texture->handle;

		context->OMSetBlendState(nullptr, nullptr, 0xffffffff);

		UINT numViewPorts = 0;
		context->RSGetViewports(&numViewPorts, nullptr);
		D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
		if (numViewPorts)
			context->RSGetViewports(&numViewPorts, viewports);

		UINT numScissors = 0;
		context->RSGetScissorRects(&numScissors, nullptr);
		D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
		if (numScissors)
			context->RSGetScissorRects(&numScissors, scissors);

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
	}
#ifdef OC_HAS_FSR3
	// ── FSR 3 temporal upscaling path (DX12 interop) ──
	// Takes priority over FSR 1 when the SKSE bridge provides motion vectors + depth.
	else if (s_fsr3Upscaler && s_fsr3Upscaler->IsReady()
	    && s_pBridge && s_pBridge->status == 1 && s_pBridge->mvTexture
	    && !s_pBridge->isMainMenu && !s_pBridge->isLoadingScreen // Skip FSR3 on menu/loading
	    && ValidateBridgeTexture(reinterpret_cast<void*>(s_pBridge->mvTexture), "MV")
	    && oovr_global_configuration.MotionVectorsEnabled()
	    && oovr_global_configuration.FsrEnabled()
	    && oovr_global_configuration.FsrRenderScale() < 0.99f
	    && !isOverlay && !swapchain_rtvs.empty()) {

		{
			static int s_fsr3Hits = 0;
			s_fsr3Hits++;
			if (s_fsr3Hits <= 4 || s_fsr3Hits % 200 == 0)
				OOVR_LOGF("FSR3: dispatch path hit #%d — eye=%d bounds=%s", s_fsr3Hits, s_currentEyeIdx, bounds ? "yes" : "no");
		}

// ╔══════════════════════════════════════════════════════════════════╗
// ║ [DIAG] FSR3 BYPASS — skip DX12 dispatch, copy render-res       ║
// ║ directly to display-res swapchain. If ghost disappears,        ║
// ║ FSR3/DX12 interop is the problem. Remove this block to         ║
// ║ re-enable FSR3.                                                ║
// ╚══════════════════════════════════════════════════════════════════╝
#define FSR3_BYPASS_FOR_DIAG 0
#if FSR3_BYPASS_FOR_DIAG
		{
			// Clear swapchain to black first (display res is larger than render res)
			float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
			context->ClearRenderTargetView(swapchain_rtvs[currentIndex], black);

			// Copy render-res per-eye region to top-left of display-res swapchain
			if (bounds) {
				uint32_t eyeL = (uint32_t)(bounds->uMin * srcDesc.Width);
				uint32_t eyeR = (uint32_t)(bounds->uMax * srcDesc.Width);
				uint32_t eyeT = 0, eyeB = srcDesc.Height;
				if (bounds->vMin > bounds->vMax) {
					eyeT = (uint32_t)(bounds->vMax * srcDesc.Height);
					eyeB = (uint32_t)(bounds->vMin * srcDesc.Height);
				} else {
					eyeT = (uint32_t)(bounds->vMin * srcDesc.Height);
					eyeB = (uint32_t)(bounds->vMax * srcDesc.Height);
				}
				D3D11_BOX box = { eyeL, eyeT, 0, eyeR, eyeB, 1 };
				context->CopySubresourceRegion(imagesHandles[currentIndex].texture, 0,
				    0, 0, 0, src, 0, &box);
			} else {
				context->CopySubresourceRegion(imagesHandles[currentIndex].texture, 0,
				    0, 0, 0, src, 0, nullptr);
			}
			{
				static bool s = false;
				if (!s) {
					s = true;
					OOVR_LOG("FSR3-BYPASS: Skipping DX12 dispatch — direct render-res copy to swapchain");
				}
			}
		}
#else
		// ── Normal FSR3 dispatch path (not bypassed) ──
		{

			ID3D11Texture2D* fsrSrc = src;

			// Resolve MSAA first if needed
			if (srcDesc.SampleDesc.Count > 1 && !resolvedMSAATextures.empty()) {
				context->ResolveSubresource(resolvedMSAATextures[currentIndex], 0, src, 0, srcDesc.Format);
				fsrSrc = resolvedMSAATextures[currentIndex];
			}

			D3D11_TEXTURE2D_DESC fsrSrcDesc;
			fsrSrc->GetDesc(&fsrSrcDesc);

			// Per-eye color region: extract from stereo-combined texture when bounds is present
			D3D11_BOX colorRegion = {};
			D3D11_BOX* colorRegionPtr = nullptr;
			uint32_t perEyeRenderW = fsrSrcDesc.Width;
			uint32_t perEyeRenderH = fsrSrcDesc.Height;
			if (bounds) {
				colorRegion.left = (uint32_t)(bounds->uMin * fsrSrcDesc.Width);
				colorRegion.right = (uint32_t)(bounds->uMax * fsrSrcDesc.Width);
				if (bounds->vMin <= bounds->vMax) {
					colorRegion.top = (uint32_t)(bounds->vMin * fsrSrcDesc.Height);
					colorRegion.bottom = (uint32_t)(bounds->vMax * fsrSrcDesc.Height);
				} else {
					colorRegion.top = (uint32_t)(bounds->vMax * fsrSrcDesc.Height);
					colorRegion.bottom = (uint32_t)(bounds->vMin * fsrSrcDesc.Height);
				}
				colorRegion.front = 0;
				colorRegion.back = 1;
				colorRegionPtr = &colorRegion;
				perEyeRenderW = colorRegion.right - colorRegion.left;
				perEyeRenderH = colorRegion.bottom - colorRegion.top;
			}

			// Get MV + depth textures from SKSE bridge. These are raw pointers into the
			// game's renderer — they can become stale if the renderer resets (e.g., load
			// screen) between the VirtualQuery check above and the GetDesc call here.
			// SafeGetTextureDesc catches use-after-free and we fall back to a direct copy.
			auto* mvTex = reinterpret_cast<ID3D11Texture2D*>(s_pBridge->mvTexture);
			auto* depthTex = s_pBridge->depthTexture
			    ? reinterpret_cast<ID3D11Texture2D*>(s_pBridge->depthTexture)
			    : nullptr;
			if (depthTex && !ValidateBridgeTexture(depthTex, "Depth"))
				depthTex = nullptr;

			D3D11_TEXTURE2D_DESC mvDesc;
			if (!SafeGetTextureDesc(mvTex, &mvDesc)) {
				OOVR_LOG("FSR3: Bridge MV texture became stale (TOCTOU race) — fallback copy this frame");
				goto fsr3_fallback_copy;
			}
			{ // Scoped block — goto fsr3_fallback_copy jumps past this entire scope

					// ── Depth format conversion ──
				// Skyrim's depth-stencil is typically R24G8_TYPELESS (D24_UNORM_S8_UINT).
				// CopySubresourceRegion from D24/R24G8 → R32F silently fails (incompatible format families).
				// Extract depth via compute shader when needed, replacing depthTex with the R32F output.
				if (depthTex) {
					D3D11_TEXTURE2D_DESC depthDesc;
					if (SafeGetTextureDesc(depthTex, &depthDesc)) {
						bool needsExtract = (depthDesc.Format != DXGI_FORMAT_R32_FLOAT
						    && depthDesc.Format != DXGI_FORMAT_D32_FLOAT
						    && depthDesc.Format != DXGI_FORMAT_R32_TYPELESS);

						if (needsExtract) {
							if (EnsureDepthExtractResources(device, depthDesc.Width, depthDesc.Height)) {
								auto* depthSRV = GetOrCreateDepthSRV(device, depthTex,
								    depthDesc.Format, depthDesc.Width, depthDesc.Height);
								if (depthSRV) {
									ExtractDepthToR32F(context, depthSRV, depthDesc.Width, depthDesc.Height);
									depthTex = s_depthR32F; // Use extracted R32F for FSR3 + debug modes
									{
										static bool s = false;
										if (!s) {
											s = true;
											OOVR_LOGF("DepthExtract: Converted depth fmt=%u → R32F (%ux%u)",
											    depthDesc.Format, depthDesc.Width, depthDesc.Height);
										}
									}
								} else {
									{
										static bool s = false;
										if (!s) {
											s = true;
											OOVR_LOGF("DepthExtract: Failed to create SRV for fmt=%u — depth unavailable", depthDesc.Format);
										}
									}
									depthTex = nullptr;
								}
							} else {
								{
									static bool s = false;
									if (!s) {
										s = true;
										OOVR_LOG("DepthExtract: Failed to init resources — depth unavailable");
									}
								}
								depthTex = nullptr;
							}
						}
						// else: R32_FLOAT/D32_FLOAT/R32_TYPELESS → copy-compatible with R32F, no extraction needed
					}
				}

				// ── Reactive mask generation (depth-edge detection) ──
				// Generates per-pixel reactiveness from depth discontinuities. This tells
				// FSR3 to favor current-frame data at depth edges (tree branches against sky),
				// reducing temporal ghosting on thin geometry.
				ID3D11Texture2D* reactiveMaskTex = nullptr;
				if (depthTex) {
					D3D11_TEXTURE2D_DESC dDesc;
					depthTex->GetDesc(&dDesc);
					if (EnsureReactiveMaskResources(device, dDesc.Width, dDesc.Height)) {
						auto* rmDepthSRV = GetOrCreateReactiveMaskDepthSRV(device, depthTex);
						if (rmDepthSRV) {
							GenerateReactiveMask(context, rmDepthSRV, dDesc.Width, dDesc.Height,
							    oovr_global_configuration.Fsr3ReactiveBase(),
							    oovr_global_configuration.Fsr3ReactiveEdgeBoost(),
							    0.005f, // edge threshold
							    30.0f,  // edge scale
							    oovr_global_configuration.Fsr3ReactiveDepthFalloffStart(),
							    oovr_global_configuration.Fsr3ReactiveDepthFalloffEnd());
							reactiveMaskTex = s_reactiveMaskTex;
						}
					}
				}

				// ── Camera MV generation ──
				// RSS VP from RendererShadowState captures HMD rotation, HMD tracking,
				// and thumbstick locomotion. Double-precision clip-to-clip reprojection
				// avoids float32 catastrophic cancellation from large game-unit coordinates.
				//
				// RendererShadowState layout (VR):
				//   +0x3E0: cameraData (EYE_POSITION<ViewData, 2>), each ViewData = 0x250
				//     ViewData+0x130: viewProjMatrixUnjittered (4x4 row-major)
				ID3D11Texture2D* cameraMVTex = nullptr;
				if (depthTex && oovr_global_configuration.Fsr3CameraMV()
				    && s_pBridge && s_pBridge->rssBasePtr) {
					int eye = s_currentEyeIdx;
					const uint8_t* rssBase = reinterpret_cast<const uint8_t*>(
					    static_cast<uintptr_t>(s_pBridge->rssBasePtr));

					// Game matrices are DirectX row-major (row-vector: clip = v * M).
					// Our column-vector convention needs M^T. Row-major raw bytes
					// reinterpreted as column-major ARE the transpose — just memcpy.
					const float* rssVPRM = reinterpret_cast<const float*>(
					    rssBase + 0x3E0 + eye * 0x250 + 0x130);
					float curVP[16];
					memcpy(curVP, rssVPRM, sizeof(curVP));

					// Store UNADJUSTED curVP for next frame BEFORE locomotion injection.
					// prevVP must be in the unadjusted coordinate space so that next frame's
					// locomotion delta is correctly computed from the raw RSS matrices.
					float unadjustedCurVP[16];
					memcpy(unadjustedCurVP, curVP, sizeof(curVP));

					// ── Locomotion injection ──
					// Skyrim uses camera-relative rendering: the VP matrix origin shifts
					// with the FULL camera position each frame. prevVP * inv(curVP) only
					// captures rotation — ALL camera translation is invisible.
					//
					// Horizontal (dx, dy): from actorPos — stick locomotion only.
					// Vertical (dz): from NiCamera::world.translate.z — includes terrain,
					// jumping, walk-cycle camera bob, AND HMD physical tracking. Single
					// float read, no matrix extraction noise.
					if (eye == 0) {
						s_cmvLocoDx = 0.0f; s_cmvLocoDy = 0.0f; s_cmvLocoDz = 0.0f;

						// Horizontal (dx,dy) from actorPos
						if (s_pBridge->actorPosPtr) {
							const float* actorPos = reinterpret_cast<const float*>(
							    static_cast<uintptr_t>(s_pBridge->actorPosPtr));
							float px = actorPos[0], py = actorPos[1];
							if (s_cmvHasPrevActorPos) {
								float dx = px - s_cmvPrevActorPos[0];
								float dy = py - s_cmvPrevActorPos[1];
								float hDist2 = dx * dx + dy * dy;
								if (hDist2 > 0.0001f && hDist2 < 225.0f) {
									s_cmvLocoDx = dx; s_cmvLocoDy = dy;
								}
							}
							s_cmvPrevActorPos[0] = px;
							s_cmvPrevActorPos[1] = py;
							s_cmvHasPrevActorPos = true;
						}

						// Vertical (dz) from NiCamera world position Z.
						// Captures terrain + jumping + walk-cycle bob + HMD tracking.
						if (s_pBridge->cameraPosPtr) {
							const float* camPos = reinterpret_cast<const float*>(
							    static_cast<uintptr_t>(s_pBridge->cameraPosPtr));
							float cz = camPos[2]; // NiCamera::world.translate.z
							if (s_cmvHasPrevCamZ) {
								float dz = cz - s_cmvPrevCamZ;
								if (fabsf(dz) < 50.0f) {
									s_cmvLocoDz = dz;
								}
							}
							s_cmvPrevCamZ = cz;
							s_cmvHasPrevCamZ = true;
						}

						{
							static int s_locoLog = 0;
							if ((s_locoLog < 30 || s_locoLog % 300 == 0)
							    && (s_cmvLocoDx != 0.0f || s_cmvLocoDy != 0.0f || s_cmvLocoDz != 0.0f)) {
								OOVR_LOGF("CameraMV LOCO: hDelta(%.4f,%.4f) vDelta(%.4f) camZ=%.2f",
								    s_cmvLocoDx, s_cmvLocoDy, s_cmvLocoDz, s_cmvPrevCamZ);
							}
							s_locoLog++;
						}
					}

					// Apply locomotion injection to curVP
					if (s_cmvLocoDx != 0.0f || s_cmvLocoDy != 0.0f || s_cmvLocoDz != 0.0f) {
						InjectLocoIntoVP(curVP, s_cmvLocoDx, s_cmvLocoDy, s_cmvLocoDz);
					}

					if (s_hasPrevVP[eye]) {
						// Compute clip-to-clip M = prevVP * inv(curVP) in double precision.
						// curVP includes horizontal locomotion (actorPos) + vertical
						// camera delta (NiCamera Z), so clipToClip captures
						// rotation + full 3-axis camera translation.
						float clipToClipMat[16];
						if (ComputeClipToClip(curVP, s_prevVP[eye], clipToClipMat)) {
							D3D11_TEXTURE2D_DESC dDesc;
							depthTex->GetDesc(&dDesc);
							int depthOffX = 0, depthOffY = 0;
							bool depthIsStereo = (dDesc.Width >= perEyeRenderW * 2 - 4);
							if (depthIsStereo && s_currentEyeIdx == 1)
								depthOffX = (int)(dDesc.Width / 2);

							// Jitter delta: the game projection includes our FSR3 jitter
							// (from GetProjectionRaw), subtract in shader for clean MVs.
							float curJitterX = s_fsr3RenderJitterX;
							float curJitterY = s_fsr3RenderJitterY;
							float jdUVx = (curJitterX - s_prevJitterX[eye]) / (float)perEyeRenderW;
							float jdUVy = (curJitterY - s_prevJitterY[eye]) / (float)perEyeRenderH;

							auto* gameMVSRV = s_pBridge->mvSRV ? reinterpret_cast<ID3D11ShaderResourceView*>(s_pBridge->mvSRV) : nullptr;
							int gameMVOffX = 0;
							if (gameMVSRV && (mvDesc.Width >= perEyeRenderW * 2 - 4) && eye == 1)
								gameMVOffX = (int)(mvDesc.Width / 2);
							bool useGameMV = oovr_global_configuration.ActorMV() && gameMVSRV;

							if (EnsureCameraMVResources(device, perEyeRenderW, perEyeRenderH)) {
								auto* cmvDepthSRV = GetOrCreateCameraMVDepthSRV(device, depthTex);
								if (cmvDepthSRV) {
									GenerateCameraMVs(context, cmvDepthSRV,
									    perEyeRenderW, perEyeRenderH,
									    clipToClipMat,
									    depthOffX, depthOffY,
									    jdUVx, jdUVy,
									    0.0f, 0.0f,
									    gameMVSRV, gameMVOffX, 0,
									    useGameMV);

									// Dilate MVs: 3x3 closest-depth gives alpha-tested pixels
									// (tree branches with sky depth) the foreground neighbor's MV.
									if (EnsureMVDilateResources(device, perEyeRenderW, perEyeRenderH)) {
										auto* mvSRV = GetOrCreateMVDilateMVSRV(device, s_cameraMVTex);
										auto* dilDepthSRV = GetOrCreateMVDilateDepthSRV(device, depthTex);
										if (mvSRV && dilDepthSRV) {
											DilateCameraMVs(context, mvSRV, dilDepthSRV,
											    perEyeRenderW, perEyeRenderH,
											    depthOffX, depthOffY);
											cameraMVTex = s_mvDilateTex;
										} else {
											cameraMVTex = s_cameraMVTex;
										}
									} else {
										cameraMVTex = s_cameraMVTex;
									}

									{
										static bool s = false;
										if (!s) {
											s = true;
											OOVR_LOGF("CameraMV: RSS VP + loco injection + dilation -- %ux%u eye=%d",
											    perEyeRenderW, perEyeRenderH, eye);
										}
									}
								}
							}
						}
					}

					// Store UNADJUSTED VP for next frame
					memcpy(s_prevVP[eye], unadjustedCurVP, sizeof(curVP));
					s_prevJitterX[eye] = s_fsr3RenderJitterX;
					s_prevJitterY[eye] = s_fsr3RenderJitterY;
					s_hasPrevVP[eye] = true;
				}

				// MV region: detect if stereo-combined (double width) or per-eye
				// (used as fallback when camera MVs are not available)
				bool mvStereoCombined = (mvDesc.Width >= perEyeRenderW * 2 - 4); // Allow small rounding error
				D3D11_BOX mvRegion = {};
				if (mvStereoCombined) {
					// Stereo-combined: each eye in its own half
					uint32_t mvEyeWidth = mvDesc.Width / 2;
					mvRegion.left = (s_currentEyeIdx == 0) ? 0 : mvEyeWidth;
					mvRegion.right = mvRegion.left + mvEyeWidth;
				} else {
					// Per-eye: engine overwrites MV RT for each eye, use full texture
					mvRegion.left = 0;
					mvRegion.right = mvDesc.Width;
				}
				mvRegion.top = 0;
				mvRegion.bottom = mvDesc.Height;
				mvRegion.front = 0;
				mvRegion.back = 1;

				// Frame delta time — both eyes of a stereo pair should use the same dt.
				// Measured on left eye (eye 0); right eye reuses it. Without this fix,
				// right eye would measure ~1ms (time since left eye dispatch, not real frame time).
				static float s_fsr3StereoFrameDeltaMs = 11.1f;
				auto now = std::chrono::steady_clock::now();
				if (s_currentEyeIdx == 0) {
					float deltaMs = 11.1f; // Default ~90fps
					if (s_fsr3LastFrameTime.time_since_epoch().count() > 0) {
						auto delta = std::chrono::duration_cast<std::chrono::microseconds>(now - s_fsr3LastFrameTime);
						deltaMs = std::max(1.0f, std::min(100.0f, delta.count() / 1000.0f));
					}
					s_fsr3StereoFrameDeltaMs = deltaMs;
					s_fsr3LastFrameTime = now;
				}
				float deltaMs = s_fsr3StereoFrameDeltaMs;

				// Build dispatch parameters
				Fsr3Upscaler::DispatchParams fsr3Params = {};
				fsr3Params.color = fsrSrc;
				fsr3Params.colorSourceRegion = colorRegionPtr;
				if (cameraMVTex) {
					fsr3Params.motionVectors = cameraMVTex;
					fsr3Params.mvSourceRegion = nullptr; // Camera MVs are already per-eye, no sub-region
				} else {
					fsr3Params.motionVectors = mvTex;
					fsr3Params.mvSourceRegion = &mvRegion;
				}
				fsr3Params.depth = depthTex;
				// Depth is stereo-combined (same layout as color): extract per-eye region
				D3D11_BOX depthRegion = {};
				D3D11_BOX* depthRegionPtr = nullptr;
				if (depthTex && bounds) {
					D3D11_TEXTURE2D_DESC depthDesc;
					if (!SafeGetTextureDesc(depthTex, &depthDesc)) {
						OOVR_LOG("FSR3: Bridge depth texture became stale — disabling depth for this frame");
						depthTex = nullptr;
					} else {
						bool depthStereoCombined = (depthDesc.Width >= perEyeRenderW * 2 - 4);
						if (depthStereoCombined) {
							uint32_t depthEyeW = depthDesc.Width / 2;
							depthRegion.left = (s_currentEyeIdx == 0) ? 0 : depthEyeW;
							depthRegion.right = depthRegion.left + depthEyeW;
							depthRegion.top = 0;
							depthRegion.bottom = depthDesc.Height;
							depthRegion.front = 0;
							depthRegion.back = 1;
							depthRegionPtr = &depthRegion;
						}
					}
				}
				fsr3Params.depthSourceRegion = depthRegionPtr;
				fsr3Params.reactiveMask = reactiveMaskTex;
				fsr3Params.reactiveSourceRegion = depthRegionPtr; // Same stereo layout as depth
				fsr3Params.jitterX = s_fsr3RenderJitterX; // Use the jitter that was applied to rendering
				fsr3Params.jitterY = s_fsr3RenderJitterY;
				fsr3Params.deltaTimeMs = deltaMs;
				fsr3Params.renderWidth = perEyeRenderW;
				fsr3Params.renderHeight = perEyeRenderH;
				float fsr3InvScale = 1.0f / std::max(0.5f, oovr_global_configuration.FsrRenderScale());
				uint32_t perEyeDisplayW = (uint32_t)(perEyeRenderW * fsr3InvScale);
				uint32_t perEyeDisplayH = (uint32_t)(perEyeRenderH * fsr3InvScale);
				perEyeDisplayW = std::min(perEyeDisplayW, (uint32_t)createInfo.width);
				perEyeDisplayH = std::min(perEyeDisplayH, (uint32_t)createInfo.height);
				fsr3Params.outputWidth = perEyeDisplayW;
				fsr3Params.outputHeight = perEyeDisplayH;
				fsr3Params.cameraNear = g_fsr3CameraNear;
				fsr3Params.cameraFar = g_fsr3CameraFar;
				fsr3Params.cameraFovY = s_fsr3CameraFovY;
				fsr3Params.sharpness = oovr_global_configuration.Fsr3Sharpness();
				fsr3Params.reset = s_fsr3FirstDispatch;
				// Jitter cancellation: when camera MVs are active, our shader compensates for
				// the jitter delta and FSR3's context-level jitter cancellation also subtracts
				// jitter. This is technically double-compensation, but the error is sub-pixel
				// (~0.15px max) and invisible. Empirically, having jitter cancellation ON when
				// camera MVs are active produces stable results, while OFF causes shaking.
				fsr3Params.jitterCancellation = oovr_global_configuration.Fsr3CameraMV();
				fsr3Params.viewToMeters = oovr_global_configuration.Fsr3ViewToMeters();
				fsr3Params.mvScale = oovr_global_configuration.MotionVectorScale();
				int fsr3DbgMode = oovr_global_configuration.Fsr3DebugMode();
				fsr3Params.debugMode = fsr3DbgMode;

				// Mode 2: Bypass FSR3 — copy raw game render to swapchain (no upscaling)
				if (fsr3DbgMode == 2) {
					float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
					context->ClearRenderTargetView(swapchain_rtvs[currentIndex], black);
					if (colorRegionPtr) {
						context->CopySubresourceRegion(imagesHandles[currentIndex].texture, 0,
						    0, 0, 0, fsrSrc, 0, colorRegionPtr);
					} else {
						context->CopySubresourceRegion(imagesHandles[currentIndex].texture, 0,
						    0, 0, 0, fsrSrc, 0, nullptr);
					}
					{
						static bool s = false;
						if (!s) {
							s = true;
							OOVR_LOG("FSR3-DEBUG: Mode 2 — bypass, raw game render to swapchain");
						}
					}
					goto fsr3_end;
				}

				// Mode 3: Depth visualization — render depth as grayscale via shader
				// Mode 4: MV visualization — render motion vectors as RG color via shader
				// When camera MVs are available, show those (what FSR3 actually uses);
				// otherwise fall back to the game's bridge MV texture.
				// (CopySubresourceRegion from R32F/R16G16F → R8G8B8A8 silently fails)
				if ((fsr3DbgMode == 3 && depthTex) || fsr3DbgMode == 4) {
					EnsureDebugShaders(device);
					ID3D11PixelShader* vizPS = (fsr3DbgMode == 3) ? s_debugDepthPS : s_debugMvPS;
					ID3D11Texture2D* vizTex = (fsr3DbgMode == 3) ? depthTex
					                                             : (cameraMVTex ? cameraMVTex : mvTex);

					if (vizPS && vizTex) {
						// Compute sub-region UVs for the per-eye portion
						// Camera MVs are already per-eye (no sub-region needed)
						D3D11_TEXTURE2D_DESC vizDesc;
						vizTex->GetDesc(&vizDesc);
						float uvMin[2] = { 0.0f, 0.0f };
						float uvMax[2] = { 1.0f, 1.0f };
						D3D11_BOX* regionPtr = (fsr3DbgMode == 3) ? depthRegionPtr
						                                          : (cameraMVTex ? nullptr : &mvRegion);
						if (regionPtr) {
							uvMin[0] = (float)regionPtr->left / vizDesc.Width;
							uvMin[1] = (float)regionPtr->top / vizDesc.Height;
							uvMax[0] = (float)regionPtr->right / vizDesc.Width;
							uvMax[1] = (float)regionPtr->bottom / vizDesc.Height;
						}

						// Update debug constant buffer
						// NOTE: The full-screen VS does tex.y = 1.0f - tex.y (D3D bottom-up convention).
						// This means input.tex.y=0 is at the BOTTOM of the screen (= top of texture).
						// To correctly map screen → texture sub-region, swap uvMin.y and uvMax.y.
						if (s_debugCB) {
							D3D11_MAPPED_SUBRESOURCE mapped;
							if (SUCCEEDED(context->Map(s_debugCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
								float* cb = (float*)mapped.pData;
								cb[0] = uvMin[0];
								cb[1] = uvMax[1]; // uvMin: x normal, y swapped
								cb[2] = uvMax[0];
								cb[3] = uvMin[1]; // uvMax: x normal, y swapped
								context->Unmap(s_debugCB, 0);
							}
						}

						// Create SRV with explicit format (handles typeless textures)
						ID3D11ShaderResourceView* vizSRV = nullptr;
						D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
						srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
						srvDesc.Texture2D.MipLevels = 1;
						// Use known format based on mode
						if (fsr3DbgMode == 3) {
							srvDesc.Format = DXGI_FORMAT_R32_FLOAT; // depthTex is always R32F after extraction
						} else {
							srvDesc.Format = DXGI_FORMAT_R16G16_FLOAT; // MV texture from bridge
						}
						HRESULT srvHr = device->CreateShaderResourceView(vizTex, &srvDesc, &vizSRV);
						if (FAILED(srvHr)) {
							// Fallback: try with default format
							srvHr = device->CreateShaderResourceView(vizTex, nullptr, &vizSRV);
						}
						if (FAILED(srvHr) || !vizSRV) {
							{
								static int errCnt = 0;
								if (errCnt++ < 10)
									OOVR_LOGF("FSR3-DEBUG: CreateSRV for mode %d failed (hr=0x%08X fmt=%u texFmt=%u)",
									    fsr3DbgMode, srvHr, srvDesc.Format, vizDesc.Format);
							}
						}
						if (vizSRV) {
							// Set up viewport to fill swapchain
							D3D11_TEXTURE2D_DESC swapDesc;
							imagesHandles[currentIndex].texture->GetDesc(&swapDesc);
							D3D11_VIEWPORT vp = {};
							vp.Width = (float)swapDesc.Width;
							vp.Height = (float)swapDesc.Height;
							vp.MaxDepth = 1.0f;

							float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
							context->ClearRenderTargetView(swapchain_rtvs[currentIndex], black);

							// Save state
							D3D11_PRIMITIVE_TOPOLOGY savedTopo;
							context->IAGetPrimitiveTopology(&savedTopo);
							UINT numVP = 0;
							context->RSGetViewports(&numVP, nullptr);
							D3D11_VIEWPORT savedVP[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
							if (numVP)
								context->RSGetViewports(&numVP, savedVP);
							UINT numSR = 0;
							context->RSGetScissorRects(&numSR, nullptr);
							D3D11_RECT savedSR[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
							if (numSR)
								context->RSGetScissorRects(&numSR, savedSR);
							ID3D11RasterizerState* savedRS = nullptr;
							context->RSGetState(&savedRS);

							// Render full-screen quad
							// Reset rasterizer state: game may leave ScissorEnable=true with a
							// small scissor rect that clips our quad to nothing (causes black).
							context->RSSetState(nullptr);
							D3D11_RECT sr = { 0, 0, (LONG)swapDesc.Width, (LONG)swapDesc.Height };
							context->RSSetScissorRects(1, &sr);
							context->RSSetViewports(1, &vp);
							context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
							context->OMSetBlendState(nullptr, nullptr, 0xffffffff);
							context->OMSetRenderTargets(1, &swapchain_rtvs[currentIndex], nullptr);
							context->VSSetShader(fs_vshader, nullptr, 0);
							context->PSSetShader(vizPS, nullptr, 0);
							context->PSSetShaderResources(0, 1, &vizSRV);
							context->PSSetConstantBuffers(0, 1, &s_debugCB);
							context->PSSetSamplers(0, 1, &quad_sampleState);
							context->Draw(4, 0);

							// Unbind and restore
							ID3D11ShaderResourceView* nullSRV = nullptr;
							context->PSSetShaderResources(0, 1, &nullSRV);
							ID3D11RenderTargetView* nullRTV = nullptr;
							context->OMSetRenderTargets(1, &nullRTV, nullptr);
							context->IASetPrimitiveTopology(savedTopo);
							if (numVP)
								context->RSSetViewports(numVP, savedVP);
							if (numSR)
								context->RSSetScissorRects(numSR, savedSR);
							context->RSSetState(savedRS);
							if (savedRS)
								savedRS->Release();

							vizSRV->Release();

							{
								static int vizLog = 0;
								if (vizLog++ < 3)
									OOVR_LOGF("FSR3-DEBUG: Mode %d — %s viz OK (region=%.2f,%.2f → %.2f,%.2f texFmt=%u %ux%u)",
									    fsr3DbgMode, fsr3DbgMode == 3 ? "depth" : "MV",
									    uvMin[0], uvMin[1], uvMax[0], uvMax[1],
									    vizDesc.Format, vizDesc.Width, vizDesc.Height);
							}
						}
					}
					goto fsr3_end;
				}

				bool fsr3Ok = s_fsr3Upscaler->Dispatch(s_currentEyeIdx, context, fsr3Params);
				s_fsr3FirstDispatch = false;

				// Sync both eyes: if left eye wasn't ready (warmup), force right eye to use
				// fallback too, so both eyes transition to FSR3 output on the same stereo frame.
				{
					static bool s_fsr3LeftEyeReady = false;
					if (s_currentEyeIdx == 0) {
						s_fsr3LeftEyeReady = fsr3Ok;
					} else if (fsr3Ok && !s_fsr3LeftEyeReady) {
						fsr3Ok = false;
					}
				}

				if (fsr3Ok) {
					ID3D11Texture2D* fsr3Output = s_fsr3Upscaler->GetOutputDX11(s_currentEyeIdx);

	
					// FSR3 has built-in RCAS sharpening — direct copy to swapchain.
					context->CopySubresourceRegion(imagesHandles[currentIndex].texture, 0,
					    0, 0, 0, fsr3Output, 0, nullptr);

					// Set viewport to display resolution — FSR3 filled the full swapchain
					s_fsr3ViewportW = perEyeDisplayW;
					s_fsr3ViewportH = perEyeDisplayH;

					{
						static bool s = false;
						if (!s) {
							s = true;
							OOVR_LOGF("FSR3: First output — upscale %ux%u→%ux%u, swapchain %ux%u",
							    perEyeRenderW, perEyeRenderH, perEyeDisplayW, perEyeDisplayH,
							    createInfo.width, createInfo.height);
						}
					}
				} else {
					// Async pipeline warmup: no output yet. Copy render-res game content
					// as fallback into the display-res swapchain.
					// Set viewport to render resolution so the render-res content fills the
					// viewport correctly. Without this, PostSubmit would use createInfo
					// (display res), causing a mismatch where the render-res image only fills
					// part of the viewport — visible as one eye at lower res on the first frame.
					if (colorRegionPtr) {
						context->CopySubresourceRegion(imagesHandles[currentIndex].texture, 0,
						    0, 0, 0, fsrSrc, 0, colorRegionPtr);
					} else {
						D3D11_BOX srcBox = { 0, 0, 0, perEyeRenderW, perEyeRenderH, 1 };
						context->CopySubresourceRegion(imagesHandles[currentIndex].texture, 0,
						    0, 0, 0, fsrSrc, 0, &srcBox);
					}
					s_fsr3ViewportW = perEyeRenderW;
					s_fsr3ViewportH = perEyeRenderH;
					{
						static bool s = false;
						if (!s) {
							s = true;
							OOVR_LOGF("FSR3: Dispatch warmup — fallback render-res copy (%ux%u) to display-res swapchain", perEyeRenderW, perEyeRenderH);
						}
					}
				}
			} // end scoped block (goto fsr3_fallback_copy jumps past here)
			goto fsr3_end;

		// Reached via goto when bridge MV texture becomes stale between VirtualQuery
		// and GetDesc (TOCTOU race). Copy render-res content so we don't show black.
		fsr3_fallback_copy: {
			if (colorRegionPtr) {
				context->CopySubresourceRegion(imagesHandles[currentIndex].texture, 0,
				    0, 0, 0, fsrSrc, 0, colorRegionPtr);
			} else {
				D3D11_BOX srcBox = { 0, 0, 0, perEyeRenderW, perEyeRenderH, 1 };
				context->CopySubresourceRegion(imagesHandles[currentIndex].texture, 0,
				    0, 0, 0, fsrSrc, 0, &srcBox);
			}
			s_fsr3ViewportW = perEyeRenderW;
			s_fsr3ViewportH = perEyeRenderH;
			{
				static bool s = false;
				if (!s) {
					s = true;
					OOVR_LOG("FSR3: Stale bridge texture — fallback render-res copy");
				}
			}
		}
		fsr3_end:;
		} // close #else block
#endif // FSR3_BYPASS_FOR_DIAG
	}
#endif
#ifdef OC_HAS_DLSS
	// ── DLSS 4 Super Resolution path (native DX11 NGX, no DX12 interop) ──
	else if (s_dlssUpscaler && s_dlssUpscaler->IsReady()
	    && s_pBridge && s_pBridge->status == 1 && s_pBridge->mvTexture
	    && !s_pBridge->isMainMenu && !s_pBridge->isLoadingScreen
	    && ValidateBridgeTexture(reinterpret_cast<void*>(s_pBridge->mvTexture), "MV")
	    && oovr_global_configuration.DlssEnabled()
	    && oovr_global_configuration.FsrRenderScale() < 0.99f
	    && !isOverlay && !swapchain_rtvs.empty()) {

		{
			static bool s_dlssFirstLog = false;
			if (!s_dlssFirstLog) {
				s_dlssFirstLog = true;
				OOVR_LOGF("DLSS: dispatch path active, eye=%d", s_currentEyeIdx);
			}
		}

		ID3D11Texture2D* dlssSrc = src;
		// Resolve MSAA first if needed
		if (srcDesc.SampleDesc.Count > 1 && !resolvedMSAATextures.empty()) {
			context->ResolveSubresource(resolvedMSAATextures[currentIndex], 0, src, 0, srcDesc.Format);
			dlssSrc = resolvedMSAATextures[currentIndex];
		}
		D3D11_TEXTURE2D_DESC dlssSrcDesc;
		dlssSrc->GetDesc(&dlssSrcDesc);

		// Per-eye color sub-region
		D3D11_BOX dlssColorRegion = {};
		D3D11_BOX* dlssColorRegionPtr = nullptr;
		uint32_t dlssRenderW = dlssSrcDesc.Width;
		uint32_t dlssRenderH = dlssSrcDesc.Height;
		if (bounds) {
			dlssColorRegion.left = (uint32_t)(bounds->uMin * dlssSrcDesc.Width);
			dlssColorRegion.right = (uint32_t)(bounds->uMax * dlssSrcDesc.Width);
			if (bounds->vMin <= bounds->vMax) {
				dlssColorRegion.top = (uint32_t)(bounds->vMin * dlssSrcDesc.Height);
				dlssColorRegion.bottom = (uint32_t)(bounds->vMax * dlssSrcDesc.Height);
			} else {
				dlssColorRegion.top = (uint32_t)(bounds->vMax * dlssSrcDesc.Height);
				dlssColorRegion.bottom = (uint32_t)(bounds->vMin * dlssSrcDesc.Height);
			}
			dlssColorRegion.front = 0;
			dlssColorRegion.back = 1;
			dlssColorRegionPtr = &dlssColorRegion;
			dlssRenderW = dlssColorRegion.right - dlssColorRegion.left;
			dlssRenderH = dlssColorRegion.bottom - dlssColorRegion.top;
		}

		// MV + depth from SKSE bridge
		auto* dlssMVTex = reinterpret_cast<ID3D11Texture2D*>(s_pBridge->mvTexture);
		auto* dlssDepthTex = s_pBridge->depthTexture
		    ? reinterpret_cast<ID3D11Texture2D*>(s_pBridge->depthTexture)
		    : nullptr;
		if (dlssDepthTex && !ValidateBridgeTexture(dlssDepthTex, "Depth"))
			dlssDepthTex = nullptr;

		D3D11_TEXTURE2D_DESC dlssMVDesc;
		bool dlssBridgeOk = SafeGetTextureDesc(dlssMVTex, &dlssMVDesc);

		if (dlssBridgeOk) {
			// MV stereo-combined sub-region
			bool dlssMVStereo = (dlssMVDesc.Width >= dlssRenderW * 2 - 4);
			D3D11_BOX dlssMVRegion = {};
			if (dlssMVStereo) {
				uint32_t mvEyeW = dlssMVDesc.Width / 2;
				dlssMVRegion.left = (s_currentEyeIdx == 0) ? 0 : mvEyeW;
				dlssMVRegion.right = dlssMVRegion.left + mvEyeW;
			} else {
				dlssMVRegion.left = 0;
				dlssMVRegion.right = dlssMVDesc.Width;
			}
			dlssMVRegion.top = 0;
			dlssMVRegion.bottom = dlssMVDesc.Height;
			dlssMVRegion.front = 0;
			dlssMVRegion.back = 1;

			// Depth sub-region
			D3D11_BOX dlssDepthRegion = {};
			D3D11_BOX* dlssDepthRegionPtr = nullptr;
			if (dlssDepthTex) {
				D3D11_TEXTURE2D_DESC dlssDepthDesc;
				if (SafeGetTextureDesc(dlssDepthTex, &dlssDepthDesc)) {
					bool depthStereo = (dlssDepthDesc.Width >= dlssRenderW * 2 - 4);
					dlssDepthRegion.left = (depthStereo && s_currentEyeIdx == 1) ? dlssDepthDesc.Width / 2 : 0;
					dlssDepthRegion.right = dlssDepthRegion.left + (depthStereo ? dlssDepthDesc.Width / 2 : dlssDepthDesc.Width);
					dlssDepthRegion.top = 0;
					dlssDepthRegion.bottom = dlssDepthDesc.Height;
					dlssDepthRegion.front = 0;
					dlssDepthRegion.back = 1;
					dlssDepthRegionPtr = &dlssDepthRegion;
				} else {
					dlssDepthTex = nullptr;
				}
			}

			// ── Depth format conversion for DLSS ──
			// Skyrim's depth-stencil is R24G8_TYPELESS (D24_UNORM_S8_UINT).
			// CopySubresourceRegion to R32F silently fails. Extract via compute shader.
			if (dlssDepthTex) {
				D3D11_TEXTURE2D_DESC dlssDepthFmtDesc;
				if (SafeGetTextureDesc(dlssDepthTex, &dlssDepthFmtDesc)) {
					bool needsExtract = (dlssDepthFmtDesc.Format != DXGI_FORMAT_R32_FLOAT
					    && dlssDepthFmtDesc.Format != DXGI_FORMAT_D32_FLOAT
					    && dlssDepthFmtDesc.Format != DXGI_FORMAT_R32_TYPELESS);
					if (needsExtract) {
						if (EnsureDepthExtractResources(device, dlssDepthFmtDesc.Width, dlssDepthFmtDesc.Height)) {
							auto* depthSRV = GetOrCreateDepthSRV(device, dlssDepthTex,
							    dlssDepthFmtDesc.Format, dlssDepthFmtDesc.Width, dlssDepthFmtDesc.Height);
							if (depthSRV) {
								ExtractDepthToR32F(context, depthSRV, dlssDepthFmtDesc.Width, dlssDepthFmtDesc.Height);
								dlssDepthTex = s_depthR32F;
								{
									static bool s = false;
									if (!s) {
										s = true;
										OOVR_LOGF("DLSS DepthExtract: Converted depth fmt=%u → R32F (%ux%u)",
										    dlssDepthFmtDesc.Format, dlssDepthFmtDesc.Width, dlssDepthFmtDesc.Height);
									}
								}
							}
						}
					}
				}
			}

			// ── Camera MV generation for DLSS ──
			// Generates per-pixel camera motion from depth + VP matrix deltas.
			// Without this, static geometry has zero motion during head rotation/locomotion.
			ID3D11Texture2D* dlssCameraMVTex = nullptr;
			if (dlssDepthTex && oovr_global_configuration.Fsr3CameraMV()
			    && s_pBridge && s_pBridge->rssBasePtr) {
				int eye = s_currentEyeIdx;
				const uint8_t* rssBase = reinterpret_cast<const uint8_t*>(
				    static_cast<uintptr_t>(s_pBridge->rssBasePtr));

				const float* rssVPRM = reinterpret_cast<const float*>(
				    rssBase + 0x3E0 + eye * 0x250 + 0x130);
				float curVP[16];
				memcpy(curVP, rssVPRM, sizeof(curVP));

				// Store UNADJUSTED curVP before locomotion injection
				float unadjustedCurVP[16];
				memcpy(unadjustedCurVP, curVP, sizeof(curVP));

				// ── Locomotion injection (same as FSR3 path) ──
				// Horizontal from actorPos, vertical from NiCamera::world.translate.z
				if (eye == 0) {
					s_cmvLocoDx = 0.0f; s_cmvLocoDy = 0.0f; s_cmvLocoDz = 0.0f;

					// Horizontal (dx,dy) from actorPos
					if (s_pBridge->actorPosPtr) {
						const float* actorPos = reinterpret_cast<const float*>(
						    static_cast<uintptr_t>(s_pBridge->actorPosPtr));
						float px = actorPos[0], py = actorPos[1];
						if (s_cmvHasPrevActorPos) {
							float dx = px - s_cmvPrevActorPos[0];
							float dy = py - s_cmvPrevActorPos[1];
							float hDist2 = dx * dx + dy * dy;
							if (hDist2 > 0.0001f && hDist2 < 225.0f) {
								s_cmvLocoDx = dx; s_cmvLocoDy = dy;
							}
						}
						s_cmvPrevActorPos[0] = px;
						s_cmvPrevActorPos[1] = py;
						s_cmvHasPrevActorPos = true;
					}

					// Vertical (dz) from NiCamera world position Z.
					// Captures terrain + jumping + walk-cycle bob + HMD tracking.
					if (s_pBridge->cameraPosPtr) {
						const float* camPos = reinterpret_cast<const float*>(
						    static_cast<uintptr_t>(s_pBridge->cameraPosPtr));
						float cz = camPos[2]; // NiCamera::world.translate.z
						if (s_cmvHasPrevCamZ) {
							float dz = cz - s_cmvPrevCamZ;
							if (fabsf(dz) < 50.0f) {
								s_cmvLocoDz = dz;
							}
						}
						s_cmvPrevCamZ = cz;
						s_cmvHasPrevCamZ = true;
					}

					{
						static int s_locoLog = 0;
						if (s_locoLog < 50 || s_locoLog % 300 == 0) {
							OOVR_LOGF("DLSS LOCO-DIAG: inject(%.4f,%.4f,%.4f) camZ=%.2f actorZ=%.2f",
							    s_cmvLocoDx, s_cmvLocoDy, s_cmvLocoDz,
							    s_cmvPrevCamZ, s_pBridge->actorPosPtr ?
							    reinterpret_cast<const float*>(static_cast<uintptr_t>(s_pBridge->actorPosPtr))[2] : 0.0f);
						}
						s_locoLog++;
					}
				}

				// Apply locomotion injection to curVP
				if (s_cmvLocoDx != 0.0f || s_cmvLocoDy != 0.0f || s_cmvLocoDz != 0.0f) {
					InjectLocoIntoVP(curVP, s_cmvLocoDx, s_cmvLocoDy, s_cmvLocoDz);
				}

				if (s_hasPrevVP[eye]) {
					float clipToClipMat[16];
					if (ComputeClipToClip(curVP, s_prevVP[eye], clipToClipMat)) {
						D3D11_TEXTURE2D_DESC dDesc;
						dlssDepthTex->GetDesc(&dDesc);
						int depthOffX = 0, depthOffY = 0;
						bool depthIsStereo = (dDesc.Width >= dlssRenderW * 2 - 4);
						if (depthIsStereo && s_currentEyeIdx == 1)
							depthOffX = (int)(dDesc.Width / 2);

						// No jitter delta — MVs are from unjittered RSS VP matrices.
						// currJitterUV unjitters pixel coords before clipToClip transform.
						float jdUVx = 0.0f;
						float jdUVy = 0.0f;
						float currJUVx = s_fsr3RenderJitterX / (float)dlssRenderW;
						float currJUVy = s_fsr3RenderJitterY / (float)dlssRenderH;

						auto* gameMVSRV = s_pBridge->mvSRV ? reinterpret_cast<ID3D11ShaderResourceView*>(s_pBridge->mvSRV) : nullptr;
						int gameMVOffX = 0;
						if (gameMVSRV && (dlssMVDesc.Width >= dlssRenderW * 2 - 4) && eye == 1)
							gameMVOffX = (int)(dlssMVDesc.Width / 2);
						bool useGameMV = oovr_global_configuration.ActorMV() && gameMVSRV;

						if (EnsureCameraMVResources(device, dlssRenderW, dlssRenderH)) {
							auto* cmvDepthSRV = GetOrCreateCameraMVDepthSRV(device, dlssDepthTex);
							if (cmvDepthSRV) {
								GenerateCameraMVs(context, cmvDepthSRV,
								    dlssRenderW, dlssRenderH,
								    clipToClipMat,
								    depthOffX, depthOffY,
								    jdUVx, jdUVy,
								    currJUVx, currJUVy,
								    gameMVSRV, gameMVOffX, 0,
								    useGameMV);
								// Dilate MVs: 3x3 closest-depth gives alpha-tested pixels
								// (tree branches with sky depth) the foreground neighbor's MV.
								if (EnsureMVDilateResources(device, dlssRenderW, dlssRenderH)) {
									auto* mvSRV = GetOrCreateMVDilateMVSRV(device, s_cameraMVTex);
									auto* dilDepthSRV = GetOrCreateMVDilateDepthSRV(device, dlssDepthTex);
									if (mvSRV && dilDepthSRV) {
										DilateCameraMVs(context, mvSRV, dilDepthSRV,
										    dlssRenderW, dlssRenderH,
										    depthOffX, depthOffY);
										dlssCameraMVTex = s_mvDilateTex;
									} else {
										dlssCameraMVTex = s_cameraMVTex;
									}
								} else {
									dlssCameraMVTex = s_cameraMVTex;
								}

								{
									static bool s = false;
									if (!s) {
										s = true;
										OOVR_LOGF("DLSS CameraMV: RSS VP + loco injection + dilation -- %ux%u eye=%d",
										    dlssRenderW, dlssRenderH, eye);
									}
								}
							}
						}
					}
				}

				// Store UNADJUSTED VP for next frame
				memcpy(s_prevVP[eye], unadjustedCurVP, sizeof(curVP));
				s_prevJitterX[eye] = s_fsr3RenderJitterX;
				s_prevJitterY[eye] = s_fsr3RenderJitterY;
				s_hasPrevVP[eye] = true;
			}

			// ── Bias mask generation (depth-edge detection for DLSS ghosting reduction) ──
			// Same algorithm as FSR3's reactive mask: detects depth discontinuities and
			// generates per-pixel bias that tells DLSS to favor current frame over history
			// at object silhouettes (foliage, thin geometry).
			ID3D11Texture2D* dlssBiasMaskTex = nullptr;
			D3D11_BOX* dlssBiasMaskRegionPtr = nullptr;
			if (dlssDepthTex && (oovr_global_configuration.DlssBiasBase() > 0.0f
			                  || oovr_global_configuration.DlssBiasEdgeBoost() > 0.0f)) {
				D3D11_TEXTURE2D_DESC dDesc;
				dlssDepthTex->GetDesc(&dDesc);
				if (EnsureReactiveMaskResources(device, dDesc.Width, dDesc.Height)) {
					auto* rmDepthSRV = GetOrCreateReactiveMaskDepthSRV(device, dlssDepthTex);
					if (rmDepthSRV) {
						GenerateReactiveMask(context, rmDepthSRV, dDesc.Width, dDesc.Height,
						    oovr_global_configuration.DlssBiasBase(),
						    oovr_global_configuration.DlssBiasEdgeBoost(),
						    0.005f, // edge threshold
						    30.0f,  // edge scale
						    oovr_global_configuration.DlssBiasDepthFalloffStart(),
						    oovr_global_configuration.DlssBiasDepthFalloffEnd());
						dlssBiasMaskTex = s_reactiveMaskTex;
						// If depth is stereo-combined, bias mask uses same sub-region
						dlssBiasMaskRegionPtr = dlssDepthRegionPtr;
						{
							static bool s = false;
							if (!s) {
								s = true;
								OOVR_LOGF("DLSS BiasMask: Generated %ux%u base=%.3f edgeBoost=%.3f",
								    dDesc.Width, dDesc.Height,
								    oovr_global_configuration.DlssBiasBase(),
								    oovr_global_configuration.DlssBiasEdgeBoost());
							}
						}
					}
				}
			}

			// Per-eye display resolution (same render-scale logic as FSR3)
			float dlssInvScale = 1.0f / std::max(0.5f, oovr_global_configuration.FsrRenderScale());
			uint32_t dlssDisplayW = std::min((uint32_t)(dlssRenderW * dlssInvScale), (uint32_t)createInfo.width);
			uint32_t dlssDisplayH = std::min((uint32_t)(dlssRenderH * dlssInvScale), (uint32_t)createInfo.height);

			// Frame delta time (left eye measurement, shared across stereo pair)
			static std::chrono::steady_clock::time_point s_dlssLastFrameTime;
			static float s_dlssFrameDeltaMs = 11.1f;
			{
				auto now = std::chrono::steady_clock::now();
				if (s_currentEyeIdx == 0) {
					if (s_dlssLastFrameTime.time_since_epoch().count() > 0) {
						auto delta = std::chrono::duration_cast<std::chrono::microseconds>(now - s_dlssLastFrameTime);
						s_dlssFrameDeltaMs = std::max(1.0f, std::min(100.0f, delta.count() / 1000.0f));
					}
					s_dlssLastFrameTime = now;
				}
			}

			// Build and dispatch
			DlssUpscaler::DispatchParams dlssParams = {};
			dlssParams.color = dlssSrc;
			dlssParams.colorSourceRegion = dlssColorRegionPtr;
			if (dlssCameraMVTex) {
				dlssParams.motionVectors = dlssCameraMVTex;
				dlssParams.mvSourceRegion = nullptr; // Camera MVs are already per-eye
				// Camera MVs are UV-space (0-1). DLSS InMVScale converts to pixel space.
				float dlssMvScale = oovr_global_configuration.DlssMvScale();
				dlssParams.mvScaleX = (float)dlssRenderW * dlssMvScale;
				dlssParams.mvScaleY = (float)dlssRenderH * dlssMvScale;
			} else {
				dlssParams.motionVectors = dlssMVTex;
				dlssParams.mvSourceRegion = dlssMVStereo ? &dlssMVRegion : nullptr;
				float s = oovr_global_configuration.MotionVectorScale();
				dlssParams.mvScaleX = s;
				dlssParams.mvScaleY = s;
			}
			dlssParams.depth = dlssDepthTex;
			dlssParams.depthSourceRegion = dlssDepthRegionPtr;
			dlssParams.jitterX = s_fsr3RenderJitterX; // shared jitter
			dlssParams.jitterY = s_fsr3RenderJitterY;
			dlssParams.deltaTimeMs = s_dlssFrameDeltaMs;
			dlssParams.renderWidth = dlssRenderW;
			dlssParams.renderHeight = dlssRenderH;
			dlssParams.outputWidth = dlssDisplayW;
			dlssParams.outputHeight = dlssDisplayH;
			dlssParams.cameraNear = g_fsr3CameraNear;
			dlssParams.cameraFar = g_fsr3CameraFar;
			dlssParams.sharpness = oovr_global_configuration.DlssSharpness();
			dlssParams.biasMask = dlssBiasMaskTex;
			dlssParams.biasMaskSourceRegion = dlssBiasMaskRegionPtr;
			dlssParams.reset = s_fsr3FirstDispatch;
			dlssParams.debugMode = 0;
			s_fsr3FirstDispatch = false;

			bool dlssOk = s_dlssUpscaler->Dispatch(s_currentEyeIdx, context, dlssParams);

			// Eye sync: ensure both eyes transition to DLSS output on the same stereo frame
			{
				static bool s_dlssLeftEyeReady = false;
				if (s_currentEyeIdx == 0) {
					s_dlssLeftEyeReady = dlssOk;
				} else if (dlssOk && !s_dlssLeftEyeReady) {
					dlssOk = false; // Force right eye fallback if left eye failed
				}
			}

			if (dlssOk) {
				ID3D11Texture2D* dlssOutput = s_dlssUpscaler->GetOutputDX11(s_currentEyeIdx);
				context->CopySubresourceRegion(imagesHandles[currentIndex].texture, 0,
				    0, 0, 0, dlssOutput, 0, nullptr);
				s_fsr3ViewportW = dlssDisplayW;
				s_fsr3ViewportH = dlssDisplayH;
				{
					static bool s = false;
					if (!s) {
						s = true;
						OOVR_LOGF("DLSS: First output %ux%u→%ux%u swapchain %ux%u",
						    dlssRenderW, dlssRenderH, dlssDisplayW, dlssDisplayH,
						    createInfo.width, createInfo.height);
					}
				}
			} else {
				// Dispatch failed — copy render-res content as fallback
				if (dlssColorRegionPtr) {
					context->CopySubresourceRegion(imagesHandles[currentIndex].texture, 0,
					    0, 0, 0, dlssSrc, 0, dlssColorRegionPtr);
				} else {
					D3D11_BOX srcBox = { 0, 0, 0, dlssRenderW, dlssRenderH, 1 };
					context->CopySubresourceRegion(imagesHandles[currentIndex].texture, 0,
					    0, 0, 0, dlssSrc, 0, &srcBox);
				}
				s_fsr3ViewportW = dlssRenderW;
				s_fsr3ViewportH = dlssRenderH;
			}
		} else {
			// Stale bridge texture — copy render-res content as fallback
			{
				static bool s = false;
				if (!s) {
					s = true;
					OOVR_LOG("DLSS: Stale bridge texture — fallback copy");
				}
			}
			if (dlssColorRegionPtr) {
				context->CopySubresourceRegion(imagesHandles[currentIndex].texture, 0,
				    0, 0, 0, dlssSrc, 0, dlssColorRegionPtr);
			} else {
				D3D11_BOX srcBox = { 0, 0, 0, dlssRenderW, dlssRenderH, 1 };
				context->CopySubresourceRegion(imagesHandles[currentIndex].texture, 0,
				    0, 0, 0, dlssSrc, 0, &srcBox);
			}
			s_fsr3ViewportW = dlssRenderW;
			s_fsr3ViewportH = dlssRenderH;
		}
	}
#endif
	else if (fsrReady && oovr_global_configuration.CasEnabled() && !isOverlay
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
			D3D11_VIEWPORT dv = {};
			dv.Width = (float)dlaaWidth;
			dv.Height = (float)dlaaHeight;
			dv.MaxDepth = 1.0f;
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

		UINT numViewPorts = 0;
		context->RSGetViewports(&numViewPorts, nullptr);
		D3D11_VIEWPORT savedViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
		if (numViewPorts)
			context->RSGetViewports(&numViewPorts, savedViewports);

		UINT numScissors = 0;
		context->RSGetScissorRects(&numScissors, nullptr);
		D3D11_RECT savedScissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
		if (numScissors)
			context->RSGetScissorRects(&numScissors, savedScissors);

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

		D3D11_MAPPED_SUBRESOURCE mapped;
		if (SUCCEEDED(context->Map(dlaa_cbuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
			float cbData[4] = { 1.0f / dlaaWidth, 1.0f / dlaaHeight, 0, 0 };
			memcpy(mapped.pData, cbData, 16);
			context->Unmap(dlaa_cbuffer, 0);
		}

		UINT numViewPorts = 0;
		context->RSGetViewports(&numViewPorts, nullptr);
		D3D11_VIEWPORT savedVPs[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
		if (numViewPorts)
			context->RSGetViewports(&numViewPorts, savedVPs);
		UINT numScissors = 0;
		context->RSGetScissorRects(&numScissors, nullptr);
		D3D11_RECT savedSR[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
		if (numScissors)
			context->RSGetScissorRects(&numScissors, savedSR);
		ID3D11RasterizerState* savedRS = nullptr;
		context->RSGetState(&savedRS);
		context->RSSetState(nullptr);

		D3D11_PRIMITIVE_TOPOLOGY savedTopo;
		context->IAGetPrimitiveTopology(&savedTopo);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
		context->OMSetBlendState(nullptr, nullptr, 0xffffffff);

		D3D11_VIEWPORT dv = {};
		dv.Width = (float)dlaaWidth;
		dv.Height = (float)dlaaHeight;
		dv.MaxDepth = 1.0f;
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
		D3D11_VIEWPORT sv = {};
		sv.Width = (float)createInfo.width;
		sv.Height = (float)createInfo.height;
		sv.MaxDepth = 1.0f;
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
		if (numViewPorts)
			context->RSSetViewports(numViewPorts, savedVPs);
		if (numScissors)
			context->RSSetScissorRects(numScissors, savedSR);
		context->RSSetState(savedRS);
	} else {
		// ── Normal copy path (no FSR/CAS/DLAA) ──
		// Clear swapchain to black when it's larger than source (FSR configured but not yet active)
		// to avoid garbage pixels in the unused region during loading/transition
		if (bounds && !swapchain_rtvs.empty() && createInfo.width != srcDesc.Width) {
			float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
			context->ClearRenderTargetView(swapchain_rtvs[currentIndex], black);
		}
		if (srcDesc.SampleDesc.Count > 1) {
			D3D11_TEXTURE2D_DESC resDesc = srcDesc;
			resDesc.SampleDesc.Count = 1;
			context->ResolveSubresource(resolvedMSAATextures[currentIndex], 0, src, 0, resDesc.Format);
			context->CopySubresourceRegion(imagesHandles[currentIndex].texture, 0, 0, 0, 0, resolvedMSAATextures[currentIndex], 0, &sourceRegion);
		} else {
			context->CopySubresourceRegion(imagesHandles[currentIndex].texture, 0, 0, 0, 0, src, 0, &sourceRegion);
		}
#if defined(OC_HAS_FSR3) || defined(OC_HAS_DLSS)
		// When swapchain is display-res but content is render-res, set viewport to match
		// actual content size so PostSubmit doesn't stretch the small image across the full viewport
		if (createInfo.width != (sourceRegion.right - sourceRegion.left)) {
			s_fsr3ViewportW = sourceRegion.right - sourceRegion.left;
			s_fsr3ViewportH = sourceRegion.bottom - sourceRegion.top;
		}
#endif
	}

	// Release the swapchain - OpenXR will use the last-released image in a swapchain
	// No manual Flush() needed — xrReleaseSwapchainImage handles GPU synchronization internally.
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
#if defined(OC_HAS_FSR3) || defined(OC_HAS_DLSS)
	// Reset upscaler viewport crop — set by FSR3/DLSS dispatch if it runs this frame
	s_fsr3ViewportW = 0;
	s_fsr3ViewportH = 0;
#endif

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

#ifdef OC_HAS_FSR3
	// Capture per-eye pose and FOV for camera MV computation (inner Invoke reads these)
	s_fsr3EyePose[s_currentEyeIdx] = layer.pose;
	s_fsr3EyeFov[s_currentEyeIdx] = layer.fov;
#endif

#ifdef OC_HAS_FSR3
	// ── FSR 3: lazy-init upscaler and update per-frame jitter ──
	if (oovr_global_configuration.FsrEnabled() && oovr_global_configuration.FsrRenderScale() < 0.99f) {
		// Try to open the SKSE render target bridge (MV + depth)
		OpenRenderTargetBridge();

		// Lazy-init the FSR 3 upscaler (DX12 device + FidelityFX DLLs)
		if (!s_fsr3Upscaler && s_pBridge && s_pBridge->status == 1) {
			s_fsr3Upscaler = new Fsr3Upscaler();
			if (!s_fsr3Upscaler->Initialize(device)) {
				OOVR_LOG("FSR3: Initialization failed — falling back to FSR 1");
				delete s_fsr3Upscaler;
				s_fsr3Upscaler = nullptr;
			}
		}


		// Save jitter and camera FOV on left eye (once per stereo frame).
		// IMPORTANT: Do NOT compute next-frame jitter here — right eye's GetProjectionRaw
		// hasn't been called yet and would pick up the wrong (next) jitter value.
		if (s_fsr3Upscaler && s_fsr3Upscaler->IsReady() && eye == XruEyeLeft) {
			// No jitter on main menu / loading screen — upscaler won't run
			if (s_pBridge && (s_pBridge->isMainMenu || s_pBridge->isLoadingScreen)) {
				g_fsr3JitterEnabled = false;
			} else {
				// Save the jitter that was applied to THIS frame's rendering
				// (g_fsr3JitterX/Y were set after the PREVIOUS frame's right eye submit)
				s_fsr3RenderJitterX = g_fsr3JitterX;
				s_fsr3RenderJitterY = g_fsr3JitterY;
#if FSR3_BYPASS_FOR_DIAG
				g_fsr3JitterEnabled = false; // No jitter when bypassing FSR3
#else
				g_fsr3JitterEnabled = true;
#endif

				// Capture vertical FOV from OpenXR view
				s_fsr3CameraFovY = fabsf(layer.fov.angleUp) + fabsf(layer.fov.angleDown);
			}
		}
	} else if (!oovr_global_configuration.DlssEnabled()) {
		// Only disable jitter if DLSS isn't handling it either
		g_fsr3JitterEnabled = false;
	}
#endif

#ifdef OC_HAS_DLSS
	// ── DLSS: save jitter and enable on left eye ──
	if (oovr_global_configuration.DlssEnabled() && oovr_global_configuration.FsrRenderScale() < 0.99f
	    && s_dlssUpscaler && s_dlssUpscaler->IsReady() && eye == XruEyeLeft) {
		if (s_pBridge && (s_pBridge->isMainMenu || s_pBridge->isLoadingScreen)) {
			g_fsr3JitterEnabled = false;
		} else {
			s_fsr3RenderJitterX = g_fsr3JitterX;
			s_fsr3RenderJitterY = g_fsr3JitterY;
			g_fsr3JitterEnabled = true;
		}
	} else if (!oovr_global_configuration.FsrEnabled() && eye == XruEyeLeft
	    && !(s_dlssUpscaler && s_dlssUpscaler->IsReady())) {
		g_fsr3JitterEnabled = false;
	}

	// ── DLSS 4: lazy-init upscaler ──
	// DLSS is mutually exclusive with FSR3 — only one activates at a time.
	if (oovr_global_configuration.DlssEnabled() && oovr_global_configuration.FsrRenderScale() < 0.99f) {
		static bool s_dlssInitFailed = false; // Prevent retrying every frame (~300ms per attempt)
		OpenRenderTargetBridge();
		if (!s_dlssUpscaler && !s_dlssInitFailed && s_pBridge && s_pBridge->status == 1) {
			s_dlssUpscaler = new DlssUpscaler();
			if (!s_dlssUpscaler->Initialize(device)) {
				OOVR_LOG("DLSS: Initialization failed — falling back to FSR 1. Check log for details.");
				delete s_dlssUpscaler;
				s_dlssUpscaler = nullptr;
				s_dlssInitFailed = true;
			}
		}
	}
#endif

	// OCU ASW: lazy-init (needs bridge MV for texture dimensions + eye resolution)
	if (oovr_global_configuration.ASWEnabled()) {
		OpenRenderTargetBridge();
		if (!g_aswProvider && s_pBridge && s_pBridge->status == 1) {
			// Get eye resolution — use display-res when upscaler is active so
			// ASW staging textures match the upscaled output resolution.
			auto* src = (ID3D11Texture2D*)texture->handle;
			D3D11_TEXTURE2D_DESC srcDesc;
			if (SafeGetTextureDesc(src, &srcDesc)) {
				uint32_t renderEyeW = ptrBounds ? srcDesc.Width / 2 : srcDesc.Width;
				uint32_t renderEyeH = srcDesc.Height;
				uint32_t aswEyeW = renderEyeW;
				uint32_t aswEyeH = renderEyeH;

				// If an upscaler is active, initialize ASW at display resolution
				float renderScale = oovr_global_configuration.FsrRenderScale();
				bool upscalerActive = false;
				bool dlssEn = false;
#ifdef OC_HAS_DLSS
				dlssEn = oovr_global_configuration.DlssEnabled();
				upscalerActive = upscalerActive || (dlssEn && renderScale < 0.99f);
#endif
#ifdef OC_HAS_FSR3
				if (!upscalerActive)
					upscalerActive = (oovr_global_configuration.FsrEnabled() && renderScale < 0.99f && !dlssEn);
#endif
				OOVR_LOGF("ASW INIT_DIAG: renderScale=%.3f dlssEnabled=%d upscalerActive=%d render=%ux%u swapchain=%ux%u",
				    renderScale, dlssEn ? 1 : 0, upscalerActive ? 1 : 0,
				    renderEyeW, renderEyeH, (uint32_t)createInfo.width, (uint32_t)createInfo.height);
				if (upscalerActive) {
					float invScale = 1.0f / std::max(0.5f, renderScale);
					aswEyeW = (uint32_t)(renderEyeW * invScale);
					aswEyeH = (uint32_t)(renderEyeH * invScale);
					// Don't clamp to current swapchain — it may not be inflated yet
					// (swapchain inflation happens on first upscaler dispatch).
					OOVR_LOGF("ASW: Upscaler active — initializing at display-res %ux%u (render %ux%u, scale %.2f)",
					    aswEyeW, aswEyeH, renderEyeW, renderEyeH, renderScale);
				}

				// Decide: Meta runtime space warp vs custom PC-side ASW
				bool useMetaSpaceWarp = g_spaceWarpAvailable
				    && !oovr_global_configuration.ASWForceCustom();

				if (useMetaSpaceWarp) {
					// XR_FB_space_warp: runtime handles reprojection on headset
					g_spaceWarpProvider = new SpaceWarpProvider();
					if (!g_spaceWarpProvider->Initialize(aswEyeW, aswEyeH)) {
						OOVR_LOG("ASW: Meta space warp init failed — falling back to custom ASW");
						delete g_spaceWarpProvider;
						g_spaceWarpProvider = nullptr;
						useMetaSpaceWarp = false;
					} else {
						OOVR_LOG("ASW: Using Meta XR_FB_space_warp (runtime-side reprojection)");
					}
				}

				if (!useMetaSpaceWarp) {
					// Custom PC-side ASW
					g_aswProvider = new ASWProvider();
					if (!g_aswProvider->Initialize(device, renderEyeW, renderEyeH, aswEyeW, aswEyeH)) {
						OOVR_LOG("ASW: Custom ASW initialization failed — disabling");
						delete g_aswProvider;
						g_aswProvider = nullptr;
					} else {
						OOVR_LOG("ASW: Using custom PC-side ASW");
#ifdef OC_HAS_DLSS
						// Register DLSS warp upscale callback when DLSS is active.
						// This makes SubmitWarpedOutput run DLSS spatial upscaling on
						// each eye's render-res warp output before copying to the output swapchain.
						if (dlssEn && s_dlssUpscaler && s_dlssUpscaler->IsReady()) {
							g_aswProvider->SetWarpUpscaleCallback(&DlssWarpUpscaleCallback);
							OOVR_LOG("ASW: DLSS warp upscale callback registered");
						}
#endif
					}
				}
			}
		}
	}

	// Copy the texture across
	Invoke(texture, ptrBounds);

	// OCU ASW: pause warping during loading screens (prevents black flashing
	// from warping stale pre-loading content).
	if (g_aswProvider && s_pBridge) {
		g_aswProvider->SetPaused(s_pBridge->isLoadingScreen != 0);
	}

	// OCU Meta Space Warp: submit MV + depth to runtime via XR_FB_space_warp.
	// Accumulate per-eye regions; submit both eyes when eye 1 arrives.
	layer.next = nullptr;
	if (g_spaceWarpProvider && g_spaceWarpProvider->IsReady()
	    && s_pBridge && s_pBridge->status == 1 && s_pBridge->mvTexture
	    && !s_pBridge->isMainMenu && !s_pBridge->isLoadingScreen
	    && ValidateBridgeTexture(reinterpret_cast<void*>(s_pBridge->mvTexture), "SpaceWarp-MV")) {

		static D3D11_BOX s_swMvRegions[2] = {};
		static D3D11_BOX s_swDepthRegions[2] = {};
		static ID3D11Texture2D* s_swMvTex = nullptr;
		static ID3D11Texture2D* s_swDepthTex = nullptr;

		int eyeIdx = s_currentEyeIdx;
		auto* mvTex = reinterpret_cast<ID3D11Texture2D*>(s_pBridge->mvTexture);
		D3D11_TEXTURE2D_DESC mvDesc;
		if (SafeGetTextureDesc(mvTex, &mvDesc)) {
			uint32_t mvEyeW = mvDesc.Width / 2;

			s_swMvRegions[eyeIdx].left = eyeIdx * mvEyeW;
			s_swMvRegions[eyeIdx].right = s_swMvRegions[eyeIdx].left + mvEyeW;
			s_swMvRegions[eyeIdx].top = 0;
			s_swMvRegions[eyeIdx].bottom = mvDesc.Height;
			s_swMvRegions[eyeIdx].front = 0;
			s_swMvRegions[eyeIdx].back = 1;
			s_swMvTex = mvTex;

			// Extract depth to R32F (same as custom ASW path)
			auto* depthTex = s_pBridge->depthTexture
			    ? reinterpret_cast<ID3D11Texture2D*>(s_pBridge->depthTexture)
			    : nullptr;
			if (depthTex && !ValidateBridgeTexture(depthTex, "SpaceWarp-Depth"))
				depthTex = nullptr;

			s_swDepthTex = nullptr;
			if (depthTex) {
				D3D11_TEXTURE2D_DESC depthDesc;
				if (SafeGetTextureDesc(depthTex, &depthDesc)) {
					if (EnsureDepthExtractResources(device, depthDesc.Width, depthDesc.Height)) {
						auto* depthSRV = GetOrCreateDepthSRV(device, depthTex,
						    depthDesc.Format, depthDesc.Width, depthDesc.Height);
						if (depthSRV && ExtractDepthToR32F(context, depthSRV,
						        depthDesc.Width, depthDesc.Height)) {
							s_swDepthTex = s_depthR32F;
							uint32_t depthEyeW = depthDesc.Width / 2;
							s_swDepthRegions[eyeIdx].left = eyeIdx * depthEyeW;
							s_swDepthRegions[eyeIdx].right = s_swDepthRegions[eyeIdx].left + depthEyeW;
							s_swDepthRegions[eyeIdx].top = 0;
							s_swDepthRegions[eyeIdx].bottom = depthDesc.Height;
							s_swDepthRegions[eyeIdx].front = 0;
							s_swDepthRegions[eyeIdx].back = 1;
						}
					}
				}
			}

			// Submit both eyes when right eye arrives
			if (eyeIdx == 1 && s_swMvTex) {
				bool ok = g_spaceWarpProvider->SubmitFrame(context,
				    s_swMvTex, s_swMvRegions,
				    s_swDepthTex, s_swDepthRegions,
				    g_fsr3CameraNear, g_fsr3CameraFar);
				static int s_log = 0;
				if (s_log++ < 5)
					OOVR_LOGF("SpaceWarp: SubmitFrame result=%d near=%.2f far=%.1f",
					    ok ? 1 : 0, g_fsr3CameraNear, g_fsr3CameraFar);
			}
		}
	}

	// OCU ASW: poll for async shader compilation completion (non-blocking).
	// Must be outside the IsReady() gate — shaders compile in background while
	// IsReady() returns false. This check costs ~0 when not compiling.
	if (g_aswProvider && !g_aswProvider->IsReady())
		g_aswProvider->TryFinishShaderCompilation();

	// OCU ASW: cache frame data (color + MV + depth + pose) for warping
	// Skip caching during main menu / loading screen — MV and depth data are invalid,
	// and warping menu content causes visual glitches on save load.
	if (g_aswProvider && g_aswProvider->IsReady()
	    && s_pBridge && s_pBridge->status == 1 && s_pBridge->mvTexture
	    && !s_pBridge->isMainMenu && !s_pBridge->isLoadingScreen
	    && ValidateBridgeTexture(reinterpret_cast<void*>(s_pBridge->mvTexture), "ASW-MV")) {

		auto* mvTex = reinterpret_cast<ID3D11Texture2D*>(s_pBridge->mvTexture);
		auto* depthTex = s_pBridge->depthTexture
		    ? reinterpret_cast<ID3D11Texture2D*>(s_pBridge->depthTexture)
		    : nullptr;
		if (depthTex && !ValidateBridgeTexture(depthTex, "ASW-Depth"))
			depthTex = nullptr;

		D3D11_TEXTURE2D_DESC mvDesc;
		if (SafeGetTextureDesc(mvTex, &mvDesc)) {
			uint32_t mvEyeW = mvDesc.Width / 2;
			int eyeIdx = s_currentEyeIdx;

			// Build per-eye MV region (bridge texture is stereo-combined)
			D3D11_BOX mvRegion = {};
			mvRegion.left = eyeIdx * mvEyeW;
			mvRegion.right = mvRegion.left + mvEyeW;
			mvRegion.top = 0;
			mvRegion.bottom = mvDesc.Height;
			mvRegion.front = 0;
			mvRegion.back = 1;

			// Always cache pre-upscaler render-res color for ASW.
			// When DLSS/FSR3 is active, warp output will be upscaled via the
			// warp upscale callback (SubmitWarpedOutput). This ensures every
			// displayed frame goes through the upscaler, fixing jitter shaking
			// that occurs when alternating upscaled game frames with non-upscaled warps.
			ID3D11Texture2D* colorSrc = (ID3D11Texture2D*)texture->handle;
			D3D11_TEXTURE2D_DESC colorDesc;
			D3D11_BOX colorRegion = {};

			if (SafeGetTextureDesc(colorSrc, &colorDesc)) {
				// Raw submitted texture may be stereo-packed
				uint32_t colorEyeW = ptrBounds ? colorDesc.Width / 2 : colorDesc.Width;
				colorRegion.left = ptrBounds ? eyeIdx * colorEyeW : 0;
				colorRegion.right = colorRegion.left + colorEyeW;
				colorRegion.top = 0;
				colorRegion.bottom = colorDesc.Height;
				colorRegion.front = 0;
				colorRegion.back = 1;
			}

			// Extract bridge depth (R24G8_TYPELESS) to R32F via compute shader.
			// CopySubresourceRegion silently produces zeros for depth-stencil textures
			// due to GPU-internal depth compression. SRV read via CS works correctly.
			ID3D11Texture2D* aswDepthSrc = nullptr;
			D3D11_BOX depthRegion = {};
			if (depthTex) {
				D3D11_TEXTURE2D_DESC depthDesc;
				if (SafeGetTextureDesc(depthTex, &depthDesc)) {
					if (EnsureDepthExtractResources(device, depthDesc.Width, depthDesc.Height)) {
						auto* depthSRV = GetOrCreateDepthSRV(device, depthTex,
						    depthDesc.Format, depthDesc.Width, depthDesc.Height);
						if (depthSRV && ExtractDepthToR32F(context, depthSRV,
						        depthDesc.Width, depthDesc.Height)) {
							// Use the R32F extraction result instead of raw bridge depth
							aswDepthSrc = s_depthR32F;
							uint32_t depthEyeW = depthDesc.Width / 2;
							depthRegion.left = eyeIdx * depthEyeW;
							depthRegion.right = depthRegion.left + depthEyeW;
							depthRegion.top = 0;
							depthRegion.bottom = depthDesc.Height;
							depthRegion.front = 0;
							depthRegion.back = 1;
							}
					}
				}
			}

			// Compute clipToClipNoLoco for this eye from RSS VP matrices.
			// MUST be done BEFORE CacheFrame — CacheFrame(eye=1) advances m_buildSlot,
			// so SetClipToClipNoLoco after CacheFrame would write to the wrong slot.
			if (s_pBridge->rssBasePtr) {
				static float s_aswPrevVP[2][16] = {};
				static bool s_aswHasPrevVP[2] = { false, false };

				const float* rssVPRM = reinterpret_cast<const float*>(
				    reinterpret_cast<const uint8_t*>(
				        static_cast<uintptr_t>(s_pBridge->rssBasePtr))
				    + 0x3E0 + eyeIdx * 0x250 + 0x130);
				float curVP[16];
				memcpy(curVP, rssVPRM, sizeof(curVP));

				if (s_aswHasPrevVP[eyeIdx]) {
					// No-loco c2c: head rotation + head translation only
					float c2cNoLoco[16];
					if (ComputeClipToClip(curVP, s_aswPrevVP[eyeIdx], c2cNoLoco)) {
						g_aswProvider->SetClipToClipNoLoco(eyeIdx, c2cNoLoco);
					}

					// Full c2c WITH locomotion: inject actorPos delta into curVP
					// (same as camera MV code), giving depth-aware locomotion parallax.
					float curVPWithLoco[16];
					memcpy(curVPWithLoco, curVP, sizeof(curVPWithLoco));
					if (s_cmvLocoDx != 0.0f || s_cmvLocoDy != 0.0f || s_cmvLocoDz != 0.0f) {
						InjectLocoIntoVP(curVPWithLoco, s_cmvLocoDx, s_cmvLocoDy, s_cmvLocoDz);
					}
					float c2cWithLoco[16];
					if (ComputeClipToClip(curVPWithLoco, s_aswPrevVP[eyeIdx], c2cWithLoco)) {
						g_aswProvider->SetClipToClipWithLoco(eyeIdx, c2cWithLoco);
					}
				}
				memcpy(s_aswPrevVP[eyeIdx], curVP, sizeof(curVP));
				s_aswHasPrevVP[eyeIdx] = true;
			}

			// Store NiCamera Z for vertical warp correction (BEFORE CacheFrame)
			if (s_pBridge->cameraPosPtr) {
				const float* camPos = reinterpret_cast<const float*>(
				    static_cast<uintptr_t>(s_pBridge->cameraPosPtr));
				g_aswProvider->SetSlotCameraPosZ(camPos[2]);
				// Pass live pointer + view matrix so WarpFrame can read at warp time
				g_aswProvider->SetCameraPosPtr(s_pBridge->cameraPosPtr);
				if (s_pBridge->rssBasePtr) {
					g_aswProvider->SetRSSViewMatPtr(reinterpret_cast<const float*>(
					    reinterpret_cast<const uint8_t*>(
					        static_cast<uintptr_t>(s_pBridge->rssBasePtr))
					    + 0x3E0 + 1 * 0x250 + 0x30));
				}
			}

			g_aswProvider->CacheFrame(eyeIdx, context,
			    colorSrc, &colorRegion,
			    mvTex, &mvRegion,
			    aswDepthSrc, aswDepthSrc ? &depthRegion : nullptr,
			    layer.pose, layer.fov,
			    g_fsr3CameraNear, g_fsr3CameraFar);

			// Stick locomotion + yaw correction: compute once per real frame (right eye).
			//
			// Uses actorPosPtr (PlayerCharacter::data.location) for world position.
			// This moves only with stick locomotion/physics, NOT head tracking —
			// clean signal for ASW warp correction.
			// RSS view matrix translation is always zero (camera-relative rendering).
			//
			// Uses actorYawPtr (PlayerCharacter::data.angle.z) for stick yaw.
			// PlayerCamera::yaw is broken in Skyrim VR (stuck at 0).
			if (eyeIdx == 1 && s_pBridge->actorPosPtr) {
				static float s_prevActorPos[3] = {};
				static float s_prevActorYaw = 0.0f;
				static bool s_hasPrevActor = false;
				// Smoothed locomotion/yaw — exponential decay on release prevents
				// jarring snap when player releases thumbstick.
				static float s_smoothLocoX = 0.0f, s_smoothLocoY = 0.0f, s_smoothLocoZ = 0.0f;
				static float s_smoothYaw = 0.0f;
				static constexpr float kLocoDecay = 0.3f;  // Per-frame decay (0=instant, 1=no decay)
				static constexpr float kYawDecay = 0.3f;

				const float* actorPos = reinterpret_cast<const float*>(
				    static_cast<uintptr_t>(s_pBridge->actorPosPtr));
				float posX = actorPos[0], posY = actorPos[1], posZ = actorPos[2];

	
				if (s_hasPrevActor) {
					// World-space delta (new - old): direction of locomotion
					float dwx = posX - s_prevActorPos[0];
					float dwy = posY - s_prevActorPos[1];
					float dwz = posZ - s_prevActorPos[2];

					float dist2 = dwx * dwx + dwy * dwy + dwz * dwz;
					if (dist2 > 0.0001f && dist2 < 225.0f) { // dead zone + 15-unit teleport clamp
						// Transform world delta to current view space using RSS view matrix rotation.
						// RSS view matrix is at rssBase + 0x3E0 + eye*0x250 + 0x30.
						// Row-major, row-vector: v_view = v_world * R
						const float* vm = s_pBridge->rssBasePtr
						    ? reinterpret_cast<const float*>(
						          reinterpret_cast<const uint8_t*>(
						              static_cast<uintptr_t>(s_pBridge->rssBasePtr))
						          + 0x3E0 + 1 * 0x250 + 0x30)
						    : nullptr;
						if (vm) {
							float viewX = dwx * vm[0] + dwy * vm[4] + dwz * vm[8];
							float viewY = dwx * vm[1] + dwy * vm[5] + dwz * vm[9];
							float viewZ = dwx * vm[2] + dwy * vm[6] + dwz * vm[10];
							float viewMag2 = viewX * viewX + viewY * viewY + viewZ * viewZ;
							if (viewMag2 < 0.0225f) { // ~0.15 view-space units: suppress idle/physics drift
								viewX = 0.0f;
								viewY = 0.0f;
								viewZ = 0.0f;
							}
							// Active locomotion: use raw values (responsive)
							s_smoothLocoX = viewX;
							s_smoothLocoY = viewY;
							s_smoothLocoZ = viewZ;
							g_aswProvider->SetLocomotionTranslation(viewX, viewY, viewZ);
						}
					} else {
						// Dead zone or teleport: decay smoothly instead of snapping to zero
						s_smoothLocoX *= kLocoDecay;
						s_smoothLocoY *= kLocoDecay;
						s_smoothLocoZ *= kLocoDecay;
						// Zero out when decayed to negligible
						if (s_smoothLocoX * s_smoothLocoX + s_smoothLocoY * s_smoothLocoY +
						    s_smoothLocoZ * s_smoothLocoZ < 1e-8f) {
							s_smoothLocoX = s_smoothLocoY = s_smoothLocoZ = 0.0f;
						}
						g_aswProvider->SetLocomotionTranslation(s_smoothLocoX, s_smoothLocoY, s_smoothLocoZ);
					}

					// Yaw delta from actorYaw (PlayerCharacter::data.angle.z)
					float yaw = s_pBridge->actorYawPtr
					    ? *reinterpret_cast<const float*>(
					          static_cast<uintptr_t>(s_pBridge->actorYawPtr))
					    : 0.0f;
					float yawDelta = s_prevActorYaw - yaw;
					if (yawDelta > 3.14159f)
						yawDelta -= 6.28318f;
					if (yawDelta < -3.14159f)
						yawDelta += 6.28318f;
					if (fabsf(yawDelta) > 0.5f)
						yawDelta = 0.0f;

					// Read right thumbstick X directly from OpenXR — clean signal
					// with zero head-yaw contamination. Non-zero = player is
					// intentionally rotating via stick.
					float rightStickX = 0.0f;
					if (xr_rightStickX_action != XR_NULL_HANDLE && xr_session.get() != XR_NULL_HANDLE) {
						XrActionStateGetInfo info = { XR_TYPE_ACTION_STATE_GET_INFO };
						info.action = xr_rightStickX_action;
						XrActionStateFloat state = { XR_TYPE_ACTION_STATE_FLOAT };
						if (XR_SUCCEEDED(xrGetActionStateFloat(xr_session.get(), &info, &state)) && state.isActive)
							rightStickX = state.currentState;
					}

					// Use actorYaw delta for the actual rotation amount (warp correction),
					// but gate it on thumbstick deflection to avoid head-yaw contamination.
					// When stick is released, decay smoothly instead of snapping to zero.
					static constexpr float kStickDeadZone = 0.05f; // ~5% deflection
					bool stickActive = fabsf(rightStickX) > kStickDeadZone;
					if (stickActive) {
						// Active rotation: use raw yaw delta (responsive)
						s_smoothYaw = yawDelta;
					} else if (fabsf(yawDelta) > 0.001f && fabsf(s_smoothYaw) > 0.0001f) {
						// Stick released but game still rotating (momentum): use yaw delta
						// but decay toward zero to smooth the transition
						s_smoothYaw = yawDelta * kYawDecay + s_smoothYaw * (1.0f - kYawDecay);
					} else {
						// Fully stopped: decay residual
						s_smoothYaw *= kYawDecay;
						if (fabsf(s_smoothYaw) < 1e-5f)
							s_smoothYaw = 0.0f;
					}

					g_aswProvider->SetLocomotionYaw(s_smoothYaw);
					s_prevActorYaw = yaw;

					// Vertical camera Z is now handled at warp time via live cameraPosPtr
					// reading (SetSlotCameraPosZ + SetCameraPosPtr set above).
				}

				s_prevActorPos[0] = posX;
				s_prevActorPos[1] = posY;
				s_prevActorPos[2] = posZ;
				s_hasPrevActor = true;
			}
		}
	}

#if defined(OC_HAS_FSR3) || defined(OC_HAS_DLSS)
	// After right eye: compute NEXT frame's jitter and increment frame counter.
	// This must happen AFTER both eyes have rendered and dispatched with the current jitter.
	// Previously this was in the left-eye block, which caused the right eye to pick up
	// the wrong (next-frame) jitter in GetProjectionRaw — producing temporal instability.
	if (g_fsr3JitterEnabled && eye == XruEyeRight) {
		auto* src = (ID3D11Texture2D*)texture->handle;
		D3D11_TEXTURE2D_DESC srcDesc;
		src->GetDesc(&srcDesc);
		uint32_t renderW = ptrBounds ? srcDesc.Width / 2 : srcDesc.Width;
		uint32_t displayW = xr_main_view(XruEyeLeft).recommendedImageRectWidth;

#ifdef OC_HAS_FSR3
		g_fsr3JitterPhaseCount = Fsr3Upscaler::GetJitterPhaseCount(renderW, displayW);
		Fsr3Upscaler::GetJitterOffset(&g_fsr3JitterX, &g_fsr3JitterY,
		    g_fsr3FrameIndex, g_fsr3JitterPhaseCount);
#else
		// DLSS-only build: use DlssUpscaler's jitter helpers
		g_fsr3JitterPhaseCount = DlssUpscaler::GetJitterPhaseCount(renderW, displayW);
		DlssUpscaler::GetJitterOffset(&g_fsr3JitterX, &g_fsr3JitterY,
		    g_fsr3FrameIndex, g_fsr3JitterPhaseCount);
#endif
		// Apply jitter scale — lower values reduce temporal instability in VR.
		// DLSS has its own jitter scale config to allow independent tuning.
		float jScale = oovr_global_configuration.Fsr3JitterScale();
#ifdef OC_HAS_DLSS
		if (oovr_global_configuration.DlssEnabled())
			jScale = oovr_global_configuration.DlssJitterScale();
#endif
		g_fsr3JitterX *= jScale;
		g_fsr3JitterY *= jScale;
		g_fsr3FrameIndex++;
	}
#endif

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
#if defined(OC_HAS_FSR3) || defined(OC_HAS_DLSS)
		// When upscaler has produced output, use display-res viewport
		if (s_fsr3ViewportW > 0 && s_fsr3ViewportH > 0) {
			viewport.extent.width = s_fsr3ViewportW;
			viewport.extent.height = s_fsr3ViewportH;
		} else
#endif
		{
			viewport.extent.width = createInfo.width;
			viewport.extent.height = createInfo.height;
		}
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
