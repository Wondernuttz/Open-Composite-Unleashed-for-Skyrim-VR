#include "stdafx.h"

#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)

#include "../Reimpl/BaseCompositor.h"
#include "../Reimpl/BaseInput.h"
#include "dx11compositor.h"
#include "VRSGaze.h"
#include "VRSSceneScope.h"
#include "VRSShaderGuard.h"
#include "VRSAlphaCoverageScope.h"
#include "DepthExtract.h"
#include "RDMRenderDiagnostics.h"
#include "FoveationBlackoutRenderer.h"
#include "PublishedBridge.h"

// Shared by the eye compositors; prepared before either eye can omit scene work.
static FoveationBlackoutRenderer s_blackoutRenderer;
static bool s_blackoutPresentationFailed = false;


#include "../Misc/Config.h"
#include "../Misc/EffectFoveationState.h"
#include "../Misc/MipBiasHook.h"
#include "../Misc/xr_ext.h"
#include "generated/static_bases.gen.h"

#include <algorithm>
#include <cstddef>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <d3dcompiler.h> // For compiling shaders! D3DCompile
#include <string>
#include <filesystem>
#include <fstream>


#pragma comment(lib, "d3dcompiler.lib")

// ── Shader disk cache (same as ASWProvider) ──
static uint64_t FnvHash(const void* data, size_t len)
{
	uint64_t h = 14695981039346656037ULL;
	for (size_t i = 0; i < len; i++) {
		h ^= ((const uint8_t*)data)[i];
		h *= 1099511628211ULL;
	}
	return h;
}

static bool CompileOrLoadCached(
    const char* source, size_t sourceLen, const char* entry, const char* target,
    DWORD flags, ID3DBlob** outBlob)
{
	uint64_t srcHash = FnvHash(source, sourceLen);
	uint64_t entryHash = FnvHash(entry, strlen(entry));
	uint64_t flagsHash = FnvHash(&flags, sizeof(flags));
	uint64_t cacheKey = srcHash ^ (entryHash * 31) ^ (flagsHash * 997);

	namespace fs = std::filesystem;
	wchar_t dllPath[MAX_PATH] = {};
	HMODULE hm = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	    (LPCWSTR)&CompileOrLoadCached, &hm);
	GetModuleFileNameW(hm, dllPath, MAX_PATH);
	fs::path cacheDir = fs::path(dllPath).parent_path() / ".shader_cache";
	char cacheName[64];
	snprintf(cacheName, sizeof(cacheName), "%s_%016llx.cso", entry, (unsigned long long)cacheKey);
	fs::path cachePath = cacheDir / cacheName;

	if (fs::exists(cachePath)) {
		std::ifstream f(cachePath, std::ios::binary | std::ios::ate);
		if (f.is_open()) {
			auto sz = f.tellg();
			if (sz > 0) {
				f.seekg(0);
				if (SUCCEEDED(D3DCreateBlob((SIZE_T)sz, outBlob))) {
					f.read((char*)(*outBlob)->GetBufferPointer(), sz);
					if (f.good())
						return true;
					(*outBlob)->Release();
					*outBlob = nullptr;
				}
			}
		}
	}

	ID3DBlob* errs = nullptr;
	HRESULT hr = D3DCompile(source, sourceLen, entry, nullptr, nullptr, entry, target, flags, 0, outBlob, &errs);
	if (FAILED(hr)) {
		if (errs) {
			OOVR_LOGF("Shader compile error (%s): %s", entry, (char*)errs->GetBufferPointer());
			errs->Release();
		}
		return false;
	}
	if (errs) errs->Release();

	try {
		fs::create_directories(cacheDir);
		std::ofstream f(cachePath, std::ios::binary);
		if (f.is_open())
			f.write((const char*)(*outBlob)->GetBufferPointer(), (*outBlob)->GetBufferSize());
	} catch (...) {}
	return true;
}

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
#include "../../DrvOpenXR/DapaMaskBridge.h"

#include <MinHook.h>


// Resolve the submitted eye region from OpenVR's normalized texture bounds.
// A non-null bounds pointer only means that a region was supplied; it does not
// imply that the texture contains both eyes side-by-side. In particular,
// external submit-time upscalers may provide one full texture per eye together
// with explicit { 0, 0, 1, 1 } bounds.
static bool ResolveSubmittedTextureRegion(
    const D3D11_TEXTURE2D_DESC& desc,
    const vr::VRTextureBounds_t* bounds,
    D3D11_BOX& region)
{
	if (desc.Width == 0 || desc.Height == 0)
		return false;

	float minU = 0.0f;
	float maxU = 1.0f;
	float minV = 0.0f;
	float maxV = 1.0f;
	if (bounds) {
		if (!std::isfinite(bounds->uMin) || !std::isfinite(bounds->uMax)
		    || !std::isfinite(bounds->vMin) || !std::isfinite(bounds->vMax)) {
			return false;
		}

		minU = std::clamp(std::min(bounds->uMin, bounds->uMax), 0.0f, 1.0f);
		maxU = std::clamp(std::max(bounds->uMin, bounds->uMax), 0.0f, 1.0f);
		minV = std::clamp(std::min(bounds->vMin, bounds->vMax), 0.0f, 1.0f);
		maxV = std::clamp(std::max(bounds->vMin, bounds->vMax), 0.0f, 1.0f);
	}

	// Match the normal submit copy's truncation policy. Computing the span
	// independently from the offset also keeps both halves of an odd-width
	// side-by-side texture the same size.
	auto toPixel = [](float normalized, uint32_t dimension) {
		return static_cast<uint32_t>(normalized * static_cast<float>(dimension));
	};

	const uint32_t regionWidth = toPixel(maxU - minU, desc.Width);
	const uint32_t regionHeight = toPixel(maxV - minV, desc.Height);
	region.left = toPixel(minU, desc.Width);
	region.top = toPixel(minV, desc.Height);
	region.right = std::min(desc.Width, region.left + regionWidth);
	region.bottom = std::min(desc.Height, region.top + regionHeight);
	region.front = 0;
	region.back = 1;
	return region.right > region.left && region.bottom > region.top;
}

// ============================================================================
// SKSE Render Target Bridge — shared memory for motion vectors + depth
// ============================================================================
#pragma pack(push, 1)
struct OCRenderTargetBridge {
	static constexpr uint32_t MAGIC = 0x56544F4D; // 'MOTV'
	static constexpr uint32_t VERSION = 2;

	uint32_t magic;
	uint32_t version;
	uint32_t byteSize;
	uint32_t publishSequence; // Odd = resource writer active, even = stable
	uint32_t status; // 0=not ready, 1=ready, 2=error
	uint32_t resourceReaders; // Pins published COM pointers through reader AddRef
	uint64_t resourceGeneration;
	uint32_t mvWidth;
	uint32_t mvHeight;
	uint32_t depthWidth;
	uint32_t depthHeight;

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

	// Stencil capture — R24G8_TYPELESS copy captured mid-frame by SKSE plugin's
	// ClearDepthStencilView hook (before the game clears stencil).
	uint64_t stencilCaptureTexture; // ID3D11Texture2D* (R24G8_TYPELESS, same size as depth)
	uint8_t stencilCapturedThisFrame; // 1 = valid capture for current frame
	uint8_t _padStencil[7];

	// Player first-person model — NiAVObject nodes for hand bounding sphere detection.
	uint64_t playerFirstPersonRootPtr; // NiAVObject* → player's 1st-person skeleton root
	uint64_t playerFPLeftHandPtr;      // NiAVObject* → left hand node
	uint64_t playerFPRightHandPtr;     // NiAVObject* → right hand node
	uint64_t playerFPWeaponPtr;        // NiAVObject* → weapon node

	// First-person render pass detection.
	uint8_t fpRenderFinished; // 1 = FP render done (set by hook, reset by compositor)
	uint8_t _padFPRender[7];
	uint64_t finishAccumulatingAddr; // BSShaderAccumulator::FinishAccumulating function address

	// Per-draw-call stencil injection — marks FP pixels with stencil=2
	uint8_t  fpStencilInjectionActive; // 1 = SetupGeometry hook is running, stencil=2 marks FP pixels
	uint8_t  fpStencilInjectNow;       // 1 = currently inside FP draw (set by SKSE, read by OC MinHook)
	uint8_t  _padFPStencil[6];
	uint32_t fpStencilDrawCount;       // Number of FP draw calls this frame (diagnostic)
	uint32_t fpStencilDrawCountTotal;  // Cumulative FP draws (diagnostic)

	// Player-mask texture and its independent producer/consumer lease.
	uint64_t preFPDepthTexture;        // ID3D11Texture2D* (R32_FLOAT, same size as scene depth)
	uint8_t  preFPDepthCaptured;       // 1 = valid capture for current frame
	uint8_t  _padPreFP[3];             // [0] = mask protocol; immutable after bridge publication
	uint32_t maskAccessGate;

	// FP geometry pointers — BSGeometry* addresses for positively-identified FP draws.
	// OC reads their worldBound LIVE at WarpFrame time (no stale data).
	uint64_t fpGeomPointers[16];       // Up to 16 BSGeometry* pointers
	uint32_t fpGeomCount;              // Number of valid pointers
	uint32_t maskFrameGeneration;
	uint32_t maskConflictSerial;
	uint32_t maskFrameConflictBaseline;

	// FP draw replay — pointer to heap-allocated FPReplayData (same process, read by OC).
	uint64_t fpReplayDataPtr;          // FPReplayData* (cast to uint64_t)

	// Menu state — DAPA suspends synthetic frames while a gameplay menu is open.
	uint8_t  isMenuOpen;               // 1 = a gameplay menu is open, 0 = gameplay
	uint8_t  isConsoleOpen;            // 1 = the game console menu is open (VR keyboard overlay sync)
	uint8_t  _padMenu[6];              // alignment
};
#pragma pack(pop)
static_assert(sizeof(OCRenderTargetBridge) == 704);
static_assert(offsetof(OCRenderTargetBridge, publishSequence) % alignof(uint32_t) == 0);
static_assert(offsetof(OCRenderTargetBridge, resourceReaders) % alignof(uint32_t) == 0);
static_assert(offsetof(OCRenderTargetBridge, maskAccessGate) == 540);
static_assert(offsetof(OCRenderTargetBridge, maskFrameGeneration) == 676);
static_assert(offsetof(OCRenderTargetBridge, maskConflictSerial) == 680);
static_assert(offsetof(OCRenderTargetBridge, maskFrameConflictBaseline) == 684);

static PublishedBridge<OCRenderTargetBridge> s_pBridge;
static uint32_t s_dapaMaskCacheFrame = 0;
static uint32_t s_dapaMaskCacheConflicts = 0;

bool OCBridge_DapaMaskCacheValid()
{
	// Revalidate immediately before claiming a synthetic slot. A producer may
	// have reported a missed draw after the right-eye copy finished.
	DapaMaskBridge::Access<OCRenderTargetBridge> mask(
	    s_pBridge.Get(), DapaMaskBridge::AccessMode::Read);
	return mask.Clean() && mask.Frame() == s_dapaMaskCacheFrame &&
	    mask.ConflictSerial() == s_dapaMaskCacheConflicts;
}

static void OpenRenderTargetBridge();

// Real console state from the SKSE MenuOpenCloseEvent, via the bridge.
// Used by VRKeyboard to keep its console overlay in lockstep with the game
// instead of blind-toggling on tilde. Returns -1 when no bridge (non-Skyrim
// or plugin not connected yet) so the keyboard can fall back to its toggle.
// Deliberately does NOT require status == 1: that flag gates render-target
// readiness (motion vectors etc.), which has nothing to do with menu state.
int OCBridge_ConsoleState()
{
	OpenRenderTargetBridge(); // self-throttled; connects even when MV/FSR paths never ran
	if (!s_pBridge)
		return -1;
	return s_pBridge->isConsoleOpen ? 1 : 0;
}

// Read-only menu signal from the same bridge CSX/ASW already share. VRS uses
// it only as a safety gate: menus render at full shading rate. No bridge image,
// dimension, renderScale, depth, or motion-vector field is read or modified.
static int OCBridge_MenuState()
{
	OpenRenderTargetBridge();
	if (!s_pBridge)
		return -1;
	return s_pBridge->isMenuOpen ? 1 : 0;
}

bool OCBridge_DapaMenuPaused()
{
	OpenRenderTargetBridge();
	return s_pBridge && (s_pBridge->isMenuOpen != 0 ||
	    s_pBridge->isLoadingScreen != 0 || s_pBridge->isMainMenu != 0);
}

// Input eligibility is independent of render-resource readiness. Unknown menu
// state must not be treated as permission to synthesize locomotion.
int OCBridge_LocomotionState()
{
	OpenRenderTargetBridge();
	if (!s_pBridge) return -1;
	return s_pBridge->isMenuOpen || s_pBridge->isMainMenu ||
	    s_pBridge->isLoadingScreen || s_pBridge->isConsoleOpen ? 1 : 0;
}
static void OpenRenderTargetBridge()
{
	s_pBridge.Connect([]() -> OCRenderTargetBridge* {
		const HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE,
		    L"Local\\OpenCompositeRenderTargets");
		if (!mapping) return nullptr;

		auto* view = static_cast<OCRenderTargetBridge*>(
		    MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(OCRenderTargetBridge)));
		if (!view) {
			CloseHandle(mapping);
			return nullptr;
		}

		// The writer can still be initializing its header when the named mapping
		// first appears. Keep this view private until validation is complete.
		// Resource fields retain the existing v2 publish protocol below.
		if (view->magic != OCRenderTargetBridge::MAGIC ||
		    view->version != OCRenderTargetBridge::VERSION ||
		    view->byteSize < sizeof(OCRenderTargetBridge)) {
			OOVR_LOG("RT Bridge: Invalid magic/version/size — shared memory not ready or incompatible SKSE plugin");
			UnmapViewOfFile(view);
			CloseHandle(mapping);
			return nullptr;
		}

		// The mapped view retains the mapping object after its handle closes.
		// Keep the successful view alive for every input/render reader.
		CloseHandle(mapping);
		OOVR_LOG("RT Bridge: Connected to SKSE shared memory");
		return view;
	});
}

struct OCBridgeResourceSnapshot {
	uint32_t status = 0;
	uint64_t generation = 0;
	uint32_t mvWidth = 0;
	uint32_t mvHeight = 0;
	uint32_t depthWidth = 0;
	uint32_t depthHeight = 0;
	ID3D11Texture2D* mvTexture = nullptr;
	ID3D11ShaderResourceView* mvSRV = nullptr;
	ID3D11UnorderedAccessView* mvUAV = nullptr;
	ID3D11Texture2D* depthTexture = nullptr;
	ID3D11ShaderResourceView* depthSRV = nullptr;
	ID3D11Device* d3dDevice = nullptr;
	ID3D11DeviceContext* d3dContext = nullptr;

	OCBridgeResourceSnapshot() = default;
	OCBridgeResourceSnapshot(const OCBridgeResourceSnapshot&) = delete;
	OCBridgeResourceSnapshot& operator=(const OCBridgeResourceSnapshot&) = delete;

	~OCBridgeResourceSnapshot()
	{
		Reset();
	}

	void Reset()
	{
		if (mvUAV)
			mvUAV->Release();
		if (mvSRV)
			mvSRV->Release();
		if (depthSRV)
			depthSRV->Release();
		if (mvTexture)
			mvTexture->Release();
		if (depthTexture)
			depthTexture->Release();
		if (d3dContext)
			d3dContext->Release();
		if (d3dDevice)
			d3dDevice->Release();

		status = 0;
		generation = 0;
		mvWidth = 0;
		mvHeight = 0;
		depthWidth = 0;
		depthHeight = 0;
		mvTexture = nullptr;
		mvSRV = nullptr;
		mvUAV = nullptr;
		depthTexture = nullptr;
		depthSRV = nullptr;
		d3dDevice = nullptr;
		d3dContext = nullptr;
	}

	bool Ready() const
	{
		return status == 1 &&
		       mvTexture &&
		       d3dDevice &&
		       d3dContext &&
		       mvWidth != 0 &&
		       mvHeight != 0 &&
		       (!depthTexture || (depthWidth != 0 && depthHeight != 0));
	}
};

static thread_local const OCBridgeResourceSnapshot* s_scopedBridgeResourceSnapshot = nullptr;

class ScopedBridgeResourceSnapshot
{
public:
	explicit ScopedBridgeResourceSnapshot(const OCBridgeResourceSnapshot* snapshot) :
		previous(s_scopedBridgeResourceSnapshot)
	{
		// Deliberately scope an empty snapshot after acquisition failure. Nested
		// calls must not acquire a newer generation halfway through one eye submit.
		s_scopedBridgeResourceSnapshot = snapshot;
	}

	~ScopedBridgeResourceSnapshot()
	{
		s_scopedBridgeResourceSnapshot = previous;
	}

	ScopedBridgeResourceSnapshot(const ScopedBridgeResourceSnapshot&) = delete;
	ScopedBridgeResourceSnapshot& operator=(const ScopedBridgeResourceSnapshot&) = delete;

private:
	const OCBridgeResourceSnapshot* previous;
};

static uint32_t ReadBridgeAtomic(const uint32_t& value)
{
	auto* atomicValue = reinterpret_cast<volatile LONG*>(const_cast<uint32_t*>(&value));
	return static_cast<uint32_t>(InterlockedCompareExchange(atomicValue, 0, 0));
}

template <class T>
static T* RetainBridgeSnapshotResource(uint64_t address)
{
	auto* resource = reinterpret_cast<T*>(static_cast<uintptr_t>(address));
	if (resource)
		resource->AddRef();
	return resource;
}

static bool AcquireBridgeResourceSnapshot(OCBridgeResourceSnapshot& snapshot)
{
	snapshot.Reset();
	OpenRenderTargetBridge();
	if (!s_pBridge)
		return false;

	for (uint32_t attempt = 0; attempt < 8; ++attempt) {
		const uint32_t beforeSequence = ReadBridgeAtomic(s_pBridge->publishSequence);
		if ((beforeSequence & 1u) != 0) {
			SwitchToThread();
			continue;
		}

		InterlockedIncrement(reinterpret_cast<volatile LONG*>(&s_pBridge->resourceReaders));
		MemoryBarrier();
		const uint32_t pinnedSequence = ReadBridgeAtomic(s_pBridge->publishSequence);
		if (pinnedSequence != beforeSequence || (pinnedSequence & 1u) != 0) {
			InterlockedDecrement(reinterpret_cast<volatile LONG*>(&s_pBridge->resourceReaders));
			continue;
		}

		// The writer cannot replace or release this generation until resourceReaders
		// returns to zero, so AddRef is safe even if publication starts concurrently.
		snapshot.status = s_pBridge->status;
		snapshot.generation = s_pBridge->resourceGeneration;
		snapshot.mvWidth = s_pBridge->mvWidth;
		snapshot.mvHeight = s_pBridge->mvHeight;
		snapshot.depthWidth = s_pBridge->depthWidth;
		snapshot.depthHeight = s_pBridge->depthHeight;
		snapshot.mvTexture = RetainBridgeSnapshotResource<ID3D11Texture2D>(s_pBridge->mvTexture);
		snapshot.mvSRV = RetainBridgeSnapshotResource<ID3D11ShaderResourceView>(s_pBridge->mvSRV);
		snapshot.mvUAV = RetainBridgeSnapshotResource<ID3D11UnorderedAccessView>(s_pBridge->mvUAV);
		snapshot.depthTexture = RetainBridgeSnapshotResource<ID3D11Texture2D>(s_pBridge->depthTexture);
		snapshot.depthSRV = RetainBridgeSnapshotResource<ID3D11ShaderResourceView>(s_pBridge->depthSRV);
		snapshot.d3dDevice = RetainBridgeSnapshotResource<ID3D11Device>(s_pBridge->d3dDevice);
		snapshot.d3dContext = RetainBridgeSnapshotResource<ID3D11DeviceContext>(s_pBridge->d3dContext);
		MemoryBarrier();
		InterlockedDecrement(reinterpret_cast<volatile LONG*>(&s_pBridge->resourceReaders));
		return true;
	}

	return false;
}

// OCU ASW — PC-side Asynchronous SpaceWarp (global g_aswProvider in ASWProvider.h)

static const OCBridgeResourceSnapshot& ReuseOrAcquireBridgeResourceSnapshot(
    OCBridgeResourceSnapshot& localSnapshot,
    bool acquireIfUnscoped = true)
{
	if (s_scopedBridgeResourceSnapshot)
		return *s_scopedBridgeResourceSnapshot;

	if (acquireIfUnscoped)
		AcquireBridgeResourceSnapshot(localSnapshot);
	else
		localSnapshot.Reset();
	return localSnapshot;
}

#if defined(OC_HAS_FSR3) || defined(OC_HAS_DLSS)
#ifdef OC_HAS_FSR3
static Fsr3Upscaler* s_fsr3Upscaler = nullptr;
static std::chrono::steady_clock::time_point s_fsr3LastFrameTime;
static float s_fsr3CameraFovY = 1.57f; // Radians, updated from XR view each frame
#endif
static bool s_fsr3FirstDispatch = true;
static float s_fsr3RenderJitterX = 0.0f; // Jitter that was applied to current frame's rendering
static float s_fsr3RenderJitterY = 0.0f;
static uint8_t s_temporalJitterSubmittedEyeMask = 0;
static uint8_t s_bridgeTemporalResetEyeMask = 0;
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
static ID3D11ShaderResourceView* s_reactiveMaskColorSRV = nullptr;
static ID3D11Texture2D* s_reactiveMaskColorSRVTex = nullptr;
static ID3D11Texture2D* s_reactiveMaskColorTex = nullptr;
static uint32_t s_reactiveMaskColorW = 0;
static uint32_t s_reactiveMaskColorH = 0;
static DXGI_FORMAT s_reactiveMaskColorFmt = DXGI_FORMAT_UNKNOWN;
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
// Scene-target hooks used by fixed and eye-tracked foveation.
static bool s_renderTargetHooksInstalled = false;
static bool s_renderTargetHooksUsable = false;
using OMSetRT_fn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
static OMSetRT_fn s_origOMSetRT = nullptr;
// VRS is armed at WaitGetPoses, but only bound while Skyrim's actual stereo
// scene target is current. This prevents an atlas-sized resource from leaking
// into shadow, reflection, UI, and other differently sized render passes.
static VRSManager* s_vrsHookManager = nullptr;
static RDMRenderScope* s_densityMaskHookManager = nullptr;
static ID3D11Texture2D* s_vrsSceneTarget = nullptr; // observed game resource; not owned
static ID3D11DeviceContext* s_sceneHookContext = nullptr;
static ocu_vrs_scope::SceneScope s_vrsSceneScope;
static VRSAlphaCoverageScope s_vrsAlphaCoverageScope;
static std::uint32_t s_vrsSceneBindings = 0;
static bool s_vrsSceneEligible = false;
static std::uint32_t s_vrsProtectedBindings = 0;
static std::uint32_t s_vrsCoarseBindings = 0;
static std::uint32_t s_vrsUnclassifiedBindings = 0;
static std::uint32_t s_vrsTerrainDepthBindings = 0;
static void ResetVRSInputGeometry();
static bool s_vrsFrameArmed = false;
static bool s_vrsHookApplied = false;
static void SyncVRSForShaderState(ID3D11DeviceContext* ctx)
{
	if (!s_vrsHookManager || ctx != s_sceneHookContext) return;
	const auto reasons = ocu_vrs_guard::CurrentReasons(ctx);
	const auto coarseHazards = ocu_vrs_guard::CurrentCoarseHazards(ctx);
	const bool alphaCoverage = s_vrsAlphaCoverageScope.ProtectsCurrentDraw(ctx);
	const bool scene = s_vrsFrameArmed && s_vrsSceneEligible;
	const auto* viewports = ocu_vrs_guard::CurrentViewports(ctx);
	const bool viewportReady = scene && viewports &&
	    s_vrsHookManager->UpdateActiveViewports(viewports->count, viewports->values);
	const bool shouldApply = scene && viewportReady && reasons == ocu_vrs_guard::Compatible &&
	    coarseHazards == ocu_vrs_guard::CoarseCompatible && !alphaCoverage;
	if (scene && (reasons || coarseHazards || alphaCoverage)) {
		++s_vrsProtectedBindings;
		if ((reasons & ocu_vrs_guard::Unclassified) || (coarseHazards & ocu_vrs_guard::CoarseUnclassified))
			++s_vrsUnclassifiedBindings;
		if (coarseHazards & ocu_vrs_guard::RasterDepthTextureLoad) ++s_vrsTerrainDepthBindings;
	}
	if (shouldApply && !s_vrsHookApplied)
		s_vrsHookApplied = s_vrsHookManager->ApplyStereo();
	else if (!shouldApply && s_vrsHookApplied) {
		s_vrsHookManager->Disable();
		s_vrsHookApplied = false;
	}
	if (shouldApply && s_vrsHookApplied) ++s_vrsCoarseBindings;
}

static void SyncVRSForRenderTargets(ID3D11DeviceContext* ctx, UINT numViews,
    ID3D11RenderTargetView* const* ppRTVs, ID3D11DepthStencilView* dsv)
{
	if (!s_vrsHookManager || ctx != s_sceneHookContext) return;
	s_vrsSceneEligible = s_vrsFrameArmed && s_vrsSceneScope.Matches(ctx, numViews, ppRTVs, dsv);
	if (s_vrsSceneEligible) ++s_vrsSceneBindings;
	SyncVRSForShaderState(ctx);
}

static void VRSShaderStateChanged(ID3D11DeviceContext* ctx, bool targetsChanged)
{
	if (!s_vrsHookManager || ctx != s_sceneHookContext) return;
	s_vrsAlphaCoverageScope.ShaderStateChanged(ctx, targetsChanged);
	if (!targetsChanged) {
		SyncVRSForShaderState(ctx);
		return;
	}
	// ClearState, command-list playback and context-state swaps can bypass the
	// individual target/shader setters and can change driver extension state.
	if (s_vrsHookApplied) s_vrsHookManager->Disable();
	s_vrsHookApplied = false;
	ID3D11RenderTargetView* views[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
	Microsoft::WRL::ComPtr<ID3D11DepthStencilView> depth;
	ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, views, &depth);
	SyncVRSForRenderTargets(ctx, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, views, depth.Get());
	for (auto* view : views) if (view) view->Release();
}

static void DisarmSceneVRS()
{
	if (s_vrsHookManager && s_vrsHookApplied)
		s_vrsHookManager->Disable();
	s_vrsFrameArmed = false;
	s_vrsAlphaCoverageScope.EndFrame();
	s_vrsHookApplied = false;
	s_vrsSceneScope.Reset();
	s_vrsSceneEligible = false;
	ocu_vrs_guard::UnwatchContext(s_sceneHookContext);
	if (s_densityMaskHookManager)
		s_densityMaskHookManager->EndFrame();
	s_densityMaskHookManager = nullptr;
}

static void STDMETHODCALLTYPE Hook_OMSetRenderTargets(
    ID3D11DeviceContext* ctx, UINT numViews, ID3D11RenderTargetView* const* ppRTVs, ID3D11DepthStencilView* pDSV)
{
	RDMRenderScope::NotifyBeforeDepthStateBoundary(ctx);
	s_origOMSetRT(ctx, numViews, ppRTVs, pDSV);
	if (ctx != s_sceneHookContext) return;
	SyncVRSForRenderTargets(ctx, numViews, ppRTVs, pDSV);
	RDMRenderScope::NotifyTargets(ctx);
}

static bool InstallSceneTargetHooks(ID3D11Device* device)
{
	if (s_renderTargetHooksInstalled) return s_renderTargetHooksUsable;

	ID3D11DeviceContext* ctx = nullptr;
	device->GetImmediateContext(&ctx);
	if (!ctx) return false;

	auto vtable = *reinterpret_cast<void***>(ctx);

	MH_STATUS st = MH_Initialize();
	if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
		OOVR_LOGF("FPDepth: MH_Initialize failed (%d)", (int)st);
		ctx->Release();
		return false;
	}

	// Hook OMSetRenderTargets (vtable slot 33)
	st = MH_CreateHook(vtable[33], (void*)&Hook_OMSetRenderTargets, (void**)&s_origOMSetRT);
	if (st == MH_OK) st = MH_EnableHook(vtable[33]);
	const bool omRTHookOk = st == MH_OK;
	if (st != MH_OK) {
		OOVR_LOGF("FPDepth: OMSetRenderTargets hook failed (%d)", (int)st);
	} else {
		OOVR_LOGF("FPDepth: OMSetRenderTargets hooked at %p", vtable[33]);
	}

	// Also hook OMSetRenderTargetsAndUnorderedAccessViews (vtable slot 34)
	// — many games use this variant instead of slot 33
	using OMSetRTUAV_fn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*,
	    ID3D11DepthStencilView*, UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*);
	static OMSetRTUAV_fn s_origOMSetRTUAV = nullptr;
	// We'll forward to the same logic via a wrapper that extracts the DSV
	struct RTUAVHook {
		static void STDMETHODCALLTYPE Hook(ID3D11DeviceContext* ctx, UINT numRTVs,
		    ID3D11RenderTargetView* const* ppRTVs, ID3D11DepthStencilView* pDSV,
		    UINT uavStart, UINT numUAVs, ID3D11UnorderedAccessView* const* ppUAVs, const UINT* pInitial)
		{
			RDMRenderScope::NotifyBeforeDepthStateBoundary(ctx);
			s_origOMSetRTUAV(ctx, numRTVs, ppRTVs, pDSV, uavStart, numUAVs, ppUAVs, pInitial);
			if (ctx != s_sceneHookContext) return;
			ocu_vrs_scope::WithRenderTargets(ctx, numRTVs, ppRTVs, pDSV,
			    [ctx](UINT count, ID3D11RenderTargetView* const* views, ID3D11DepthStencilView* depth) {
				    SyncVRSForRenderTargets(ctx, count, views, depth);
				    RDMRenderScope::NotifyTargets(ctx);
			    });
		}
	};
	st = MH_CreateHook(vtable[34], (void*)&RTUAVHook::Hook, (void**)&s_origOMSetRTUAV);
	if (st == MH_OK) st = MH_EnableHook(vtable[34]);
	const bool omRTUAVHookOk = st == MH_OK;
	if (st != MH_OK) {
		OOVR_LOGF("FPDepth: OMSetRTAndUAV hook failed (%d)", (int)st);
	} else {
		OOVR_LOGF("FPDepth: OMSetRTAndUAV hooked at %p", vtable[34]);
	}

	s_renderTargetHooksInstalled = true;
	s_renderTargetHooksUsable = omRTHookOk && omRTUAVHookOk;
    if (s_renderTargetHooksUsable)
        RDMRenderScope::RegisterTargetObserver(vtable[33], vtable[34]);
	ctx->Release();
	return s_renderTargetHooksUsable;
}

#if defined(OC_HAS_FSR3) || defined(OC_HAS_DLSS)
// ── Reactive mask compute shader ──
// Depth-edge detection: adds reactiveness at object silhouettes (tree branches
// against sky) to reduce FSR3/DLSS temporal ghosting on thin geometry.
static constexpr char s_reactiveMaskHLSL[] = R"HLSL(
Texture2D<float>       DepthIn     : register(t0);
Texture2D<float4>      ColorIn     : register(t1);
RWTexture2D<float>     ReactiveOut : register(u0);

cbuffer ReactiveCB : register(b0) {
    float4 edgeParams;       // base, depthEdgeBoost, depthEdgeThreshold, depthEdgeScale
    float4 depthColorParams; // depthFalloffStart, depthFalloffEnd, colorBoost, colorThreshold
    float4 colorParams;      // colorScale, colorWidth, colorHeight, unused
    int4   depthOffsetPx;    // depthOffset.xy, unused.zw
};

float Luma(float3 c) {
    return dot(c, float3(0.299, 0.587, 0.114));
}

[numthreads(8, 8, 1)]
void CS_ReactiveMask(uint3 id : SV_DispatchThreadID)
{
    uint w, h;
    ReactiveOut.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;

    int2 depthCoord = int2(id.xy) + depthOffsetPx.xy;
    float center = DepthIn.Load(int3(depthCoord, 0));
    float maxDiff = 0.0;
    static const int2 depthOffsets[8] = {
        int2(-1, 0), int2(1, 0), int2(0,-1), int2(0, 1),
        int2(-3, 0), int2(3, 0), int2(0,-3), int2(0, 3),
    };
    [unroll] for (int i = 0; i < 8; i++) {
        int2 coord = clamp(depthCoord + depthOffsets[i], depthOffsetPx.xy, depthOffsetPx.xy + int2(w-1, h-1));
        maxDiff = max(maxDiff, abs(center - DepthIn.Load(int3(coord, 0))));
    }
    float edge = saturate((maxDiff - edgeParams.z) * edgeParams.w) * edgeParams.y;

    float colorEdge = 0.0;
    uint colorW = (uint)colorParams.y;
    uint colorH = (uint)colorParams.z;
    if (depthColorParams.z > 0.0 && colorW > 0 && colorH > 0) {
        float2 colorScale = float2((float)colorW / (float)w, (float)colorH / (float)h);
        int2 cxy = clamp(int2((float2(id.xy) + 0.5) * colorScale), int2(0, 0), int2(colorW - 1, colorH - 1));
        float centerL = Luma(ColorIn.Load(int3(cxy, 0)).rgb);
        float maxLumaDiff = 0.0;
        static const int2 colorOffsets[8] = {
            int2(-1, 0), int2(1, 0), int2(0,-1), int2(0, 1),
            int2(-2, 0), int2(2, 0), int2(0,-2), int2(0, 2),
        };
        [unroll] for (int i = 0; i < 8; i++) {
            int2 coord = clamp(cxy + colorOffsets[i], int2(0, 0), int2(colorW - 1, colorH - 1));
            maxLumaDiff = max(maxLumaDiff, abs(centerL - Luma(ColorIn.Load(int3(coord, 0)).rgb)));
        }
        colorEdge = saturate((maxLumaDiff - depthColorParams.w) * colorParams.x) * depthColorParams.z;
    }

    // Distance falloff: reduce bias for distant pixels so the upscaler trusts
    // history more, counteracting jitter-induced wobble on mountains/landscapes.
    // Standard-Z: higher depth = farther. Falloff ramps from 1 to 0 between start..end.
    float distFade = (depthColorParams.y > depthColorParams.x)
        ? 1.0 - saturate((center - depthColorParams.x) / (depthColorParams.y - depthColorParams.x))
        : 1.0;

    ReactiveOut[id.xy] = min((edgeParams.x + edge + colorEdge) * distFade, 0.95);
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
static ID3D11Texture2D* s_cameraMVResidualTex = nullptr;
static ID3D11UnorderedAccessView* s_cameraMVResidualUAV = nullptr;
static ID3D11Texture2D* s_cameraMVFallbackMaskTex = nullptr;
static ID3D11UnorderedAccessView* s_cameraMVFallbackMaskUAV = nullptr;
static ID3D11Buffer* s_cameraMVStatsBuffer = nullptr;
static ID3D11UnorderedAccessView* s_cameraMVStatsUAV = nullptr;
static ID3D11Buffer* s_cameraMVStatsReadback = nullptr;
static ID3D11ShaderResourceView* s_cameraMVDepthSRV = nullptr;
static ID3D11Texture2D* s_cameraMVDepthSRVTex = nullptr;
static ID3D11Buffer* s_cameraMVCB = nullptr;
static uint32_t s_cameraMVW = 0, s_cameraMVH = 0;

// Per-eye previous VP matrix for camera MV computation (column-major float[16])
static float s_prevVP[2][16] = {};
static bool s_hasPrevVP[2] = { false, false };

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
RWTexture2D<float2>    ResidualOut : register(u1);
RWTexture2D<float>     FallbackMaskOut : register(u2);
RWStructuredBuffer<uint> StatsOut : register(u3);
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
    uint   enableStats;
    float3 _pad2;
};

[numthreads(8, 8, 1)]
void CS_CameraMV(uint3 id : SV_DispatchThreadID)
{
    uint w, h;
    MVOut.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    if (enableStats != 0) InterlockedAdd(StatsOut[7], 1);

    float depth = DepthIn.Load(int3(int2(id.xy) + depthOffset, 0));

    if (depth < 0.0001) {
        MVOut[id.xy] = float2(0, 0);
        ResidualOut[id.xy] = float2(0, 0);
        FallbackMaskOut[id.xy] = 0.0;
        if (enableStats != 0) InterlockedAdd(StatsOut[0], 1);
        return;
    }
    if (enableStats != 0) InterlockedAdd(StatsOut[1], 1);

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
    float2 residualMV = float2(0, 0);
    float fallbackMask = 0.0;
    float cameraMag2 = dot(fullCameraMV, fullCameraMV);
    if (enableStats != 0 && cameraMag2 > 1.0e-10) InterlockedAdd(StatsOut[2], 1);

    if (useGameMV) {
        // Camera-first composition: reconstructed camera MVs are the authoritative
        // static-world motion for head tracking, stick turns, and locomotion. Use
        // Skyrim's bridge MV only when it contains a meaningful residual beyond
        // that camera motion (NPCs, first-person objects, close animated foliage).
        // This avoids replacing accurate depth-derived head motion with a noisy or
        // non-local bridge vector on mid-distance alpha foliage.
        float2 gameMV = GameMVIn.Load(int3(int2(id.xy) + gameMVOffset, 0));
        float gameMag2 = dot(gameMV, gameMV);
        float2 gameResidual = gameMV - fullCameraMV;
        float residualMag2 = dot(gameResidual, gameResidual);
        float residualThreshold = max(1.0e-10, cameraMag2 * 0.0025);
        bool bridgeHasMV = (gameMag2 > 1.0e-10);
        bool gameMVUsable = bridgeHasMV && (residualMag2 > residualThreshold);
        if (enableStats != 0 && gameMag2 <= 1.0e-10) InterlockedAdd(StatsOut[3], 1);
        if (gameMVUsable) {
            mv = gameMV;
            residualMV = gameResidual;
            if (enableStats != 0) InterlockedAdd(StatsOut[5], 1);
        } else if (cameraMag2 > 1.0e-10) {
            fallbackMask = 1.0;
            if (enableStats != 0) {
                if (bridgeHasMV) InterlockedAdd(StatsOut[4], 1);
                InterlockedAdd(StatsOut[6], 1);
            }
        }
    }

    mv.x += jitterDeltaUV.x;
    mv.y -= jitterDeltaUV.y;

    MVOut[id.xy] = mv;
    ResidualOut[id.xy] = residualMV;
    FallbackMaskOut[id.xy] = fallbackMask;
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
		if (!CompileOrLoadCached(ocu_depth_extract::Shader, sizeof(ocu_depth_extract::Shader) - 1,
		        "CS_DepthExtract", "cs_5_0", 0, &blob))
			return false;
		HRESULT hr = device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &s_depthExtractCS);
		blob->Release();
		if (FAILED(hr)) {
			OOVR_LOGF("DepthExtract: CreateComputeShader failed (hr=0x%08X)", hr);
			return false;
		}
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
		OCBridgeResourceSnapshot bridgeResources;
		const auto& activeBridgeResources = ReuseOrAcquireBridgeResourceSnapshot(bridgeResources);
		if (activeBridgeResources.Ready() &&
		    activeBridgeResources.depthTexture == depthTex &&
		    activeBridgeResources.depthSRV) {
			s_depthBridgeSRV = activeBridgeResources.depthSRV;
			s_depthBridgeSRV->AddRef();
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
	return ocu_depth_extract::Dispatch(context, s_depthExtractCS, depthSRV,
	    s_depthR32FUAV, width, height);
}

// ── Stencil extraction ──

static bool EnsureReactiveMaskResources(ID3D11Device* device, uint32_t w, uint32_t h)
{
	if (!s_reactiveMaskCS) {
		ID3DBlob* blob = nullptr;
		if (!CompileOrLoadCached(s_reactiveMaskHLSL, sizeof(s_reactiveMaskHLSL) - 1,
		        "CS_ReactiveMask", "cs_5_0", 0, &blob))
			return false;
		HRESULT hr = device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &s_reactiveMaskCS);
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
		bd.ByteWidth = 64;
		bd.Usage = D3D11_USAGE_DYNAMIC;
		bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		HRESULT hr = device->CreateBuffer(&bd, nullptr, &s_reactiveMaskCB);
		if (FAILED(hr))
			return false;
	}
	return true;
}

static bool EnsureReactiveMaskColorCopyResources(ID3D11Device* device, uint32_t w, uint32_t h,
    DXGI_FORMAT format)
{
	if (s_reactiveMaskColorTex && s_reactiveMaskColorW == w && s_reactiveMaskColorH == h
	    && s_reactiveMaskColorFmt == format)
		return true;

	if (s_reactiveMaskColorSRV) {
		s_reactiveMaskColorSRV->Release();
		s_reactiveMaskColorSRV = nullptr;
	}
	s_reactiveMaskColorSRVTex = nullptr;
	if (s_reactiveMaskColorTex) {
		s_reactiveMaskColorTex->Release();
		s_reactiveMaskColorTex = nullptr;
	}

	D3D11_TEXTURE2D_DESC td = {};
	td.Width = w;
	td.Height = h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = format;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

	HRESULT hr = device->CreateTexture2D(&td, nullptr, &s_reactiveMaskColorTex);
	if (FAILED(hr)) {
		static int s_log = 0;
		if (s_log++ < 5)
			OOVR_LOGF("ReactiveMask: Create color copy %ux%u fmt=%u failed hr=0x%08X",
			    w, h, format, hr);
		return false;
	}

	s_reactiveMaskColorW = w;
	s_reactiveMaskColorH = h;
	s_reactiveMaskColorFmt = format;
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

static ID3D11ShaderResourceView* GetOrCreateReactiveMaskColorSRV(ID3D11Device* device, ID3D11Texture2D* colorTex)
{
	if (!device || !colorTex)
		return nullptr;
	if (s_reactiveMaskColorSRVTex == colorTex && s_reactiveMaskColorSRV)
		return s_reactiveMaskColorSRV;
	if (s_reactiveMaskColorSRV) {
		s_reactiveMaskColorSRV->Release();
		s_reactiveMaskColorSRV = nullptr;
	}
	s_reactiveMaskColorSRVTex = nullptr;

	D3D11_TEXTURE2D_DESC td = {};
	colorTex->GetDesc(&td);

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = 1;
	srvDesc.Texture2D.MostDetailedMip = 0;
	switch (td.Format) {
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		break;
	case DXGI_FORMAT_B8G8R8A8_TYPELESS:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
		srvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		break;
	case DXGI_FORMAT_B8G8R8X8_TYPELESS:
	case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
		srvDesc.Format = DXGI_FORMAT_B8G8R8X8_UNORM;
		break;
	case DXGI_FORMAT_R10G10B10A2_TYPELESS:
		srvDesc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
		break;
	case DXGI_FORMAT_R16G16B16A16_TYPELESS:
		srvDesc.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
		break;
	default:
		srvDesc.Format = td.Format;
		break;
	}

	HRESULT hr = device->CreateShaderResourceView(colorTex, &srvDesc, &s_reactiveMaskColorSRV);
	if (FAILED(hr)) {
		static int s_log = 0;
		if (s_log++ < 5)
			OOVR_LOGF("ReactiveMask: Create color SRV failed texFmt=%u viewFmt=%u hr=0x%08X",
			    td.Format, srvDesc.Format, hr);
		return nullptr;
	}
	s_reactiveMaskColorSRVTex = colorTex;
	return s_reactiveMaskColorSRV;
}

static bool GenerateReactiveMask(ID3D11DeviceContext* context, ID3D11ShaderResourceView* depthSRV,
    ID3D11ShaderResourceView* colorSRV, uint32_t width, uint32_t height,
    uint32_t colorWidth, uint32_t colorHeight, int depthOffsetX, int depthOffsetY,
    float baseReactiveness, float edgeBoost, float edgeThreshold, float edgeScale,
    float colorBoost, float colorThreshold, float colorScale,
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
		cb[6] = (colorSRV && colorWidth > 0 && colorHeight > 0) ? colorBoost : 0.0f;
		cb[7] = colorThreshold;
		cb[8] = colorScale;
		cb[9] = (float)colorWidth;
		cb[10] = (float)colorHeight;
		cb[11] = 0.0f;
		int* icb = reinterpret_cast<int*>(cb);
		icb[12] = depthOffsetX;
		icb[13] = depthOffsetY;
		icb[14] = 0;
		icb[15] = 0;
		context->Unmap(s_reactiveMaskCB, 0);
	}

	ID3D11ComputeShader* oldCS = nullptr;
	ID3D11ShaderResourceView* oldSRVs[2] = {};
	ID3D11UnorderedAccessView* oldUAV = nullptr;
	ID3D11Buffer* oldCB = nullptr;
	context->CSGetShader(&oldCS, nullptr, nullptr);
	context->CSGetShaderResources(0, 2, oldSRVs);
	context->CSGetUnorderedAccessViews(0, 1, &oldUAV);
	context->CSGetConstantBuffers(0, 1, &oldCB);

	ID3D11ShaderResourceView* srvs[2] = { depthSRV, colorSRV };
	context->CSSetShader(s_reactiveMaskCS, nullptr, 0);
	context->CSSetShaderResources(0, 2, srvs);
	context->CSSetUnorderedAccessViews(0, 1, &s_reactiveMaskUAV, nullptr);
	context->CSSetConstantBuffers(0, 1, &s_reactiveMaskCB);
	context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);

	context->CSSetShader(oldCS, nullptr, 0);
	context->CSSetShaderResources(0, 2, oldSRVs);
	context->CSSetUnorderedAccessViews(0, 1, &oldUAV, nullptr);
	context->CSSetConstantBuffers(0, 1, &oldCB);
	if (oldCS)
		oldCS->Release();
	for (auto* oldSRV : oldSRVs)
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
		if (!CompileOrLoadCached(s_cameraMVHLSL, sizeof(s_cameraMVHLSL) - 1,
		        "CS_CameraMV", "cs_5_0", 0, &blob))
			return false;
		HRESULT hr = device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &s_cameraMVCS);
		blob->Release();
		if (FAILED(hr)) {
			OOVR_LOGF("CameraMV: CreateComputeShader failed (hr=0x%08X)", hr);
			return false;
		}
	}

	// Create/recreate R16G16_FLOAT output texture if dimensions changed
	if (!s_cameraMVTex || !s_cameraMVResidualTex || !s_cameraMVFallbackMaskTex || s_cameraMVW != w || s_cameraMVH != h) {
		if (s_cameraMVUAV) {
			s_cameraMVUAV->Release();
			s_cameraMVUAV = nullptr;
		}
		if (s_cameraMVTex) {
			s_cameraMVTex->Release();
			s_cameraMVTex = nullptr;
		}
		if (s_cameraMVResidualUAV) {
			s_cameraMVResidualUAV->Release();
			s_cameraMVResidualUAV = nullptr;
		}
		if (s_cameraMVResidualTex) {
			s_cameraMVResidualTex->Release();
			s_cameraMVResidualTex = nullptr;
		}
		if (s_cameraMVFallbackMaskUAV) {
			s_cameraMVFallbackMaskUAV->Release();
			s_cameraMVFallbackMaskUAV = nullptr;
		}
		if (s_cameraMVFallbackMaskTex) {
			s_cameraMVFallbackMaskTex->Release();
			s_cameraMVFallbackMaskTex = nullptr;
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
		hr = device->CreateTexture2D(&td, nullptr, &s_cameraMVResidualTex);
		if (FAILED(hr)) {
			OOVR_LOGF("CameraMV: CreateResidualTexture2D(%ux%u) failed (hr=0x%08X)", w, h, hr);
			s_cameraMVTex->Release();
			s_cameraMVTex = nullptr;
			return false;
		}
		D3D11_TEXTURE2D_DESC maskTd = td;
		maskTd.Format = DXGI_FORMAT_R16_FLOAT;
		hr = device->CreateTexture2D(&maskTd, nullptr, &s_cameraMVFallbackMaskTex);
		if (FAILED(hr)) {
			OOVR_LOGF("CameraMV: CreateFallbackMaskTexture2D(%ux%u) failed (hr=0x%08X)", w, h, hr);
			s_cameraMVTex->Release();
			s_cameraMVTex = nullptr;
			s_cameraMVResidualTex->Release();
			s_cameraMVResidualTex = nullptr;
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
		hr = device->CreateUnorderedAccessView(s_cameraMVResidualTex, &uavDesc, &s_cameraMVResidualUAV);
		if (FAILED(hr)) {
			OOVR_LOGF("CameraMV: CreateResidualUAV failed (hr=0x%08X)", hr);
			s_cameraMVUAV->Release();
			s_cameraMVUAV = nullptr;
			s_cameraMVTex->Release();
			s_cameraMVTex = nullptr;
			s_cameraMVResidualTex->Release();
			s_cameraMVResidualTex = nullptr;
			return false;
		}
		D3D11_UNORDERED_ACCESS_VIEW_DESC maskUavDesc = {};
		maskUavDesc.Format = DXGI_FORMAT_R16_FLOAT;
		maskUavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		hr = device->CreateUnorderedAccessView(s_cameraMVFallbackMaskTex, &maskUavDesc, &s_cameraMVFallbackMaskUAV);
		if (FAILED(hr)) {
			OOVR_LOGF("CameraMV: CreateFallbackMaskUAV failed (hr=0x%08X)", hr);
			s_cameraMVUAV->Release();
			s_cameraMVUAV = nullptr;
			s_cameraMVResidualUAV->Release();
			s_cameraMVResidualUAV = nullptr;
			s_cameraMVTex->Release();
			s_cameraMVTex = nullptr;
			s_cameraMVResidualTex->Release();
			s_cameraMVResidualTex = nullptr;
			s_cameraMVFallbackMaskTex->Release();
			s_cameraMVFallbackMaskTex = nullptr;
			return false;
		}
		s_cameraMVW = w;
		s_cameraMVH = h;
		OOVR_LOGF("CameraMV: R16G16_FLOAT output + residual + fallback mask %ux%u created", w, h);
	}

	if (!s_cameraMVStatsBuffer || !s_cameraMVStatsUAV || !s_cameraMVStatsReadback) {
		if (s_cameraMVStatsUAV) {
			s_cameraMVStatsUAV->Release();
			s_cameraMVStatsUAV = nullptr;
		}
		if (s_cameraMVStatsBuffer) {
			s_cameraMVStatsBuffer->Release();
			s_cameraMVStatsBuffer = nullptr;
		}
		if (s_cameraMVStatsReadback) {
			s_cameraMVStatsReadback->Release();
			s_cameraMVStatsReadback = nullptr;
		}

		D3D11_BUFFER_DESC bd = {};
		bd.ByteWidth = 8 * sizeof(uint32_t);
		bd.Usage = D3D11_USAGE_DEFAULT;
		bd.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		bd.StructureByteStride = sizeof(uint32_t);
		HRESULT hr = device->CreateBuffer(&bd, nullptr, &s_cameraMVStatsBuffer);
		if (FAILED(hr)) {
			OOVR_LOGF("CameraMV: CreateStatsBuffer failed (hr=0x%08X)", hr);
			return false;
		}

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
		uavDesc.Format = DXGI_FORMAT_UNKNOWN;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uavDesc.Buffer.NumElements = 8;
		hr = device->CreateUnorderedAccessView(s_cameraMVStatsBuffer, &uavDesc, &s_cameraMVStatsUAV);
		if (FAILED(hr)) {
			OOVR_LOGF("CameraMV: CreateStatsUAV failed (hr=0x%08X)", hr);
			return false;
		}

		D3D11_BUFFER_DESC rb = bd;
		rb.Usage = D3D11_USAGE_STAGING;
		rb.BindFlags = 0;
		rb.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		hr = device->CreateBuffer(&rb, nullptr, &s_cameraMVStatsReadback);
		if (FAILED(hr)) {
			OOVR_LOGF("CameraMV: CreateStatsReadback failed (hr=0x%08X)", hr);
			return false;
		}
	}

	// Create constant buffer once (128 bytes = 8 * 16)
	if (!s_cameraMVCB) {
		D3D11_BUFFER_DESC bd = {};
		bd.ByteWidth = 128; // clipToClip + render/depth/jitter/MV params + stats flag
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

static void LogCameraMVStats(ID3D11DeviceContext* context, uint32_t outputW, uint32_t outputH, int eye)
{
	if (!s_cameraMVStatsBuffer || !s_cameraMVStatsReadback)
		return;

	context->CopyResource(s_cameraMVStatsReadback, s_cameraMVStatsBuffer);

	D3D11_MAPPED_SUBRESOURCE mapped = {};
	HRESULT hr = context->Map(s_cameraMVStatsReadback, 0, D3D11_MAP_READ, 0, &mapped);
	if (FAILED(hr) || !mapped.pData)
		return;

	const uint32_t* s = reinterpret_cast<const uint32_t*>(mapped.pData);
	uint32_t invalidDepth = s[0];
	uint32_t validDepth = s[1];
	uint32_t cameraActive = s[2];
	uint32_t bridgeZero = s[3];
	uint32_t bridgeTooWeak = s[4];
	uint32_t bridgeUsed = s[5];
	uint32_t fallbackUsed = s[6];
	uint32_t total = s[7];
	context->Unmap(s_cameraMVStatsReadback, 0);

	float denom = total ? (float)total : 1.0f;
	float validDenom = validDepth ? (float)validDepth : 1.0f;
	OOVR_LOGF("CameraMV-STATS: eye=%d %ux%u total=%u validDepth=%u(%.1f%%) invalidDepth=%u(%.1f%%) "
	          "cameraActive=%u(%.1f%% valid) bridgeZero=%u(%.1f%% valid) bridgeTooWeak=%u(%.1f%% valid) "
	          "bridgeUsed=%u(%.1f%% valid) fallbackUsed=%u(%.1f%% valid)",
	    eye, outputW, outputH,
	    total,
	    validDepth, 100.0f * (float)validDepth / denom,
	    invalidDepth, 100.0f * (float)invalidDepth / denom,
	    cameraActive, 100.0f * (float)cameraActive / validDenom,
	    bridgeZero, 100.0f * (float)bridgeZero / validDenom,
	    bridgeTooWeak, 100.0f * (float)bridgeTooWeak / validDenom,
	    bridgeUsed, 100.0f * (float)bridgeUsed / validDenom,
	    fallbackUsed, 100.0f * (float)fallbackUsed / validDenom);
}

// Generate camera-derived motion vectors from depth + clip-to-clip reprojection matrix.
static bool GenerateCameraMVs(ID3D11DeviceContext* context, ID3D11ShaderResourceView* depthSRV,
    uint32_t outputW, uint32_t outputH,
    const float clipToClip[16],
    int depthOffsetX, int depthOffsetY, float jitterDeltaUVx, float jitterDeltaUVy,
    float currJitterUVx, float currJitterUVy,
    ID3D11ShaderResourceView* gameMVSRV, int gameMVOffsetX, int gameMVOffsetY,
    bool useGameMV, bool enableStats, int eyeForStats)
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
		cbu[28] = enableStats ? 1u : 0u;      // offset 112: enableStats
		cbu[29] = cbu[30] = cbu[31] = 0u;
		context->Unmap(s_cameraMVCB, 0);
	}

	// Save current CS state
	ID3D11ComputeShader* oldCS = nullptr;
	ID3D11ShaderResourceView* oldSRVs[2] = { nullptr, nullptr };
	ID3D11UnorderedAccessView* oldUAVs[4] = { nullptr, nullptr, nullptr, nullptr };
	ID3D11Buffer* oldCB = nullptr;
	context->CSGetShader(&oldCS, nullptr, nullptr);
	context->CSGetShaderResources(0, 2, oldSRVs);
	context->CSGetUnorderedAccessViews(0, 4, oldUAVs);
	context->CSGetConstantBuffers(0, 1, &oldCB);

	if (enableStats && s_cameraMVStatsUAV) {
		UINT zero[4] = { 0, 0, 0, 0 };
		context->ClearUnorderedAccessViewUint(s_cameraMVStatsUAV, zero);
	}

	context->CSSetShader(s_cameraMVCS, nullptr, 0);
	ID3D11ShaderResourceView* srvs[2] = { depthSRV, gameMVSRV };
	context->CSSetShaderResources(0, 2, srvs);
	ID3D11UnorderedAccessView* uavs[4] = { s_cameraMVUAV, s_cameraMVResidualUAV, s_cameraMVFallbackMaskUAV, s_cameraMVStatsUAV };
	context->CSSetUnorderedAccessViews(0, 4, uavs, nullptr);
	context->CSSetConstantBuffers(0, 1, &s_cameraMVCB);
	context->Dispatch((outputW + 7) / 8, (outputH + 7) / 8, 1);

	ID3D11UnorderedAccessView* nullUAVs[4] = { nullptr, nullptr, nullptr, nullptr };
	context->CSSetUnorderedAccessViews(0, 4, nullUAVs, nullptr);
	if (enableStats)
		LogCameraMVStats(context, outputW, outputH, eyeForStats);

	// Restore CS state
	context->CSSetShader(oldCS, nullptr, 0);
	context->CSSetShaderResources(0, 2, oldSRVs);
	context->CSSetUnorderedAccessViews(0, 4, oldUAVs, nullptr);
	context->CSSetConstantBuffers(0, 1, &oldCB);
	if (oldCS) oldCS->Release();
	if (oldSRVs[0]) oldSRVs[0]->Release();
	if (oldSRVs[1]) oldSRVs[1]->Release();
	if (oldUAVs[0]) oldUAVs[0]->Release();
	if (oldUAVs[1]) oldUAVs[1]->Release();
	if (oldUAVs[2]) oldUAVs[2]->Release();
	if (oldUAVs[3]) oldUAVs[3]->Release();
	if (oldCB) oldCB->Release();
	return true;
}

// ── MV Dilation (3x3 closest-depth) ──

static bool EnsureMVDilateResources(ID3D11Device* device, uint32_t w, uint32_t h)
{
	if (!s_mvDilateCS) {
		ID3DBlob* blob = nullptr;
		if (!CompileOrLoadCached(s_mvDilateHLSL, sizeof(s_mvDilateHLSL) - 1,
		        "CS_MVDilate", "cs_5_0", 0, &blob))
			return false;
		HRESULT hr = device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &s_mvDilateCS);
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
static ID3D11PixelShader* s_debugResidualMvPS = nullptr;
static ID3D11PixelShader* s_debugReactivePS = nullptr;

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

static constexpr char s_debugResidualMvHLSL[] = R"HLSL(
cbuffer DebugCB : register(b0) { float2 uvMin; float2 uvMax; };
Texture2D<float2> MVTex : register(t0);
struct VsOut { float4 pos : SV_POSITION; float2 tex : TEXCOORD0; };
float4 PS_DebugResidualMV(VsOut input) : SV_TARGET
{
    float2 sampleUV = uvMin + input.tex * (uvMax - uvMin);
    uint w, h;
    MVTex.GetDimensions(w, h);
    float2 mv = MVTex.Load(int3(sampleUV * float2(w, h), 0));

    // Residual/local MVs are much smaller than full camera MVs. Keep zero dark
    // and scale aggressively so wind/NPC/local animation stands out.
    float mag = saturate(length(mv) * 800.0);
    float r = lerp(0.08, saturate(0.5 + mv.x * 250.0), mag);
    float g = lerp(0.08, saturate(0.5 + mv.y * 250.0), mag);
    float b = max(0.08, mag);
    return float4(r, g, b, 1.0);
}
)HLSL";

static constexpr char s_debugReactiveHLSL[] = R"HLSL(
cbuffer DebugCB : register(b0) { float2 uvMin; float2 uvMax; };
Texture2D<float> ReactiveTex : register(t0);
struct VsOut { float4 pos : SV_POSITION; float2 tex : TEXCOORD0; };
float4 PS_DebugReactive(VsOut input) : SV_TARGET
{
    float2 sampleUV = uvMin + input.tex * (uvMax - uvMin);
    uint w, h;
    ReactiveTex.GetDimensions(w, h);
    float v = ReactiveTex.Load(int3(sampleUV * float2(w, h), 0));
    float amplified = saturate(v * 5.0);
    return float4(amplified, v > 0.02 ? 0.18 : 0.0, 1.0 - amplified, 1.0);
}
)HLSL";

static ID3D11Buffer* s_debugCB = nullptr;

static void EnsureDebugShaders(ID3D11Device* device)
{
	auto compilePS = [&](const char* src, size_t len, const char* entry, ID3D11PixelShader** out) {
		if (*out)
			return;
		ID3DBlob* blob = nullptr;
		if (!CompileOrLoadCached(src, len, entry, "ps_5_0", 0, &blob))
			return;
		device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, out);
		blob->Release();
	};
	compilePS(s_debugDepthHLSL, sizeof(s_debugDepthHLSL) - 1, "PS_DebugDepth", &s_debugDepthPS);
	compilePS(s_debugMvHLSL, sizeof(s_debugMvHLSL) - 1, "PS_DebugMV", &s_debugMvPS);
	compilePS(s_debugResidualMvHLSL, sizeof(s_debugResidualMvHLSL) - 1, "PS_DebugResidualMV", &s_debugResidualMvPS);
	compilePS(s_debugReactiveHLSL, sizeof(s_debugReactiveHLSL) - 1, "PS_DebugReactive", &s_debugReactivePS);

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

#ifdef OC_HAS_FSR3
static bool Fsr3TemporalRequested()
{
	return oovr_global_configuration.FsrEnabled()
	    && (oovr_global_configuration.FsrRenderScale() < 0.99f
	        || oovr_global_configuration.FsrNativeAA());
}

static float Fsr3EffectiveRenderScale()
{
	return oovr_global_configuration.FsrNativeAA()
	    ? 1.0f
	    : oovr_global_configuration.FsrRenderScale();
}
#else
static bool Fsr3TemporalRequested()
{
	return oovr_global_configuration.FsrEnabled()
	    && oovr_global_configuration.FsrRenderScale() < 0.99f;
}

static float Fsr3EffectiveRenderScale()
{
	return oovr_global_configuration.FsrRenderScale();
}
#endif

static std::string TrimLower(std::string value)
{
	value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char c) {
		return !std::isspace(c);
	}));
	value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char c) {
		return !std::isspace(c);
	}).base(), value.end());
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
		return (char)std::tolower(c);
	});
	return value;
}

static bool TryParseFloat(const std::string& value, float& out)
{
	errno = 0;
	char* end = nullptr;
	float parsed = std::strtof(value.c_str(), &end);
	while (end && std::isspace((unsigned char)*end))
		end++;
	if (end == value.c_str() || !end || *end != '\0' || errno == ERANGE || !std::isfinite(parsed))
		return false;
	out = parsed;
	return true;
}

enum class MipBiasUpscaler {
	None,
	Fsr3,
	Dlss,
};

static bool TemporalUpscalerRequestedForMipBias(float& renderScale, MipBiasUpscaler& upscaler)
{
	renderScale = Fsr3EffectiveRenderScale();
	upscaler = MipBiasUpscaler::None;

#ifdef OC_HAS_FSR3
	if (Fsr3TemporalRequested()) {
		renderScale = Fsr3EffectiveRenderScale();
		upscaler = MipBiasUpscaler::Fsr3;
		return true;
	}
#endif

#ifdef OC_HAS_DLSS
	if (oovr_global_configuration.DlssEnabled()
	    && (oovr_global_configuration.FsrRenderScale() < 0.99f
	        || oovr_global_configuration.DlssPreset() == 4)) {
		renderScale = oovr_global_configuration.FsrRenderScale();
		upscaler = MipBiasUpscaler::Dlss;
		return true;
	}
#endif

	return false;
}

static bool GetConfiguredMipBias(float& lodBias)
{
	if (!oovr_global_configuration.MipBiasEnabled())
		return false;

	std::string mipBiasMode = TrimLower(oovr_global_configuration.MipBias());
	if (mipBiasMode == "off" || mipBiasMode == "false" || mipBiasMode == "disabled"
	    || mipBiasMode == "none") {
		return false;
	}

	if (!mipBiasMode.empty() && mipBiasMode != "auto" && mipBiasMode != "default") {
		if (TryParseFloat(mipBiasMode, lodBias))
			return true;

		static bool warned = false;
		if (!warned) {
			OOVR_LOGF("MipBias: invalid mipBias value '%s'; falling back to auto", mipBiasMode.c_str());
			warned = true;
		}
	}

	float renderScale = 1.0f;
	MipBiasUpscaler upscaler = MipBiasUpscaler::None;
	if (!TemporalUpscalerRequestedForMipBias(renderScale, upscaler))
		return false;

	renderScale = std::clamp(renderScale, 0.1f, 1.0f);
	float upscalerOffset = 0.0f;
	if (upscaler == MipBiasUpscaler::Fsr3)
		upscalerOffset = oovr_global_configuration.Fsr3MipBiasOffset();
	else if (upscaler == MipBiasUpscaler::Dlss)
		upscalerOffset = oovr_global_configuration.DlssMipBiasOffset();

	lodBias = std::log2(renderScale)
	    + oovr_global_configuration.MipBiasOffset()
	    + upscalerOffset;
	return true;
}

static void UpdateMipBiasForUpscaler(ID3D11DeviceContext* ctx)
{
	float lodBias = 0.0f;
	bool enabled = GetConfiguredMipBias(lodBias);
	if (enabled && InitMipBiasHook(ctx)) {
		ConfigureMipBiasHook(true, lodBias);
	} else if (IsMipBiasHookActive()) {
		ConfigureMipBiasHook(false, 0.0f);
	}
}

#ifdef OC_HAS_DLSS
static DlssUpscaler* s_dlssUpscaler = nullptr;

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

	ID3DBlob *compiled = nullptr;
	if (!CompileOrLoadCached(hlsl, strlen(hlsl), entrypoint, target, flags, &compiled))
		OOVR_ABORTF("Error: Shader compile failed for %s", entrypoint);
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
	OOVR_LOGF("Foveation shader guard v1: game-device shader capture %s",
	    ocu_vrs_guard::InstallShaderCapture(device) ? "installed" : "unavailable");
	UpdateMipBiasForUpscaler(context);

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
	if (oovr_global_configuration.DlaaEnabled() || oovr_global_configuration.BlueSkyDefenderEnabled()) {
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
					OOVR_LOG("DLAA/PostAA: Shaders compiled and ready");
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
			OOVR_LOG("DLAA/PostAA: Shader compilation failed — falling back to no AA");
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

	// This is Skyrim/ENB's shared immediate context.  Drain outstanding work before
	// releasing OCU resources, but do not erase the application's pipeline state.
	// ClearState here broke the companion-window render whenever an overlay-owned
	// compositor was destroyed (garbage on Steam Link, black on VDXR).
	if (context) {
		context->Flush();
	}
	if (iAmDxcomp) {
		ShutdownMipBiasHook();
	}
	ReleaseFsr3PostAASRVs();

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
		if (s_vrsHookManager == &vrsManager) {
			DisarmSceneVRS();
			s_vrsHookManager = nullptr;
			s_vrsSceneTarget = nullptr;
		}
		if (s_densityMaskHookManager == &densityMaskManager) {
			DisarmSceneVRS();
			s_densityMaskHookManager = nullptr;
			s_vrsSceneTarget = nullptr;
		}
		vrsManager.Shutdown();
		densityMaskManager.Shutdown();
		s_sceneHookContext = nullptr;
		ResetVRSInputGeometry();
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
#ifdef OC_HAS_FSR3
	OCBridgeResourceSnapshot localBridgeResources;
	const auto& bridgeResources =
	    ReuseOrAcquireBridgeResourceSnapshot(localBridgeResources,
	        Fsr3TemporalRequested() && !cube && !isOverlay);
	ScopedBridgeResourceSnapshot bridgeResourceScope(&bridgeResources);
	const bool bridgeResourcesReady = bridgeResources.Ready();
#endif

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
	bool fsrActive = fsrReady && Fsr3TemporalRequested() && !cube && !isOverlay;
#ifdef OC_HAS_FSR3
	// FSR 3 can handle stereo-combined textures (bounds present); FSR 1 cannot
	if (fsrActive && bounds) {
		fsrActive = s_fsr3Upscaler && s_fsr3Upscaler->IsReady()
		    && bridgeResourcesReady && bridgeResources.mvTexture
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
	    && (oovr_global_configuration.FsrRenderScale() < 0.99f || oovr_global_configuration.DlssPreset() == 4)
	    && !cube && !isOverlay;
#else
	bool dlssNeedsInflation = false;
#endif

	uint32_t outWidth = srcDesc.Width;
	uint32_t outHeight = srcDesc.Height;
	if (fsrActive || dlssNeedsInflation) {
		// FSR / DLSS: inflate swapchain to display resolution so the
		// upscaler has room to write the full-res output.
		float invScale = 1.0f / std::max(0.5f, Fsr3EffectiveRenderScale());
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
			OOVR_LOGF("%s output: %dx%d (scale %.2f%s)", dlssNeedsInflation ? "DLSS" : "FSR",
			    outWidth, outHeight, Fsr3EffectiveRenderScale(),
			    oovr_global_configuration.FsrNativeAA() ? ", native AA" : "");

		// Preserve Skyrim/ENB's shared immediate-context state.  Flushing orders
		// outstanding work before old OCU swapchain resources are retired without
		// blanking the application's companion-window pipeline.
		if (chain || !swapchain_rtvs.empty()) {
			context->Flush();
			OOVR_LOG("D3D11: retiring swapchain resources without clearing application context state");
		} else {
			OOVR_LOG("D3D11: first swapchain creation preserves the game's immediate-context state");
		}

#ifdef OC_HAS_FSR3
		// If FSR3 is active, drain its DX12 queue too — shared textures cross both APIs
		if (s_fsr3Upscaler && s_fsr3Upscaler->IsReady()) {
			ReleaseFsr3PostAASRVs();
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

		// ── DLAA / CAS staging textures (output resolution, skip for overlays) ──
		// dlaaOutput is used as staging for both DLAA and CAS post-passes
		// (both need to copy swapchain content before reading+writing it).
		bool needStagingTextures = !isOverlay &&
		    (dlaaReady || oovr_global_configuration.CasEnabled()
		        || oovr_global_configuration.BlueSkyDefenderEnabled());
		if (needStagingTextures) {
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

			// DLAA operates at the output resolution (display-res when upscaler active,
			// render-res when no upscaler). This ensures DLAA can process FSR3/DLSS output.
			uint32_t dw = outWidth;
			uint32_t dh = outHeight;

			// Intermediate: RGBA8 (RGB = pre-filtered color, A = edge luminance)
			D3D11_TEXTURE2D_DESC diDesc = {};
			diDesc.Width = dw;
			diDesc.Height = dh;
			diDesc.MipLevels = 1;
			diDesc.ArraySize = 1;
			diDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			diDesc.SampleDesc.Count = 1;
			diDesc.Usage = D3D11_USAGE_DEFAULT;
			diDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;

			// Output/staging: match swapchain's actual texture format (may be TYPELESS).
			// Used as staging for CAS and DLAA post-passes.
			D3D11_TEXTURE2D_DESC doDesc = diDesc;
			{
				D3D11_TEXTURE2D_DESC swapDesc;
				imagesHandles[0].texture->GetDesc(&swapDesc);
				doDesc.Format = swapDesc.Format;
			}

			HRESULT hr1 = device->CreateTexture2D(&diDesc, nullptr, &dlaaIntermediate);
			HRESULT hr2 = device->CreateTexture2D(&doDesc, nullptr, &dlaaOutput);
			if (SUCCEEDED(hr1) && SUCCEEDED(hr2)) {
				device->CreateShaderResourceView(dlaaIntermediate, nullptr, &dlaaIntermediateSRV);
				// dlaaOutput may be TYPELESS (matching swapchain) — need explicit SRGB for views
				// so hardware correctly decodes gamma (swapchain content is SRGB-encoded).
				D3D11_SHADER_RESOURCE_VIEW_DESC outSrvDesc = {};
				outSrvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
				outSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
				outSrvDesc.Texture2D.MipLevels = 1;
				device->CreateShaderResourceView(dlaaOutput, &outSrvDesc, &dlaaOutputSRV);
				D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
				rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
				rtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
				device->CreateRenderTargetView(dlaaIntermediate, &rtvDesc, &dlaaIntermediateRTV);
				rtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
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
static float s_vrsOpticalX[2] = { 0.5f, 0.5f };
static float s_vrsOpticalY[2] = { 0.5f, 0.5f };
static float s_vrsProjX[2] = { 0.5f, 0.5f };
static float s_vrsProjY[2] = { 0.5f, 0.5f };
static float s_vrsTanL[2] = {};
static float s_vrsTanR[2] = {};
static float s_vrsTanU[2] = {};
static float s_vrsTanD[2] = {};
static ocu_vrs_gaze::Center s_vrsSmoothedGaze[2];
static bool s_vrsHasSmoothedGaze = false;
static std::int64_t s_vrsLastGazeQpc = 0;
static int s_vrsRenderWidth[2] = {};
static int s_vrsRenderHeight[2] = {};
static Microsoft::WRL::ComPtr<ID3D11Texture2D> s_vrsRenderTexture[2];
static VRSManager::EyeRegion s_vrsEyeRegion[2];
static std::uint8_t s_vrsGeometryMask = 0;
static bool s_vrsInitialFrameDone = false;
static bool s_vrsPatternReady = false;
static std::uint32_t s_vrsGazeDiagnosticCounter = 0;
static int s_currentEyeIdx = 0;

static void ResetVRSInputGeometry()
{
	s_vrsRenderTexture[0].Reset();
	s_vrsRenderTexture[1].Reset();
	s_vrsGeometryMask = 0;
}

void DX11Compositor::BeginVRSGameFrame()
{
	rdmDiagnosticSchedule.BeginFrame();
	// Reuse the existing menu/bridge lookup before expiring coverage: this call
	// can establish the first bridge connection during this very frame boundary.
	const bool menuOpen = OCBridge_MenuState() == 1;
	// WaitGetPoses begins every real frame, including native/backoff/menu frames
	// that never enter DAPA's cache path. Expire the producer mask here so its
	// first owned draw clears old pixels, and a frame with no owned draws cannot
	// reuse old coverage. A conflicting producer/reader invalidates only this
	// real frame's mask; the next boundary retries without waiting.
	DapaMaskBridge::BeginFrame(s_pBridge.Get());
	// Publish a fresh disabled frame before any early return. Effect consumers
	// use the same gaze policy without depending on a GPU backend or upscaler.
	ocu_effect_foveation::BeginFrame();
	// Only the two submissions since the previous frame boundary may form a pair.
	const auto submittedEyeMask = s_vrsGeometryMask;
	s_vrsGeometryMask = 0;
	s_vrsSceneBindings = 0;
	s_vrsProtectedBindings = s_vrsUnclassifiedBindings = s_vrsCoarseBindings = 0;
	s_vrsTerrainDepthBindings = 0;
	s_sceneHookContext = context;
	// Always expire previous-frame reconstruction, even when gaze disappears,
	// menus open, geometry is unavailable, or the selected backend changes.
	DisarmSceneVRS();
	densityMaskManager.EndFrame();
	if (!oovr_global_configuration.VrsAnyEnabled() || menuOpen) {
		DisarmSceneVRS();
		s_vrsSceneTarget = nullptr;
		vrsManager.Disable();
		s_vrsPatternReady = false;
		s_vrsHasSmoothedGaze = false;
		return;
	}

	const bool hasStereoGeometry = submittedEyeMask == 0x3 &&
	    s_vrsRenderTexture[0] != nullptr &&
	    s_vrsRenderTexture[0].Get() == s_vrsRenderTexture[1].Get() &&
	    s_vrsRenderWidth[0] == s_vrsRenderWidth[1] &&
	    s_vrsRenderHeight[0] == s_vrsRenderHeight[1] &&
	    s_vrsRenderWidth[0] > 0 && s_vrsRenderHeight[0] > 0;
	// Eye output layout is independent of gaze availability. Fixed foveation
	// and gaze-loss fallback must also reach the validated scene-depth path
	// when an upscaler submits a separate texture for each eye.
	const bool hasSubmittedEyeGeometry =
	    submittedEyeMask == 0x3 && s_vrsRenderTexture[0] && s_vrsRenderTexture[1] &&
	    s_vrsRenderWidth[0] > 0 && s_vrsRenderHeight[0] > 0 &&
	    s_vrsRenderWidth[1] > 0 && s_vrsRenderHeight[1] > 0;
	static int lastGeometryStatus = -1;
	auto reportGeometry = [&](int status, const char* description) {
		if (lastGeometryStatus == status) return;
		lastGeometryStatus = status;
		OOVR_LOGF("Foveation geometry v3: %s; submittedMask=0x%X left=%dx%d right=%dx%d",
		    description, unsigned(submittedEyeMask), s_vrsRenderWidth[0], s_vrsRenderHeight[0],
		    s_vrsRenderWidth[1], s_vrsRenderHeight[1]);
	};
	if (!hasSubmittedEyeGeometry) {
		DisarmSceneVRS();
		vrsManager.Disable();
		s_vrsPatternReady = false;
		s_vrsHasSmoothedGaze = false;
		reportGeometry(0, "waiting for both submitted eyes; scene foveation withheld");
		return;
	}

	bool gazeUsed = false;
	XrTime effectGazeSampleTime = 0;
	float nextCenterX[2] = { s_vrsOpticalX[0], s_vrsOpticalX[1] };
	float nextCenterY[2] = { s_vrsOpticalY[0], s_vrsOpticalY[1] };
	if (oovr_global_configuration.VrsEyeTracked()) {
		if (BaseInput* input = GetUnsafeBaseInput()) {
			XrVector3f gazeDirection{};
			XrPosef eyeViewPoses[2] = {
				{ { 0, 0, 0, 1 }, { 0, 0, 0 } },
				{ { 0, 0, 0, 1 }, { 0, 0, 0 } }
			};
			XrTime gazeSampleTime = 0;
			if (input->SampleEyeGazeDirection(xr_gbl->nextPredictedFrameTime,
			        gazeDirection, eyeViewPoses, gazeSampleTime)) {
				ocu_vrs_gaze::Center target[2];
				ocu_vrs_gaze::Direction eyeLocal[2];
				gazeUsed = ocu_vrs_gaze::ProjectViewSpace(
				               gazeDirection.x, gazeDirection.y, gazeDirection.z,
				               eyeViewPoses[0].orientation.x, eyeViewPoses[0].orientation.y,
				               eyeViewPoses[0].orientation.z, eyeViewPoses[0].orientation.w,
				               s_vrsTanL[0], s_vrsTanR[0], s_vrsTanU[0], s_vrsTanD[0],
				               target[0], &eyeLocal[0]) &&
				    ocu_vrs_gaze::ProjectViewSpace(
				        gazeDirection.x, gazeDirection.y, gazeDirection.z,
				        eyeViewPoses[1].orientation.x, eyeViewPoses[1].orientation.y,
				        eyeViewPoses[1].orientation.z, eyeViewPoses[1].orientation.w,
				        s_vrsTanL[1], s_vrsTanR[1], s_vrsTanU[1], s_vrsTanD[1],
				        target[1], &eyeLocal[1]);
				if (gazeUsed) {
					effectGazeSampleTime = gazeSampleTime;
					const auto now = ocu_effect_foveation::ClockTicks();
					LARGE_INTEGER frequency{};
					QueryPerformanceFrequency(&frequency);
					const float dt = frequency.QuadPart > 0 && now > s_vrsLastGazeQpc ?
					    static_cast<float>(double(now - s_vrsLastGazeQpc) / double(frequency.QuadPart)) : 0.0f;
					s_vrsLastGazeQpc = now;
					for (int gazeEye = 0; gazeEye < 2; ++gazeEye) {
						s_vrsSmoothedGaze[gazeEye] = ocu_vrs_gaze::Smooth(
						    s_vrsSmoothedGaze[gazeEye], target[gazeEye], dt, s_vrsHasSmoothedGaze);
						nextCenterX[gazeEye] = s_vrsSmoothedGaze[gazeEye].x;
						nextCenterY[gazeEye] = s_vrsSmoothedGaze[gazeEye].y;
					}
					s_vrsHasSmoothedGaze = true;
					if (oovr_debug_logging_enabled() &&
					    (s_vrsGazeDiagnosticCounter++ % 600u) == 0u) {
						OOVR_LOGF(
						    "VRS live gaze at pre-render boundary: eyeDirL=(%.3f,%.3f,%.3f), eyeDirR=(%.3f,%.3f,%.3f), leftUV=(%.3f,%.3f), rightUV=(%.3f,%.3f), sampleTime=%s",
						    eyeLocal[0].x, eyeLocal[0].y, eyeLocal[0].z,
						    eyeLocal[1].x, eyeLocal[1].y, eyeLocal[1].z,
						    nextCenterX[0], nextCenterY[0], nextCenterX[1], nextCenterY[1],
						    gazeSampleTime == 0 ? "unavailable" : "runtime-provided");
					}
				}
			}
		}
	}

	const auto vrsMode = ocu_vrs_gaze::SelectMode(
	    oovr_global_configuration.VrsEyeTracked(),
	    oovr_global_configuration.VrsFixedEnabled(), gazeUsed, false);
	if (vrsMode != ocu_vrs_gaze::Mode::EyeTracked)
		s_vrsHasSmoothedGaze = false;

	const auto profileRadii = oovr_global_configuration.FoveationRadii(
	    vrsMode == ocu_vrs_gaze::Mode::EyeTracked);
	const bool customEyeRates = vrsMode == ocu_vrs_gaze::Mode::EyeTracked &&
	    oovr_global_configuration.VrsEyeCustomRates();
	const auto profileRates = oovr_global_configuration.FoveationRates(
	    vrsMode == ocu_vrs_gaze::Mode::EyeTracked);
	static int s_lastVrsMode = -1;
	const int modeValue = static_cast<int>(vrsMode);
	if (modeValue != s_lastVrsMode) {
		const char* modeName = vrsMode == ocu_vrs_gaze::Mode::EyeTracked ? "eye-tracked" :
		    (vrsMode == ocu_vrs_gaze::Mode::Fixed ? "fixed-center (explicit)" :
		                                              "off (gaze unavailable; Fixed disabled)");
		OOVR_LOGF("Foveation mode: %s; profile inner=%.2f mid=%.2f", modeName,
		    profileRadii.inner, profileRadii.mid);
		if (vrsMode == ocu_vrs_gaze::Mode::EyeTracked)
			OOVR_LOGF("Eye-tracked rates (effective): %s/%s/%s; compatibility cap=%s",
			    ocu_foveation::RateName(profileRates.inner), ocu_foveation::RateName(profileRates.mid),
			    ocu_foveation::RateName(profileRates.outer),
			    oovr_global_configuration.VrsEyeCompatibilityMode() ? "on" : "off");
		s_lastVrsMode = modeValue;
	}

	if (vrsMode == ocu_vrs_gaze::Mode::Off) {
		DisarmSceneVRS();
		vrsManager.Disable();
		s_vrsPatternReady = false;
		return;
	}

	std::string shapeBackend = oovr_global_configuration.FoveatedBackend();
	std::transform(shapeBackend.begin(), shapeBackend.end(), shapeBackend.begin(),
	    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	const float horizontalScale = vrsMode == ocu_vrs_gaze::Mode::EyeTracked && shapeBackend != "effects" ?
	    oovr_global_configuration.VrsEyeHorizontalScale() : 1.f;
	for (int eye = 0; eye < 2; ++eye) {
		if (vrsMode == ocu_vrs_gaze::Mode::EyeTracked)
			ocu_foveation::OffsetCenter(nextCenterX[eye], nextCenterY[eye], eye,
			    oovr_global_configuration.VrsEyeHorizontalOffset(), oovr_global_configuration.VrsEyeVerticalOffset());
		s_vrsProjX[eye] = nextCenterX[eye];
		s_vrsProjY[eye] = nextCenterY[eye];
	}

	ocu_effect_foveation::Snapshot effectProfile{};
	effectProfile.mode = vrsMode == ocu_vrs_gaze::Mode::EyeTracked ?
	    ocu_effect_foveation::Mode::EyeTracked : ocu_effect_foveation::Mode::Fixed;
	effectProfile.shape = ocu_effect_foveation::Shape::UVRadialHalfExtent;
	effectProfile.publicationQpc = ocu_effect_foveation::ClockTicks();
	effectProfile.predictedDisplayTime = xr_gbl->nextPredictedFrameTime;
	effectProfile.gazeSampleTime = effectGazeSampleTime;
	effectProfile.innerRadius = profileRadii.inner;
	effectProfile.midRadius = profileRadii.mid;
	for (int eye = 0; eye < 2; ++eye) {
		effectProfile.centerUV[eye][0] = nextCenterX[eye];
		effectProfile.centerUV[eye][1] = nextCenterY[eye];
		effectProfile.fovTangents[eye][0] = s_vrsTanL[eye];
		effectProfile.fovTangents[eye][1] = s_vrsTanR[eye];
		effectProfile.fovTangents[eye][2] = s_vrsTanU[eye];
		effectProfile.fovTangents[eye][3] = s_vrsTanD[eye];
	}
	ocu_effect_foveation::GetState().Publish(effectProfile, horizontalScale);

	std::string requestedBackend = oovr_global_configuration.FoveatedBackend();
	std::transform(requestedBackend.begin(), requestedBackend.end(), requestedBackend.begin(),
	    [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	if (requestedBackend != "auto" && requestedBackend != "vrs" && requestedBackend != "rdm" && requestedBackend != "effects") {
		static bool loggedBadBackend = false;
		if (!loggedBadBackend) {
			loggedBadBackend = true;
			OOVR_LOGF("Foveation: unknown backend '%s'; using Auto", requestedBackend.c_str());
		}
		requestedBackend = "auto";
	}

	if (requestedBackend == "effects") {
		DisarmSceneVRS();
		vrsManager.Disable();
		s_vrsPatternReady = false;
		static bool loggedEffectOnly = false;
		if (!loggedEffectOnly) {
			loggedEffectOnly = true;
			OOVR_LOG("Foveation: effects-only profile published; scene remains full rate. Requires an enabled compatible renderer effect consumer");
		}
		return;
	}

	// Separate upscaled eye outputs do not describe Skyrim's render atlas.
	// The bridge identifies the original stereo depth allocation; live viewports
	// supply its active render extent when a temporal upscaler changes scale.
	Microsoft::WRL::ComPtr<ID3D11Texture2D> separateEyeSceneDepth;
	D3D11_TEXTURE2D_DESC separateEyeSceneDesc{};
	if (!hasStereoGeometry) {
		OCBridgeResourceSnapshot geometryResources;
		if (AcquireBridgeResourceSnapshot(geometryResources) && geometryResources.Ready() &&
		    geometryResources.d3dDevice == device && geometryResources.d3dContext == context &&
		    geometryResources.depthTexture) {
			geometryResources.depthTexture->GetDesc(&separateEyeSceneDesc);
			if (separateEyeSceneDesc.ArraySize == 1 && separateEyeSceneDesc.SampleDesc.Count == 1 &&
			    separateEyeSceneDesc.Width >= 2 && (separateEyeSceneDesc.Width % 2) == 0 &&
			    separateEyeSceneDesc.Height > 0 && (separateEyeSceneDesc.BindFlags & D3D11_BIND_DEPTH_STENCIL))
				separateEyeSceneDepth = geometryResources.depthTexture;
		}
		if (!separateEyeSceneDepth) {
			reportGeometry(2, "separate eye outputs without valid scene depth; scene foveation withheld, profile available to shader effects");
			return;
		}
		reportGeometry(3, "separate eye outputs using validated stereo scene depth");
	} else {
		reportGeometry(1, "shared stereo eye output");
	}

	const bool mayUseVrs = requestedBackend != "rdm";
	if (mayUseVrs && !vrsManager.IsAvailable() && !vrsManager.WasInitializationAttempted()) {
		if (vrsManager.Initialize(device))
			OOVR_LOG("Foveation: NVIDIA hardware VRS backend initialized");
		else
			OOVR_LOG("Foveation: NVIDIA hardware VRS unavailable for this device session");
	}
	const bool useHardwareVrs = mayUseVrs && vrsManager.IsAvailable();
	const bool useDensityMask = requestedBackend == "rdm" ||
	    (requestedBackend == "auto" && !useHardwareVrs);
	if (requestedBackend == "vrs" && !useHardwareVrs) {
		DisarmSceneVRS();
		s_vrsPatternReady = false;
		return;
	}

	if (!InstallSceneTargetHooks(device)) {
		DisarmSceneVRS();
		vrsManager.Disable();
		s_vrsPatternReady = false;
		OOVR_LOG("Foveation: render-target scoping hooks unavailable; backend withheld to protect non-scene passes");
		return;
	}

	s_vrsSceneTarget = hasStereoGeometry ? s_vrsRenderTexture[0].Get() : nullptr;
	s_vrsFrameArmed = true;
	s_vrsHookApplied = false;

	static int s_lastBackend = -1;
	ocu_foveation::BlackoutFrame blackout;
	const auto prepareBlackout = [&](int leftWidth, int leftHeight, int rightWidth, int rightHeight) {
		if (!oovr_global_configuration.VrsEyeBlackoutCull() ||
		    !oovr_global_configuration.VrsEyeAnyBlackout() ||
		    vrsMode != ocu_vrs_gaze::Mode::EyeTracked || s_blackoutPresentationFailed ||
		    !s_blackoutRenderer.Initialize(device)) return;
		blackout.mask.middle = oovr_global_configuration.VrsEyeMiddleBlackout();
		blackout.mask.outer = oovr_global_configuration.VrsEyeOuterBlackout();
		blackout.mask.cutoff = oovr_global_configuration.VrsEyePeripheralMask();
		blackout.mask.cutoffRadius = oovr_global_configuration.VrsEyePeripheralMaskRadius(profileRadii.mid);
		blackout.inner = profileRadii.inner;
		blackout.middle = profileRadii.mid;
		blackout.horizontalScale = horizontalScale;
		blackout.frameId = ocu_effect_foveation::GetState().ReadForPresentation(
		    ocu_effect_foveation::ClockTicks()).frameId;
		const int sizes[2][2] = {{leftWidth, leftHeight}, {rightWidth, rightHeight}};
		for (int eye = 0; eye < 2; ++eye) {
			blackout.centers[eye][0] = s_vrsProjX[eye];
			blackout.centers[eye][1] = s_vrsProjY[eye];
			for (int axis = 0; axis < 2; ++axis) {
				blackout.sceneEyeSize[eye][axis] = float(sizes[eye][axis]);
				if (oovr_global_configuration.ASWEnabled()) {
					// Render a border covering DAPA's bounded source search. CacheFrame
					// independently checks this against the actual source dimensions.
					const int submitted = axis == 0 ? s_vrsEyeRegion[eye].width : s_vrsEyeRegion[eye].height;
					const float guard = (std::max)(.125f * sizes[eye][axis],
					    65.f * sizes[eye][axis] / (std::max)(1, submitted)) + 2.f;
					blackout.mask.guardPixels = (std::max)(blackout.mask.guardPixels, guard);
				}
			}
		}
	};
	if (useHardwareVrs) {
		if (!ocu_vrs_guard::WatchContext(context, &VRSShaderStateChanged)) {
			DisarmSceneVRS();
			s_vrsPatternReady = false;
			OOVR_LOG_LIMITEDF(5000, "VRS coverage guard v1: context hooks unavailable; hardware VRS not armed");
			return;
		}
		s_densityMaskHookManager = nullptr;
		densityMaskManager.EndFrame();
		int renderWidth = s_vrsRenderWidth[0];
		int renderHeight = s_vrsRenderHeight[0];
		VRSManager::EyeRegion eyeRegions[2] = { s_vrsEyeRegion[0], s_vrsEyeRegion[1] };
		ID3D11Texture2D* sceneDepth = nullptr;
		if (separateEyeSceneDepth) {
			sceneDepth = separateEyeSceneDepth.Get();
			renderWidth = static_cast<int>(separateEyeSceneDesc.Width);
			renderHeight = static_cast<int>(separateEyeSceneDesc.Height);
			eyeRegions[0] = {0, 0, renderWidth / 2, renderHeight};
			eyeRegions[1] = {renderWidth / 2, 0, renderWidth / 2, renderHeight};
		}
		OCBridgeResourceSnapshot sceneResources;
		if (!separateEyeSceneDepth && AcquireBridgeResourceSnapshot(sceneResources) && sceneResources.Ready() &&
		    sceneResources.d3dDevice == device && sceneResources.d3dContext == context &&
		    sceneResources.depthTexture) {
			D3D11_TEXTURE2D_DESC depthDesc{};
			sceneResources.depthTexture->GetDesc(&depthDesc);
			if (depthDesc.ArraySize == 1 && depthDesc.SampleDesc.Count == 1 &&
			    depthDesc.Width && depthDesc.Height && (depthDesc.BindFlags & D3D11_BIND_DEPTH_STENCIL)) {
				sceneDepth = sceneResources.depthTexture;
				renderWidth = int(depthDesc.Width);
				renderHeight = int(depthDesc.Height);
				for (int e = 0; e < 2; ++e)
					eyeRegions[e] = ocu_vrs_scope::ScaleRegion(s_vrsEyeRegion[e],
					    s_vrsRenderWidth[0], s_vrsRenderHeight[0], renderWidth, renderHeight);
			}
		}
		s_vrsSceneScope.Arm(context, sceneDepth, s_vrsSceneTarget, renderWidth, renderHeight);
		if (sceneDepth && !s_vrsAlphaCoverageScope.Arm(context, sceneDepth, &SyncVRSForShaderState)) {
			DisarmSceneVRS();
			s_vrsPatternReady = false;
			OOVR_LOG_LIMITEDF(5000, "VRS cutout-material-guard-v1: draw observer unavailable; hardware VRS not armed");
			return;
		}
		static ID3D11Texture2D* lastLoggedDepth = nullptr;
		static int lastLoggedWidth = 0, lastLoggedHeight = 0;
		if (lastLoggedDepth != sceneDepth || lastLoggedWidth != renderWidth || lastLoggedHeight != renderHeight) {
			OOVR_LOGF("Foveation scene scope v2: source=%s render=%dx%d submitted=%dx%d bridgeGeneration=%llu",
			    sceneDepth ? "main-depth" : "submitted-color fallback", renderWidth, renderHeight,
			    s_vrsRenderWidth[0], s_vrsRenderHeight[0], (unsigned long long)sceneResources.generation);
			lastLoggedDepth = sceneDepth;
			lastLoggedWidth = renderWidth;
			lastLoggedHeight = renderHeight;
		}
		vrsManager.SetProjectionCenters(s_vrsProjX[0], s_vrsProjY[0],
		    s_vrsProjX[1], s_vrsProjY[1]);
		vrsManager.SetHorizontalScale(horizontalScale);
		prepareBlackout(eyeRegions[0].width, eyeRegions[0].height,
		    eyeRegions[1].width, eyeRegions[1].height);
		vrsManager.SetBlackout(blackout.mask);
		if (!vrsManager.UpdateStereoPattern(renderWidth, renderHeight,
		        eyeRegions[0], eyeRegions[1], profileRadii.inner, profileRadii.mid, profileRates)) {
			DisarmSceneVRS();
			s_vrsPatternReady = false;
			return;
		}
		s_vrsHookManager = &vrsManager;
		ID3D11RenderTargetView* currentRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
		Microsoft::WRL::ComPtr<ID3D11DepthStencilView> currentDepth;
		context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, currentRTVs, &currentDepth);
		SyncVRSForRenderTargets(context, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, currentRTVs, currentDepth.Get());
		for (auto* rtv : currentRTVs) {
			if (rtv)
				rtv->Release();
		}
		if (s_lastBackend != 0) {
			OOVR_LOG("Foveation backend: NVIDIA hardware VRS");
			s_lastBackend = 0;
		}
	} else if (useDensityMask) {
        s_vrsHookManager = nullptr;
        vrsManager.Disable();
        OCBridgeResourceSnapshot sceneResources;
        ID3D11Texture2D* sceneDepth = nullptr;
        int width = s_vrsRenderWidth[0], height = s_vrsRenderHeight[0];
        DensityMaskManager::EyeRegion regions[2] = {
            {s_vrsEyeRegion[0].left, s_vrsEyeRegion[0].top, s_vrsEyeRegion[0].width, s_vrsEyeRegion[0].height},
            {s_vrsEyeRegion[1].left, s_vrsEyeRegion[1].top, s_vrsEyeRegion[1].width, s_vrsEyeRegion[1].height}
        };
        if (separateEyeSceneDepth) {
            sceneDepth = separateEyeSceneDepth.Get();
            width = static_cast<int>(separateEyeSceneDesc.Width);
            height = static_cast<int>(separateEyeSceneDesc.Height);
            regions[0] = {0, 0, width / 2, height};
            regions[1] = {width / 2, 0, width / 2, height};
        }
        if (!separateEyeSceneDepth && AcquireBridgeResourceSnapshot(sceneResources) && sceneResources.Ready() &&
            sceneResources.d3dDevice == device && sceneResources.d3dContext == context && sceneResources.depthTexture) {
            D3D11_TEXTURE2D_DESC d{}; sceneResources.depthTexture->GetDesc(&d);
            if (d.ArraySize == 1 && d.SampleDesc.Count == 1 && d.Width && d.Height) {
                sceneDepth = sceneResources.depthTexture; width = int(d.Width); height = int(d.Height);
                for (auto& region : regions) region = ocu_vrs_scope::ScaleRegion(region,
                    s_vrsRenderWidth[0], s_vrsRenderHeight[0], width, height);
            }
        }
        const float centers[4] = {s_vrsProjX[0], s_vrsProjY[0], s_vrsProjX[1], s_vrsProjY[1]};
        prepareBlackout(regions[0].width, regions[0].height, regions[1].width, regions[1].height);
        const bool captureDiagnostics = rdmDiagnosticSchedule.Schedule(
            OcuLogging::NowMs(), oovr_global_configuration.DebugLogging());
        if (!densityMaskManager.Arm(context, sceneDepth, s_vrsSceneTarget, width, height, regions[0], regions[1],
                {profileRadii.inner, profileRadii.mid,
                    vrsMode == ocu_vrs_gaze::Mode::EyeTracked ? oovr_global_configuration.VrsEyeCompatibilityMode() : oovr_global_configuration.VrsCompatibilityMode(),
                    customEyeRates, profileRates, horizontalScale, blackout.mask}, centers, captureDiagnostics)) {
            DisarmSceneVRS(); s_vrsPatternReady = false;
            OOVR_LOG_LIMITEDF(5000, "RDM handoff v2: context/geometry/ownership guide unavailable for this frame");
            return;
        }
        s_densityMaskHookManager = &densityMaskManager;
		if (s_lastBackend != 1) {
			OOVR_LOG("Foveation backend: cross-vendor Radial Density Mask");
			s_lastBackend = 1;
		}
	}
	if (blackout.Active() && !ocu_effect_foveation::GetState().LatchBlackout(blackout)) {
		DisarmSceneVRS();
		densityMaskManager.EndFrame();
		vrsManager.Disable();
		s_vrsPatternReady = false;
		return;
	}
	s_vrsPatternReady = true;
	if (blackout.Active())
		OOVR_LOG_LIMITEDF(5000, "Foveation blackout culling armed: backend=%s guard=%.1fpx; final mask latched to rendered frame",
		    useHardwareVrs ? "VRS" : "RDM", blackout.mask.guardPixels);

	if (!s_vrsInitialFrameDone) {
		OOVR_LOGF("Foveation: first pre-render stereo atlas armed — target %dx%d, eye regions %dx%d + %dx%d",
		    s_vrsRenderWidth[0], s_vrsRenderHeight[0],
		    s_vrsEyeRegion[0].width, s_vrsEyeRegion[0].height,
		    s_vrsEyeRegion[1].width, s_vrsEyeRegion[1].height);
		s_vrsInitialFrameDone = true;
	}
}

void DX11Compositor::ReleaseFsr3PostAASRVs()
{
	for (int i = 0; i < 2; i++) {
		if (fsr3PostAASrcSRV[i]) {
			fsr3PostAASrcSRV[i]->Release();
			fsr3PostAASrcSRV[i] = nullptr;
		}
		fsr3PostAASrcTex[i] = nullptr;
	}
}

ID3D11ShaderResourceView* DX11Compositor::GetOrCreateFsr3PostAASRV(int eyeIdx, ID3D11Texture2D* src)
{
	if (eyeIdx < 0 || eyeIdx > 1 || !src)
		return nullptr;

	if (fsr3PostAASrcTex[eyeIdx] == src && fsr3PostAASrcSRV[eyeIdx])
		return fsr3PostAASrcSRV[eyeIdx];

	if (fsr3PostAASrcSRV[eyeIdx]) {
		fsr3PostAASrcSRV[eyeIdx]->Release();
		fsr3PostAASrcSRV[eyeIdx] = nullptr;
	}
	fsr3PostAASrcTex[eyeIdx] = src;

	D3D11_TEXTURE2D_DESC td = {};
	src->GetDesc(&td);

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = 1;
	srvDesc.Texture2D.MostDetailedMip = 0;

	// FSR3 is configured with NON_LINEAR_COLOR_SRGB and writes gamma-encoded
	// color into an UNORM texture. Sample as UNORM so this post-AA pass preserves
	// the existing brightness and does not introduce an sRGB decode/encode pair.
	switch (td.Format) {
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
		srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		break;
	case DXGI_FORMAT_B8G8R8A8_TYPELESS:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
		srvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
		break;
	default:
		srvDesc.Format = td.Format;
		break;
	}

	HRESULT hr = device->CreateShaderResourceView(src, &srvDesc, &fsr3PostAASrcSRV[eyeIdx]);
	if (FAILED(hr)) {
		OOVR_LOGF("FSR3 PostAA: CreateSRV failed eye=%d texFmt=%u viewFmt=%u hr=0x%08X",
		    eyeIdx, td.Format, srvDesc.Format, hr);
		fsr3PostAASrcTex[eyeIdx] = nullptr;
		return nullptr;
	}

	return fsr3PostAASrcSRV[eyeIdx];
}

bool DX11Compositor::ApplyFsr3PostAA(ID3D11Texture2D* src, int eyeIdx, int currentIndex, uint32_t width, uint32_t height)
{
	if (!oovr_global_configuration.BlueSkyDefenderEnabled() || !dlaaReady
	    || !src || !dlaaIntermediate || !dlaaOutput
	    || !dlaaIntermediateSRV || !dlaaIntermediateRTV || !dlaaOutputRTV
	    || !dlaa_vshader || !dlaa_pre_pshader || !dlaa_main_pshader
	    || !dlaa_cbuffer || !dlaa_pointSampler)
		return false;

	ID3D11ShaderResourceView* srcSRV = GetOrCreateFsr3PostAASRV(eyeIdx, src);
	if (!srcSRV)
		return false;

	D3D11_MAPPED_SUBRESOURCE mapped = {};
	if (FAILED(context->Map(dlaa_cbuffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
		return false;

	uint32_t safeWidth = width ? width : 1;
	uint32_t safeHeight = height ? height : 1;
	float cbData[4] = {
		1.0f / safeWidth,
		1.0f / safeHeight,
		oovr_global_configuration.BlueSkyDefenderLambda(),
		oovr_global_configuration.BlueSkyDefenderEpsilon()
	};
	memcpy(mapped.pData, cbData, sizeof(cbData));
	context->Unmap(dlaa_cbuffer, 0);

	UINT numViewports = 0;
	context->RSGetViewports(&numViewports, nullptr);
	D3D11_VIEWPORT savedViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
	if (numViewports)
		context->RSGetViewports(&numViewports, savedViewports);

	UINT numScissors = 0;
	context->RSGetScissorRects(&numScissors, nullptr);
	D3D11_RECT savedScissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
	if (numScissors)
		context->RSGetScissorRects(&numScissors, savedScissors);

	ID3D11RasterizerState* savedRS = nullptr;
	context->RSGetState(&savedRS);
	D3D11_PRIMITIVE_TOPOLOGY savedTopology;
	context->IAGetPrimitiveTopology(&savedTopology);

	context->RSSetState(nullptr);
	context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	context->OMSetBlendState(nullptr, nullptr, 0xffffffff);

	D3D11_VIEWPORT vp = {};
	vp.Width = (float)width;
	vp.Height = (float)height;
	vp.MaxDepth = 1.0f;
	context->RSSetViewports(1, &vp);
	D3D11_RECT scissor = { 0, 0, (LONG)width, (LONG)height };
	context->RSSetScissorRects(1, &scissor);

	context->VSSetShader(dlaa_vshader, nullptr, 0);
	context->PSSetSamplers(0, 1, &dlaa_pointSampler);
	context->PSSetConstantBuffers(0, 1, &dlaa_cbuffer);

	context->OMSetRenderTargets(1, &dlaaIntermediateRTV, nullptr);
	context->PSSetShaderResources(0, 1, &srcSRV);
	context->PSSetShader(dlaa_pre_pshader, nullptr, 0);
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

	D3D11_BOX box = { 0, 0, 0, width, height, 1 };
	context->CopySubresourceRegion(imagesHandles[currentIndex].texture, 0,
	    0, 0, 0, dlaaOutput, 0, &box);

	context->IASetPrimitiveTopology(savedTopology);
	if (numViewports)
		context->RSSetViewports(numViewports, savedViewports);
	if (numScissors)
		context->RSSetScissorRects(numScissors, savedScissors);
	context->RSSetState(savedRS);
	if (savedRS)
		savedRS->Release();

	static bool s_logged = false;
	if (!s_logged) {
		s_logged = true;
		OOVR_LOGF("BlueSkyDefender PostAA: enabled at %ux%u lambda=%.2f epsilon=%.3f",
		    width, height,
		    oovr_global_configuration.BlueSkyDefenderLambda(),
		    oovr_global_configuration.BlueSkyDefenderEpsilon());
	}

	return true;
}

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

#if defined(OC_HAS_FSR3) || defined(OC_HAS_DLSS)
	bool bridgeResourcesNeeded = false;
#ifdef OC_HAS_FSR3
	bridgeResourcesNeeded = bridgeResourcesNeeded ||
	    (!isOverlay && Fsr3TemporalRequested());
#endif
#ifdef OC_HAS_DLSS
	bridgeResourcesNeeded = bridgeResourcesNeeded ||
	    (!isOverlay && oovr_global_configuration.DlssEnabled() &&
	        (oovr_global_configuration.FsrRenderScale() < 0.99f ||
	            oovr_global_configuration.DlssPreset() == 4));
#endif
	OCBridgeResourceSnapshot localBridgeResources;
	const auto& bridgeResources =
	    ReuseOrAcquireBridgeResourceSnapshot(localBridgeResources,
	        bridgeResourcesNeeded);
	ScopedBridgeResourceSnapshot bridgeResourceScope(&bridgeResources);
	const bool bridgeResourcesReady = bridgeResources.Ready();
#endif

	CheckCreateSwapChain(texture, bounds, false);

	// Cache a view of Skyrim's submitted texture only when a shader path will
	// actually sample it. This preserves the 3.1 optimization for inversion,
	// FSR, CAS, and DLAA without changing resource lifetime on the ordinary
	// CopySubresourceRegion path. The unconditional reference is the leading
	// suspect in the 4.x ENB/companion-window mirror regression and is not used
	// by the plain-copy path.
	const bool cachedSourceNeeded = !isOverlay &&
	    ((bounds && bounds->vMin > bounds->vMax &&
	         oovr_global_configuration.InvertUsingShaders()) ||
	        oovr_global_configuration.FsrEnabled() ||
	        oovr_global_configuration.CasEnabled() ||
	        oovr_global_configuration.DlaaEnabled());
	if (!cachedSourceNeeded) {
		if (cachedSrcSRV) {
			cachedSrcSRV->Release();
			cachedSrcSRV = nullptr;
		}
		cachedSrcTex = nullptr;
	} else if (src != cachedSrcTex) {
		if (cachedSrcSRV) {
			cachedSrcSRV->Release();
			cachedSrcSRV = nullptr;
		}
		const HRESULT hr = device->CreateShaderResourceView(src, nullptr, &cachedSrcSRV);
		cachedSrcTex = SUCCEEDED(hr) ? src : nullptr;
		if (FAILED(hr)) {
			OOVR_LOGF("D3D11: source SRV creation failed (hr=0x%08X)",
			    static_cast<unsigned>(hr));
		}
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
	D3D11_BOX sourceRegion{};
	if (!ResolveSubmittedTextureRegion(srcDesc, bounds, sourceRegion))
		OOVR_ABORTF("Invalid submitted eye texture region");

#ifdef OC_HAS_FSR3
	// FSR3 debug modes are only visible when the temporal FSR3 submit path is
	// allowed to run. Log the gates so a config fallback is obvious in the SKSE log.
	{
		static int s_fsr3GateDiagCount = 0;
		const int fsr3DbgMode = oovr_global_configuration.Fsr3DebugMode();
		if (fsr3DbgMode != 0
		    && oovr_global_configuration.FsrEnabled()
		    && Fsr3TemporalRequested()
		    && !isOverlay) {
			const bool upscalerReady = s_fsr3Upscaler && s_fsr3Upscaler->IsReady();
			const bool bridgeReady = bridgeResourcesReady;
			const bool hasMV = bridgeReady && bridgeResources.mvTexture;
			const bool mvValid = hasMV && ValidateBridgeTexture(bridgeResources.mvTexture, "MV");
			const bool motionVectorsEnabled = oovr_global_configuration.MotionVectorsEnabled();
			const bool swapchainReady = !swapchain_rtvs.empty();
			const bool wouldEnterFsr3 = upscalerReady && bridgeReady && hasMV && mvValid
			    && motionVectorsEnabled && swapchainReady;
			if (!wouldEnterFsr3 && (s_fsr3GateDiagCount < 8 || (s_fsr3GateDiagCount % 300) == 0)) {
				OOVR_LOGF("FSR3-GATE: debug=%d skipped (ready=%d bridge=%d mv=%d mvValid=%d motionVectorsEnabled=%d swapchain=%d bounds=%d)",
				    fsr3DbgMode,
				    upscalerReady ? 1 : 0,
				    bridgeReady ? 1 : 0,
				    hasMV ? 1 : 0,
				    mvValid ? 1 : 0,
				    motionVectorsEnabled ? 1 : 0,
				    swapchainReady ? 1 : 0,
				    bounds ? 1 : 0);
			}
			s_fsr3GateDiagCount++;
		}
	}
#endif

	bool copiedWithVerticalFlip = false;
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
		copiedWithVerticalFlip = true;
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
	    && bridgeResourcesReady && bridgeResources.mvTexture
	    && ValidateBridgeTexture(bridgeResources.mvTexture, "MV")
	    && oovr_global_configuration.MotionVectorsEnabled()
	    && oovr_global_configuration.FsrEnabled()
	    && Fsr3TemporalRequested()
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
			auto* mvTex = bridgeResources.mvTexture;
			auto* depthTex = bridgeResources.depthTexture;
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
					int reactiveDepthOffX = 0;
					int reactiveDepthOffY = 0;
					bool reactiveDepthStereo = (dDesc.Width >= perEyeRenderW * 2 - 4);
					if (reactiveDepthStereo && s_currentEyeIdx == 1)
						reactiveDepthOffX = (int)(dDesc.Width / 2);

					ID3D11ShaderResourceView* rmColorSRV = nullptr;
					if (EnsureReactiveMaskColorCopyResources(device, perEyeRenderW, perEyeRenderH, fsrSrcDesc.Format)) {
						if (colorRegionPtr) {
							context->CopySubresourceRegion(s_reactiveMaskColorTex, 0,
							    0, 0, 0, fsrSrc, 0, colorRegionPtr);
						} else {
							D3D11_BOX colorBox = { 0, 0, 0, perEyeRenderW, perEyeRenderH, 1 };
							context->CopySubresourceRegion(s_reactiveMaskColorTex, 0,
							    0, 0, 0, fsrSrc, 0, &colorBox);
						}
						rmColorSRV = GetOrCreateReactiveMaskColorSRV(device, s_reactiveMaskColorTex);
					}

					if (EnsureReactiveMaskResources(device, perEyeRenderW, perEyeRenderH)) {
						auto* rmDepthSRV = GetOrCreateReactiveMaskDepthSRV(device, depthTex);
						if (rmDepthSRV) {
							GenerateReactiveMask(context, rmDepthSRV, rmColorSRV,
							    perEyeRenderW, perEyeRenderH,
							    perEyeRenderW, perEyeRenderH,
							    reactiveDepthOffX, reactiveDepthOffY,
							    oovr_global_configuration.Fsr3ReactiveBase(),
							    oovr_global_configuration.Fsr3ReactiveEdgeBoost(),
							    0.005f, // edge threshold
							    30.0f,  // edge scale
							    oovr_global_configuration.Fsr3ReactiveColorBoost(),
							    oovr_global_configuration.Fsr3ReactiveColorThreshold(),
							    oovr_global_configuration.Fsr3ReactiveColorScale(),
							    oovr_global_configuration.Fsr3ReactiveDepthFalloffStart(),
							    oovr_global_configuration.Fsr3ReactiveDepthFalloffEnd());
							reactiveMaskTex = s_reactiveMaskTex;
							{
								static bool s = false;
								if (!s) {
									s = true;
									OOVR_LOGF("ReactiveMask: FSR3 per-eye %ux%u colorSRV=%d depthOff=(%d,%d) base=%.3f edge=%.3f color=%.3f",
									    perEyeRenderW, perEyeRenderH, rmColorSRV ? 1 : 0,
									    reactiveDepthOffX, reactiveDepthOffY,
									    oovr_global_configuration.Fsr3ReactiveBase(),
									    oovr_global_configuration.Fsr3ReactiveEdgeBoost(),
									    oovr_global_configuration.Fsr3ReactiveColorBoost());
								}
							}
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

					// Apply locomotion injection to curVP (OFF by default — see fsr3LocoInjection
					// in Config.h. The raw VP delta already carries camera translation, so this
					// double-counted it and produced the FSR/ASW+MV double image while moving.)
					if (oovr_global_configuration.Fsr3LocoInjection()
					    && (s_cmvLocoDx != 0.0f || s_cmvLocoDy != 0.0f || s_cmvLocoDz != 0.0f)) {
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

							// Match DLSS camera-MV jitter handling: RSS matrices are unjittered,
							// so unjitter the current pixel coordinates before reprojection and
							// pass the jitter offset to the temporal upscaler separately.
							float curJitterX = s_fsr3RenderJitterX;
							float curJitterY = s_fsr3RenderJitterY;
							float jdUVx = 0.0f;
							float jdUVy = 0.0f;
							float currJUVx = curJitterX / (float)perEyeRenderW;
							float currJUVy = curJitterY / (float)perEyeRenderH;

							// DIAG: head-motion magnitude. Measure clipToClip deviation
							// from identity — proxies total camera transform between frames.
							// Also extract translation portion (entries 3,7,11).
							if (oovr_debug_logging_enabled()) {
								static int s_fsr3MVDiagCounter = 0;
								s_fsr3MVDiagCounter++;
								if (s_fsr3MVDiagCounter % 30 == 0 && eye == 0) {
									float identDev = 0.0f;
									float identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
									for (int i = 0; i < 16; i++)
										identDev += fabsf(clipToClipMat[i] - identity[i]);
									// Translation column (column-major): entries 12,13,14
									float tx = clipToClipMat[12], ty = clipToClipMat[13], tz = clipToClipMat[14];
									int menuFlag = (s_pBridge && s_pBridge->isMainMenu) ? 1 : 0;
									int loadFlag = (s_pBridge && s_pBridge->isLoadingScreen) ? 1 : 0;
									int firstDisp = s_fsr3FirstDispatch ? 1 : 0;
									int jitEn = g_fsr3JitterEnabled ? 1 : 0;
									OOVR_LOGF("FSR3-MVDIAG: eye=%d frame=%d c2c_devFromIdent=%.6f c2c_trans=(%.6f,%.6f,%.6f) "
									          "jitterCurr=(%.4f,%.4f) currJitterUV=(%.6f,%.6f) loco=(%.4f,%.4f,%.4f) "
									          "menu=%d load=%d firstDisp=%d jitEn=%d gJ=(%.4f,%.4f)",
									    eye, s_fsr3MVDiagCounter,
									    identDev, tx, ty, tz,
									    curJitterX, curJitterY,
									    jdUVx, jdUVy,
									    s_cmvLocoDx, s_cmvLocoDy, s_cmvLocoDz,
									    menuFlag, loadFlag, firstDisp, jitEn,
									    g_fsr3JitterX, g_fsr3JitterY);
								}
							}

							auto* gameMVSRV = bridgeResources.mvSRV;
							int gameMVOffX = 0;
							if (gameMVSRV && (mvDesc.Width >= perEyeRenderW * 2 - 4) && eye == 1)
								gameMVOffX = (int)(mvDesc.Width / 2);
							bool useGameMV = oovr_global_configuration.ActorMV() && gameMVSRV;

							if (EnsureCameraMVResources(device, perEyeRenderW, perEyeRenderH)) {
								auto* cmvDepthSRV = GetOrCreateCameraMVDepthSRV(device, depthTex);
								if (cmvDepthSRV) {
									static int s_fsr3CameraMVStatsCounter = 0;
									static bool s_fsr3CameraMVStatsWasGameplay = false;
									bool enableStats = false;
									bool inGameplay = !(s_pBridge && (s_pBridge->isMainMenu || s_pBridge->isLoadingScreen));
									if (!inGameplay) {
										s_fsr3CameraMVStatsWasGameplay = false;
									} else if (!s_fsr3CameraMVStatsWasGameplay) {
										s_fsr3CameraMVStatsWasGameplay = true;
										s_fsr3CameraMVStatsCounter = 0;
									}
									if (oovr_global_configuration.Fsr3DebugMode() == 7 && eye == 0 && inGameplay) {
										s_fsr3CameraMVStatsCounter++;
										enableStats = (s_fsr3CameraMVStatsCounter <= 10
										    || (s_fsr3CameraMVStatsCounter % 120) == 0);
									}
									GenerateCameraMVs(context, cmvDepthSRV,
									    perEyeRenderW, perEyeRenderH,
									    clipToClipMat,
									    depthOffX, depthOffY,
									    jdUVx, jdUVy,
									    currJUVx, currJUVy,
									    gameMVSRV, gameMVOffX, 0,
									    useGameMV, enableStats, eye);

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
				fsr3Params.reactiveSourceRegion = nullptr; // Reactive mask is generated per-eye
				fsr3Params.jitterX = s_fsr3RenderJitterX; // Use the jitter that was applied to rendering
				fsr3Params.jitterY = s_fsr3RenderJitterY;
				fsr3Params.deltaTimeMs = deltaMs;
				fsr3Params.renderWidth = perEyeRenderW;
				fsr3Params.renderHeight = perEyeRenderH;
				float fsr3InvScale = 1.0f / std::max(0.5f, Fsr3EffectiveRenderScale());
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
				const uint8_t bridgeResetEyeBit =
				    static_cast<uint8_t>(1u << s_currentEyeIdx);
				fsr3Params.reset = s_fsr3FirstDispatch
				    || (s_bridgeTemporalResetEyeMask & bridgeResetEyeBit) != 0
				    || (s_pBridge && (s_pBridge->isMainMenu || s_pBridge->isLoadingScreen || s_pBridge->isMenuOpen));
				// Camera MVs are generated in unjittered UV space, matching the DLSS path.
				// FSR3 still receives the jitter offset separately; context-level jitter
				// cancellation should only be enabled for MV sources that already include jitter.
				fsr3Params.jitterCancellation = oovr_global_configuration.Fsr3JitterCancellation();
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
				// Mode 4: Final MV visualization — render motion vectors as RG color via shader
				// Mode 5: Residual MV visualization — show game/object MV minus camera MV
				// Mode 6: Raw bridge MV visualization — no camera reconstruction, depth gate, or dilation
				// Mode 7: Bridge fallback mask — white where camera MV replaced missing/weak bridge MV
				// Mode 8: Reactive mask — white where FSR3 is biased toward current-frame data
				// When camera MVs are available, show those (what FSR3 actually uses);
				// otherwise fall back to the game's bridge MV texture.
				// (CopySubresourceRegion from R32F/R16G16F → R8G8B8A8 silently fails)
				if ((fsr3DbgMode == 3 && depthTex) || fsr3DbgMode == 4 || fsr3DbgMode == 5
				    || fsr3DbgMode == 6 || fsr3DbgMode == 7 || fsr3DbgMode == 8) {
					EnsureDebugShaders(device);
					bool residualMode = (fsr3DbgMode == 5);
					bool rawBridgeMode = (fsr3DbgMode == 6);
					bool fallbackMaskMode = (fsr3DbgMode == 7);
					bool reactiveMode = (fsr3DbgMode == 8);
					ID3D11PixelShader* vizPS = reactiveMode ? s_debugReactivePS
					                         : ((fsr3DbgMode == 3 || fallbackMaskMode) ? s_debugDepthPS
					                         : ((residualMode || rawBridgeMode) ? s_debugResidualMvPS : s_debugMvPS));
					ID3D11Texture2D* vizTex = nullptr;
					if (fsr3DbgMode == 3)
						vizTex = depthTex;
					else if (rawBridgeMode)
						vizTex = mvTex;
					else if (fallbackMaskMode)
						vizTex = s_cameraMVFallbackMaskTex;
					else if (reactiveMode)
						vizTex = reactiveMaskTex;
					else if (residualMode)
						vizTex = s_cameraMVResidualTex;
					else
						vizTex = cameraMVTex ? cameraMVTex : mvTex;

					if (!vizTex) {
						static int missingVizLog = 0;
						if (missingVizLog++ < 10)
							OOVR_LOGF("FSR3-DEBUG: Mode %d has no source texture (depth=%d reactive=%d cameraMV=%d)",
							    fsr3DbgMode, depthTex ? 1 : 0, reactiveMaskTex ? 1 : 0, cameraMVTex ? 1 : 0);
					}

					if (vizPS && vizTex) {
						// Compute sub-region UVs for the per-eye portion
						// Camera MVs are already per-eye (no sub-region needed)
						D3D11_TEXTURE2D_DESC vizDesc;
						vizTex->GetDesc(&vizDesc);
						float uvMin[2] = { 0.0f, 0.0f };
						float uvMax[2] = { 1.0f, 1.0f };
						D3D11_BOX* regionPtr = nullptr;
						if (fsr3DbgMode == 3)
							regionPtr = depthRegionPtr;
						else if (rawBridgeMode)
							regionPtr = &mvRegion;
						else if (fsr3DbgMode == 4 && !cameraMVTex)
							regionPtr = &mvRegion;
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
						} else if (fallbackMaskMode) {
							srvDesc.Format = DXGI_FORMAT_R16_FLOAT;
						} else if (reactiveMode) {
							srvDesc.Format = DXGI_FORMAT_R8_UNORM;
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
									    fsr3DbgMode, fsr3DbgMode == 3 ? "depth" : (reactiveMode ? "reactive mask" : (fallbackMaskMode ? "bridge fallback mask" : (rawBridgeMode ? "raw bridge MV" : (residualMode ? "residual MV" : "final MV")))),
									    uvMin[0], uvMin[1], uvMax[0], uvMax[1],
									    vizDesc.Format, vizDesc.Width, vizDesc.Height);
							}
						}
					}
					goto fsr3_end;
				}

				bool fsr3Ok = s_fsr3Upscaler->Dispatch(s_currentEyeIdx, context, fsr3Params);
				s_fsr3FirstDispatch = false;
				if (fsr3Ok)
					s_bridgeTemporalResetEyeMask &= static_cast<uint8_t>(~bridgeResetEyeBit);

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
					if (!ApplyFsr3PostAA(fsr3Output, s_currentEyeIdx, currentIndex, perEyeDisplayW, perEyeDisplayH)) {
						// FSR3 has built-in RCAS sharpening — direct copy unless post-AA is enabled.
						context->CopySubresourceRegion(imagesHandles[currentIndex].texture, 0,
						    0, 0, 0, fsr3Output, 0, nullptr);
					}

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
	    && bridgeResourcesReady && bridgeResources.mvTexture
	    && ValidateBridgeTexture(bridgeResources.mvTexture, "MV")
	    && oovr_global_configuration.DlssEnabled()
	    && (oovr_global_configuration.FsrRenderScale() < 0.99f || oovr_global_configuration.DlssPreset() == 4)
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
		auto* dlssMVTex = bridgeResources.mvTexture;
		auto* dlssDepthTex = bridgeResources.depthTexture;
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

				// Apply locomotion injection to curVP (DLSS path; gated by fsr3LocoInjection, off by default — same double-count as the FSR path)
				if (oovr_global_configuration.Fsr3LocoInjection()
				    && (s_cmvLocoDx != 0.0f || s_cmvLocoDy != 0.0f || s_cmvLocoDz != 0.0f)) {
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

						// DIAG: head-motion magnitude. Same computation as FSR3 path
						// for apples-to-apples comparison in the logs.
						if (oovr_debug_logging_enabled()) {
							static int s_dlssMVDiagCounter = 0;
							s_dlssMVDiagCounter++;
							if (s_dlssMVDiagCounter % 30 == 0 && eye == 0) {
								float identDev = 0.0f;
								float identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
								for (int i = 0; i < 16; i++)
									identDev += fabsf(clipToClipMat[i] - identity[i]);
								float tx = clipToClipMat[12], ty = clipToClipMat[13], tz = clipToClipMat[14];
								OOVR_LOGF("DLSS-MVDIAG: eye=%d frame=%d c2c_devFromIdent=%.6f c2c_trans=(%.6f,%.6f,%.6f) "
								          "jitterCurr=(%.4f,%.4f) currJitterUV=(%.6f,%.6f) loco=(%.4f,%.4f,%.4f)",
								    eye, s_dlssMVDiagCounter,
								    identDev, tx, ty, tz,
								    s_fsr3RenderJitterX, s_fsr3RenderJitterY,
								    currJUVx, currJUVy,
								    s_cmvLocoDx, s_cmvLocoDy, s_cmvLocoDz);
							}
						}

						auto* gameMVSRV = bridgeResources.mvSRV;
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
								    useGameMV, false, eye);
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
					auto* rmColorSRV = GetOrCreateReactiveMaskColorSRV(device, dlssSrc);
					if (rmDepthSRV) {
						GenerateReactiveMask(context, rmDepthSRV, rmColorSRV,
						    dDesc.Width, dDesc.Height,
						    dlssSrcDesc.Width, dlssSrcDesc.Height,
						    0, 0,
						    oovr_global_configuration.DlssBiasBase(),
						    oovr_global_configuration.DlssBiasEdgeBoost(),
						    0.005f, // edge threshold
						    30.0f,  // edge scale
						    0.0f,
						    oovr_global_configuration.Fsr3ReactiveColorThreshold(),
						    oovr_global_configuration.Fsr3ReactiveColorScale(),
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
			const uint8_t bridgeResetEyeBit =
			    static_cast<uint8_t>(1u << s_currentEyeIdx);
			dlssParams.reset = s_fsr3FirstDispatch
			    || (s_bridgeTemporalResetEyeMask & bridgeResetEyeBit) != 0
			    || (s_pBridge && (s_pBridge->isMainMenu || s_pBridge->isLoadingScreen || s_pBridge->isMenuOpen));
			dlssParams.debugMode = 0;
			s_fsr3FirstDispatch = false;

			bool dlssOk = s_dlssUpscaler->Dispatch(s_currentEyeIdx, context, dlssParams);
			if (dlssOk)
				s_bridgeTemporalResetEyeMask &= static_cast<uint8_t>(~bridgeResetEyeBit);

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

	// ── Unsharp Mask sharpening post-pass ──
	// Simple and effective: blur → subtract from original → add scaled difference.
	// casSharpness controls intensity (0 = off, 1 = strong, 2+ = extreme).
	if (oovr_global_configuration.CasEnabled() && !isOverlay
	    && oovr_global_configuration.CasSharpness() > 0.0f
	    && dlaaOutput && !swapchain_rtvs.empty()) {

		// Lazy-compile unsharp mask compute shader
		static ID3D11ComputeShader* s_unsharpCS = nullptr;
		static bool s_compileFailed = false;
		if (!s_unsharpCS && !s_compileFailed) {
			static const char hlsl[] = R"(
Texture2D<float4> Input : register(t0);  // SRGB SRV — reads return linear values
RWTexture2D<float4> Output : register(u0);  // UNORM UAV — must write SRGB-encoded
cbuffer CB : register(b0) { float strength; float3 _pad; };

// Linear → sRGB encoding (D3D11 UAVs don't support auto SRGB encode)
float3 LinearToSRGB(float3 c) {
    float3 lo = c * 12.92;
    float3 hi = 1.055 * pow(abs(c), 1.0/2.4) - 0.055;
    return (c <= 0.0031308) ? lo : hi;
}

[numthreads(8, 8, 1)]
void CS(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    Output.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;

    float3 center = Input.Load(int3(id.xy, 0)).rgb;

    // Luma-weighted unsharp mask — sharpens luminance only (no color fringing)
    // 5-tap cross blur
    float3 blur = center;
    blur += Input.Load(int3(id.xy + int2(-1, 0), 0)).rgb;
    blur += Input.Load(int3(id.xy + int2( 1, 0), 0)).rgb;
    blur += Input.Load(int3(id.xy + int2( 0,-1), 0)).rgb;
    blur += Input.Load(int3(id.xy + int2( 0, 1), 0)).rgb;
    blur *= 0.2;

    // Sharpen in linear space
    float3 sharp = saturate(center + strength * (center - blur));

    Output[id.xy] = float4(LinearToSRGB(sharp), Input.Load(int3(id.xy, 0)).a);
}
)";
			ID3DBlob* blob = nullptr;
			ID3DBlob* errs = nullptr;
			HRESULT hr = D3DCompile(hlsl, sizeof(hlsl) - 1, "UnsharpMask", nullptr, nullptr,
			    "CS", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &errs);
			if (SUCCEEDED(hr) && blob) {
				device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(),
				    nullptr, &s_unsharpCS);
				blob->Release();
			} else {
				s_compileFailed = true;
				if (errs) { OOVR_LOGF("Unsharp CS compile failed: %s", (char*)errs->GetBufferPointer()); }
			}
			if (errs) errs->Release();

			// Also create a CB for the strength parameter
			if (s_unsharpCS) {
				OOVR_LOG("Unsharp mask shader compiled OK");
			}
		}

		static ID3D11Buffer* s_unsharpCB = nullptr;
		if (s_unsharpCS && !s_unsharpCB) {
			D3D11_BUFFER_DESC cbd = {};
			cbd.ByteWidth = 16;
			cbd.Usage = D3D11_USAGE_DYNAMIC;
			cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			device->CreateBuffer(&cbd, nullptr, &s_unsharpCB);
		}

		if (s_unsharpCS && s_unsharpCB) {
			uint32_t sharpW = s_fsr3ViewportW > 0 ? s_fsr3ViewportW : createInfo.width;
			uint32_t sharpH = s_fsr3ViewportH > 0 ? s_fsr3ViewportH : createInfo.height;

			// Copy swapchain → staging
			D3D11_BOX box = { 0, 0, 0, sharpW, sharpH, 1 };
			context->CopySubresourceRegion(dlaaOutput, 0, 0, 0, 0,
			    imagesHandles[currentIndex].texture, 0, &box);

			// Update strength CB
			D3D11_MAPPED_SUBRESOURCE mapped;
			if (SUCCEEDED(context->Map(s_unsharpCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
				float* cb = (float*)mapped.pData;
				cb[0] = oovr_global_configuration.CasSharpness();
				cb[1] = cb[2] = cb[3] = 0;
				context->Unmap(s_unsharpCB, 0);
			}

			// Write to dlaaOutput itself as both input AND output — wait, can't do that.
			// Instead: read from dlaaOutput (staging copy of swapchain), write to
			// dlaaIntermediate (separate texture), then copy intermediate → swapchain.
			ID3D11UnorderedAccessView* sharpUAV = nullptr;
			{
				D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
				uavDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
				uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
				device->CreateUnorderedAccessView(dlaaIntermediate, &uavDesc, &sharpUAV);
			}

			if (sharpUAV) {
				ID3D11ComputeShader* oldCS = nullptr;
				context->CSGetShader(&oldCS, nullptr, nullptr);

				context->CSSetShader(s_unsharpCS, nullptr, 0);
				context->CSSetShaderResources(0, 1, &dlaaOutputSRV);
				context->CSSetUnorderedAccessViews(0, 1, &sharpUAV, nullptr);
				context->CSSetConstantBuffers(0, 1, &s_unsharpCB);
				context->Dispatch((sharpW + 7) / 8, (sharpH + 7) / 8, 1);

				// Unbind
				ID3D11ShaderResourceView* nullSRV = nullptr;
				ID3D11UnorderedAccessView* nullUAV = nullptr;
				context->CSSetShaderResources(0, 1, &nullSRV);
				context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
				context->CSSetShader(oldCS, nullptr, 0);
				if (oldCS) oldCS->Release();

				// Copy sharpened result back to swapchain
				context->CopySubresourceRegion(imagesHandles[currentIndex].texture, 0,
				    0, 0, 0, dlaaIntermediate, 0, &box);
				context->Flush();
				sharpUAV->Release();
			}
		}
	}

	// Fill deliberately omitted pixels after every filter, in the acquired eye
	// itself. A newer gaze sample or unavailable overlay must not expose them.
	const auto blackout = ocu_effect_foveation::GetState().ReadBlackout();
	if (!isOverlay && blackout.Active()) {
		D3D11_VIEWPORT viewport{};
		viewport.Width = float(bounds && s_fsr3ViewportW > 0 ? s_fsr3ViewportW : createInfo.width);
		viewport.Height = float(bounds && s_fsr3ViewportH > 0 ? s_fsr3ViewportH : createInfo.height);
		viewport.MaxDepth = 1.f;
		// Match the physical row inversion above. Swapping OpenXR FOV angles
		// without flipping the copied texture leaves scene UV coordinates intact.
		if (!s_blackoutRenderer.Apply(context, swapchain_rtvs[currentIndex], blackout,
		        s_currentEyeIdx, viewport, copiedWithVerticalFlip)) {
			// This image already contains omissions. Hide it and render complete
			// frames thereafter if presentation resources fail unexpectedly.
			const float black[4] = {0.f, 0.f, 0.f, 1.f};
			Microsoft::WRL::ComPtr<ID3D11Predicate> predicate;
			BOOL predicateValue = FALSE;
			context->GetPredication(&predicate, &predicateValue);
			context->SetPredication(nullptr, FALSE);
			context->ClearRenderTargetView(swapchain_rtvs[currentIndex], black);
			context->SetPredication(predicate.Get(), predicateValue);
			s_blackoutPresentationFailed = true;
			OOVR_LOG("Foveation blackout presentation failed; image hidden and subsequent scene culling disabled");
		}
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
	// All game-side effects must already be complete before the first Submit.
	// Prevent later compositor/overlay work from consuming an old active profile.
	ocu_effect_foveation::Clear();
	const vr::Texture_t* gameTexture = texture;
#if defined(OC_HAS_FSR3) || defined(OC_HAS_DLSS)
	// Reset upscaler viewport crop — set by FSR3/DLSS dispatch if it runs this frame
	s_fsr3ViewportW = 0;
	s_fsr3ViewportH = 0;
#endif

	// VRS was bound at WaitGetPoses, before Skyrim rendered this frame. Clear it
	// before OCU's compositor/upscaler passes so coarse shading never touches UI,
	// motion/depth processing, or the OpenXR submission copy.
	if (s_vrsFrameArmed && s_vrsHookManager) {
		const auto shaders = ocu_vrs_guard::Counts();
		OOVR_LOG_LIMITEDF(5000, "VRS coverage guard v1 terrain-depth-guard-v1 frame: source=%s sceneBindings=%u coarseStateChanges=%u fullRateStateChanges=%u unclassifiedStateChanges=%u terrainDepthStateChanges=%u capturedShaders=(compatible=%llu protected=%llu unknown=%llu terrainDepth=%llu)",
		    s_vrsSceneScope.UsesDepth() ? "main-depth" : "submitted-color fallback", s_vrsSceneBindings,
		    s_vrsCoarseBindings, s_vrsProtectedBindings, s_vrsUnclassifiedBindings, s_vrsTerrainDepthBindings,
		    (unsigned long long)shaders.compatible, (unsigned long long)shaders.protectedShaders,
		    (unsigned long long)shaders.unclassified, (unsigned long long)shaders.rasterDepthTextureLoad);
		const auto alpha = s_vrsAlphaCoverageScope.Stats();
		OOVR_LOG_LIMITEDF(5000, "VRS cutout-material-guard-v1 frame: depthDraws=%u materials=%u protectedDraws=%u stateQueries=%u resets=%u unknownCommandLists=%u untrackedDepthDraws=%u ambiguousDraws=%u",
		    alpha.depthDraws, alpha.materials, alpha.protectedDraws, alpha.stateQueries, alpha.resets,
		    alpha.unknownCommandLists, alpha.untrackedDepthDraws, alpha.ambiguousDraws);
	}
	DisarmSceneVRS();
	if (vrsManager.IsAvailable())
		vrsManager.Disable();
	if (!oovr_global_configuration.VrsAnyEnabled()) {
		s_vrsPatternReady = false;
		s_vrsHasSmoothedGaze = false;
	}

	// Set current eye index for FSR radius matching (inner Invoke reads this)
	s_currentEyeIdx = (eye == XruEyeLeft) ? 0 : 1;
    LogRDMFrame(densityMaskManager, rdmDiagnosticSchedule.ConsumeReport());

	// The render-target bridge is only consumed by the temporal upscalers and
	// space-warp paths.  The ordinary compositor must not pin seven bridge COM
	// resources twice per frame when all of those features are disabled.
	bool bridgeResourcesNeeded = oovr_global_configuration.ASWEnabled() ||
	    g_aswProvider != nullptr;
#ifdef OC_HAS_FSR3
	bridgeResourcesNeeded = bridgeResourcesNeeded || Fsr3TemporalRequested();
#endif
#ifdef OC_HAS_DLSS
	bridgeResourcesNeeded = bridgeResourcesNeeded ||
	    (oovr_global_configuration.DlssEnabled() &&
	        (oovr_global_configuration.FsrRenderScale() < 0.99f ||
	            oovr_global_configuration.DlssPreset() == 4));
#endif

	OCBridgeResourceSnapshot bridgeResources;
	const bool bridgeSnapshotValid = bridgeResourcesNeeded &&
	    AcquireBridgeResourceSnapshot(bridgeResources);
	ScopedBridgeResourceSnapshot bridgeResourceScope(&bridgeResources);
	const bool bridgeResourcesReady = bridgeSnapshotValid && bridgeResources.Ready();
	static uint64_t s_activeBridgeGeneration = 0;
	if (bridgeSnapshotValid &&
	    bridgeResources.generation != 0 &&
	    bridgeResources.generation != s_activeBridgeGeneration) {
		s_activeBridgeGeneration = bridgeResources.generation;
#if defined(OC_HAS_FSR3) || defined(OC_HAS_DLSS)
		// A recreated MV/depth set is a temporal discontinuity. Neither eye may
		// carry history or jitter bookkeeping across this generation boundary.
		s_fsr3FirstDispatch = true;
		s_temporalJitterSubmittedEyeMask = 0;
		s_bridgeTemporalResetEyeMask = 0x3u;
#endif
		if (g_aswProvider) {
			// Issue 2's stereo cache tracks successful eye copies. Invalidate its
			// in-progress pair so generations can never be mixed.
			g_aswProvider->InvalidateCachedFrame();
		}
		OOVR_LOGF(
		    "RT Bridge v2: generation=%llu status=%u MV=%ux%u depth=%ux%u "
		    "(temporal history reset)",
		    static_cast<unsigned long long>(bridgeResources.generation),
		    bridgeResources.status,
		    bridgeResources.mvWidth,
		    bridgeResources.mvHeight,
		    bridgeResources.depthWidth,
		    bridgeResources.depthHeight);
	}

#ifdef OC_HAS_FSR3
	// Capture per-eye pose and FOV for camera MV computation (inner Invoke reads these)
	s_fsr3EyePose[s_currentEyeIdx] = layer.pose;
	s_fsr3EyeFov[s_currentEyeIdx] = layer.fov;
#endif

#ifdef OC_HAS_FSR3
	// ── FSR 3: lazy-init upscaler and update per-frame jitter ──
	if (Fsr3TemporalRequested()) {
		// Try to open the SKSE render target bridge (MV + depth)
		OpenRenderTargetBridge();

		// Lazy-init the FSR 3 upscaler (DX12 device + FidelityFX DLLs)
		if (!s_fsr3Upscaler && bridgeResourcesReady) {
			s_fsr3Upscaler = new Fsr3Upscaler();
			if (!s_fsr3Upscaler->Initialize(device)) {
				OOVR_LOG("FSR3: Initialization failed — falling back to FSR 1");
				delete s_fsr3Upscaler;
				s_fsr3Upscaler = nullptr;
			}
		}


		// Save jitter and camera FOV for the current submitted eye.
		// IMPORTANT: Do NOT compute next-frame jitter here — right eye's GetProjectionRaw
		// hasn't been called yet and would pick up the wrong (next) jitter value.
		if (s_fsr3Upscaler && s_fsr3Upscaler->IsReady()) {
			// No jitter on main menu / loading screen — spatial-only upscale (reset=true).
			// Also no jitter when motion vectors are OFF: the FSR3 temporal dispatch is gated
			// on MotionVectorsEnabled() (see the entry condition ~4595), so with MVs off it is
			// skipped and nothing resolves the sub-pixel jitter — the game would render jittered
			// frames that never get reconstructed, showing as shimmer/jitter while moving.
			if (!oovr_global_configuration.MotionVectorsEnabled()
			    || (s_pBridge && (s_pBridge->isMainMenu || s_pBridge->isLoadingScreen))) {
				g_fsr3JitterEnabled = false;
				s_temporalJitterSubmittedEyeMask = 0;
			} else {
				// Save the jitter that was applied to THIS frame's rendering
				// (g_fsr3JitterX/Y advance after the previous complete stereo frame)
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
		s_temporalJitterSubmittedEyeMask = 0;
	}
#endif

#ifdef OC_HAS_DLSS
	// ── DLSS: save jitter and enable on left eye ──
	if (oovr_global_configuration.DlssEnabled()
	    && (oovr_global_configuration.FsrRenderScale() < 0.99f || oovr_global_configuration.DlssPreset() == 4)
	    && s_dlssUpscaler && s_dlssUpscaler->IsReady()) {
		if (s_pBridge && (s_pBridge->isMainMenu || s_pBridge->isLoadingScreen)) {
			g_fsr3JitterEnabled = false;
			s_temporalJitterSubmittedEyeMask = 0;
		} else {
			s_fsr3RenderJitterX = g_fsr3JitterX;
			s_fsr3RenderJitterY = g_fsr3JitterY;
			g_fsr3JitterEnabled = true;
		}
	} else if (!oovr_global_configuration.FsrEnabled()
	    && !(s_dlssUpscaler && s_dlssUpscaler->IsReady())) {
		g_fsr3JitterEnabled = false;
		s_temporalJitterSubmittedEyeMask = 0;
	}

	// ── DLSS 4: lazy-init upscaler ──
	// DLSS is mutually exclusive with FSR3 — only one activates at a time.
	if (oovr_global_configuration.DlssEnabled()
	    && (oovr_global_configuration.FsrRenderScale() < 0.99f || oovr_global_configuration.DlssPreset() == 4)) {
		static bool s_dlssInitFailed = false; // Prevent retrying every frame (~300ms per attempt)
		OpenRenderTargetBridge();
		if (!s_dlssUpscaler && !s_dlssInitFailed && bridgeResourcesReady) {
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
		if (!g_aswProvider && bridgeResourcesReady) {
			// Start at the submitted eye size; CacheFrame adapts to later size changes.
			auto* src = (ID3D11Texture2D*)texture->handle;
			D3D11_TEXTURE2D_DESC srcDesc;
			D3D11_BOX submittedRegion = {};
			if (SafeGetTextureDesc(src, &srcDesc)
			    && ResolveSubmittedTextureRegion(srcDesc, ptrBounds, submittedRegion)) {
				uint32_t renderEyeW = submittedRegion.right - submittedRegion.left;
				uint32_t renderEyeH = submittedRegion.bottom - submittedRegion.top;
                g_aswProvider = new ASWProvider();
                if (!g_aswProvider->Initialize(device, renderEyeW, renderEyeH)) {
                    OOVR_LOG("DAPA: initialization failed — disabling");
                    delete g_aswProvider;
                    g_aswProvider = nullptr;
                } else {
                    OOVR_LOG("DAPA: Using PC-side depth/parallax reprojection");
                }
			}
		}
	}

	// Copy the texture across
	Invoke(texture, ptrBounds);

	// UI color is not a world-depth surface. Suspend DAPA for every gameplay
	// menu as well as loading/main, including menus that do not pause simulation.
	if (g_aswProvider)
		g_aswProvider->SetPaused(OCBridge_DapaMenuPaused());

	// Reset optional per-view extension data before the current frame is assembled.
	layer.next = nullptr;

	// OCU ASW: cache frame data (color + MV + depth + pose) for warping
	// A menu pause invalidates the previous pair and forbids caching menu color.
	if (g_aswProvider && g_aswProvider->IsReady()
	    && g_aswProvider->IsInjectionWanted() // auto-native: no injection → skip cache copies
	    && bridgeResourcesReady && bridgeResources.mvTexture
	    && !g_aswProvider->IsPaused()
	    && ValidateBridgeTexture(bridgeResources.mvTexture, "ASW-MV")) {

		auto* mvTex = bridgeResources.mvTexture;
		auto* depthTex = bridgeResources.depthTexture;
		if (depthTex && !ValidateBridgeTexture(depthTex, "ASW-Depth"))
			depthTex = nullptr;

		D3D11_TEXTURE2D_DESC mvDesc;
		bool mvDescOk = SafeGetTextureDesc(mvTex, &mvDesc);

		if (mvDescOk) {
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

			ID3D11Texture2D* colorSrc = (ID3D11Texture2D*)texture->handle;
			D3D11_TEXTURE2D_DESC colorDesc;
			D3D11_BOX colorRegion = {};
			const bool colorFlipV = ptrBounds && ptrBounds->vMin > ptrBounds->vMax;

#ifdef OC_HAS_FSR3
			// Warp-source selection, gated on motion vectors:
			//  - MV OFF: warp the FSR3 display-res output (crisp). No temporal MVs means
			//    no warp-of-warp, so caching the already-upscaled frame is safe and sharp.
			//  - MV ON: DON'T cache the upscaled output — fall through to the render-res game
			//    frame (colorSrc stays texture->handle) so ASW warps the pre-upscale image and
			//    the runtime upscales the warp. Softer, but it breaks the double-image
			//    (warp-of-warp) that only occurs when FSR3 temporal MVs are active.
			if (!oovr_global_configuration.MotionVectorsEnabled()
			    && !colorFlipV
			    && s_fsr3Upscaler && s_fsr3Upscaler->IsReady()
			    && oovr_global_configuration.FsrEnabled()) {
				ID3D11Texture2D* fsr3Out = s_fsr3Upscaler->GetOutputDX11(eyeIdx);
				if (fsr3Out) {
					colorSrc = fsr3Out;
					if (SafeGetTextureDesc(colorSrc, &colorDesc)) {
						colorRegion = { 0, 0, 0, colorDesc.Width, colorDesc.Height, 1 };
					}
				}
			}
#endif
			if (colorRegion.right == 0 && SafeGetTextureDesc(colorSrc, &colorDesc)) {
				ResolveSubmittedTextureRegion(colorDesc, ptrBounds, colorRegion);
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
						uint32_t depthEyeW = depthDesc.Width / 2;
						depthRegion.left = eyeIdx * depthEyeW;
						depthRegion.right = depthRegion.left + depthEyeW;
						depthRegion.top = 0;
						depthRegion.bottom = depthDesc.Height;
						depthRegion.front = 0;
						depthRegion.back = 1;

						// Extract main depth (overwrites s_depthR32F)
						auto* depthSRV = GetOrCreateDepthSRV(device, depthTex,
						    depthDesc.Format, depthDesc.Width, depthDesc.Height);
						if (depthSRV && ExtractDepthToR32F(context, depthSRV,
						        depthDesc.Width, depthDesc.Height)) {
							aswDepthSrc = s_depthR32F;
						}
					}
				}
			}

			bool aswEyeCached = false;
			if (!aswDepthSrc) {
				g_aswProvider->InvalidateCachedFrame();
				static int s_aswNoDepthLog = 0;
				if (s_aswNoDepthLog++ < 5)
					OOVR_LOGF("ASW: skipping cache for eye %d because depth extraction is unavailable", eyeIdx);
			} else if (colorRegion.right <= colorRegion.left || colorRegion.bottom <= colorRegion.top) {
				g_aswProvider->InvalidateCachedFrame();
				static int s_aswBadColorRegionLog = 0;
				if (s_aswBadColorRegionLog++ < 5)
					OOVR_LOGF("ASW: skipping cache for eye %d because submitted texture bounds are invalid", eyeIdx);
			} else {
				// Hold ownership before the first COM method and through the queued
				// copy. AddRef after reading an unprotected pointer is already too late.
				DapaMaskBridge::Access<OCRenderTargetBridge> mask(
				    s_pBridge.Get(), DapaMaskBridge::AccessMode::Read);
				const bool samePair = eyeIdx == 0 ||
				    (s_dapaMaskCacheFrame == mask.Frame() &&
				        s_dapaMaskCacheConflicts == mask.ConflictSerial());
				if (mask.Clean() && samePair) {
					aswEyeCached = g_aswProvider->CacheFrame(eyeIdx, context,
					    colorSrc, &colorRegion,
					    colorFlipV,
					    mvTex, &mvRegion,
					    aswDepthSrc, &depthRegion,
					    layer.pose, layer.fov,
					    g_fsr3CameraNear, g_fsr3CameraFar,
					    reinterpret_cast<ID3D11Texture2D*>(mask.Texture()),
					    ocu_effect_foveation::GetState().ReadBlackout());
					// A missed owned draw while we held the lease poisons this pair.
					// Never turn a partially protected player into a synthetic frame.
					aswEyeCached = aswEyeCached && mask.Clean();
				}
				if (eyeIdx == 0) {
					s_dapaMaskCacheFrame = aswEyeCached ? mask.Frame() : 0;
					s_dapaMaskCacheConflicts = mask.ConflictSerial();
				}
				if (!aswEyeCached) {
					g_aswProvider->InvalidateCachedFrame();
					if (!DapaMaskBridge::Supported(s_pBridge.Get())) {
						OOVR_LOG_LIMITEDF(5000, "DAPA mask handoff unavailable: protocol=%u; matching runtime and SKSE plugin required",
						    unsigned(s_pBridge->_padPreFP[0]));
					} else {
						OOVR_LOG_LIMITEDF(5000, "DAPA mask handoff: skipped incomplete eye=%d frame=%u; retrying next real frame",
						    eyeIdx, mask.Frame());
					}
				}
			}

			// Only BeginVRSGameFrame recycles the producer mask under its lease.
			// Geometry is per eye; actor position is sampled once per complete real pair.
			// Do not mix NiCamera Z with actor XY: it includes tracked HMD movement.
			if (aswEyeCached && s_pBridge->rssBasePtr) {
				const auto* rss = reinterpret_cast<const uint8_t*>(
				    static_cast<uintptr_t>(s_pBridge->rssBasePtr));
				const float* view = reinterpret_cast<const float*>(rss + 0x3E0 + eyeIdx * 0x250 + 0x30);
				const float* vp = reinterpret_cast<const float*>(rss + 0x3E0 + eyeIdx * 0x250 + 0x130);
				g_aswProvider->SetMotionGeometry(eyeIdx, view, vp);
			}
			if (aswEyeCached && eyeIdx == 1 && g_aswProvider->HasCachedFrame()) {
				if (s_pBridge->actorPosPtr) {
					const auto* pos = reinterpret_cast<const float*>(
					    static_cast<uintptr_t>(s_pBridge->actorPosPtr));
					float turn = 0;
					if (xr_rightStickX_action != XR_NULL_HANDLE && xr_session.get() != XR_NULL_HANDLE) {
						XrActionStateGetInfo info = {XR_TYPE_ACTION_STATE_GET_INFO};
						info.action = xr_rightStickX_action;
						XrActionStateFloat state = {XR_TYPE_ACTION_STATE_FLOAT};
						if (XR_SUCCEEDED(xrGetActionStateFloat(xr_session.get(), &info, &state)) && state.isActive)
							turn = state.currentState;
					}
					const float yaw = s_pBridge->actorYawPtr ? *reinterpret_cast<const float*>(
					    static_cast<uintptr_t>(s_pBridge->actorYawPtr)) : 0.0f;
					g_aswProvider->SampleLocomotionYaw(s_pBridge->actorYawPtr ? xr_gbl->nextPredictedFrameTime : 0, yaw, std::abs(turn) > 0.05f);
					g_aswProvider->SampleLocomotion(xr_gbl->nextPredictedFrameTime, {pos[0], pos[1], pos[2]});
				} else {
					g_aswProvider->SampleLocomotionYaw(0, 0, false);
					g_aswProvider->SampleLocomotion(0, {});
				}
			}
		} else {
			g_aswProvider->InvalidateCachedFrame();
		}
	} else if (g_aswProvider && g_aswProvider->IsReady()
	    && g_aswProvider->IsInjectionWanted()) {
		// Any skipped eye makes the single-buffer stereo pair unusable.
		g_aswProvider->InvalidateCachedFrame();
	}

#if defined(OC_HAS_FSR3) || defined(OC_HAS_DLSS)
	// After both eyes: compute NEXT frame's jitter and increment frame counter.
	// This must happen AFTER both eyes have rendered and dispatched with the current jitter.
	// Previously this was in the left-eye block, which caused the right eye to pick up
	// the wrong (next-frame) jitter in GetProjectionRaw — producing temporal instability.
	if (g_fsr3JitterEnabled) {
		s_temporalJitterSubmittedEyeMask |= (uint8_t)(1u << s_currentEyeIdx);
		if ((s_temporalJitterSubmittedEyeMask & 0x3u) == 0x3u) {
			auto* src = (ID3D11Texture2D*)texture->handle;
			D3D11_TEXTURE2D_DESC srcDesc;
			src->GetDesc(&srcDesc);
			D3D11_BOX submittedRegion = {};
			if (ResolveSubmittedTextureRegion(srcDesc, ptrBounds, submittedRegion)) {
				uint32_t renderW = submittedRegion.right - submittedRegion.left;
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
			s_temporalJitterSubmittedEyeMask = 0;
		}
	}
#endif

	// Capture the actual submitted atlas and per-eye bounds. The next
	// WaitGetPoses call consumes this geometry and binds one correctly-sized VRS
	// resource before Skyrim draws either eye of the following frame.
	if (oovr_global_configuration.VrsAnyEnabled()) {
		int eyeIdx = (eye == XruEyeLeft) ? 0 : 1;

		// Extract this eye's optical center from its asymmetric OpenXR FOV.
		float tanL = tanf(layer.fov.angleLeft);
		float tanR = tanf(layer.fov.angleRight);
		float tanU = tanf(layer.fov.angleUp);
		float tanD = tanf(layer.fov.angleDown);
		s_vrsTanL[eyeIdx] = tanL;
		s_vrsTanR[eyeIdx] = tanR;
		s_vrsTanU[eyeIdx] = tanU;
		s_vrsTanD[eyeIdx] = tanD;
		s_vrsOpticalX[eyeIdx] = (-tanL) / (tanR - tanL);
		s_vrsOpticalY[eyeIdx] = tanU / (tanU - tanD);

		auto* vrsSource = (ID3D11Texture2D*)gameTexture->handle;
		D3D11_TEXTURE2D_DESC vrsSourceDesc{};
		vrsSource->GetDesc(&vrsSourceDesc);
		D3D11_BOX vrsSubmittedRegion{};
		if (ResolveSubmittedTextureRegion(vrsSourceDesc, ptrBounds, vrsSubmittedRegion)) {
			s_vrsRenderWidth[eyeIdx] = (int)vrsSourceDesc.Width;
			s_vrsRenderHeight[eyeIdx] = (int)vrsSourceDesc.Height;
			s_vrsRenderTexture[eyeIdx] = vrsSource;
			s_vrsEyeRegion[eyeIdx] = {
				(int)vrsSubmittedRegion.left,
				(int)vrsSubmittedRegion.top,
				(int)(vrsSubmittedRegion.right - vrsSubmittedRegion.left),
				(int)(vrsSubmittedRegion.bottom - vrsSubmittedRegion.top)
			};
			s_vrsGeometryMask |= (std::uint8_t)(1u << eyeIdx);
			static bool loggedVrsInput[2] = { false, false };
			if (!loggedVrsInput[eyeIdx]) {
				loggedVrsInput[eyeIdx] = true;
				const float uMin = ptrBounds ? ptrBounds->uMin : 0.0f;
				const float uMax = ptrBounds ? ptrBounds->uMax : 1.0f;
				const float vMin = ptrBounds ? ptrBounds->vMin : 0.0f;
				const float vMax = ptrBounds ? ptrBounds->vMax : 1.0f;
				OOVR_LOGF(
				    "VRS eye input %s: texture=%ux%u region=(%u,%u)-(%u,%u) bounds=(%.3f,%.3f,%.3f,%.3f) fovTan=(L%.4f,R%.4f,U%.4f,D%.4f) opticalUV=(%.4f,%.4f)",
				    eyeIdx == 0 ? "left" : "right", vrsSourceDesc.Width, vrsSourceDesc.Height,
				    vrsSubmittedRegion.left, vrsSubmittedRegion.top,
				    vrsSubmittedRegion.right, vrsSubmittedRegion.bottom,
				    uMin, uMax, vMin, vMax, tanL, tanR, tanU, tanD,
				    s_vrsOpticalX[eyeIdx], s_vrsOpticalY[eyeIdx]);
			}
		} else {
			s_vrsGeometryMask &= (std::uint8_t)~(1u << eyeIdx);
			s_vrsRenderTexture[eyeIdx] = nullptr;
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
