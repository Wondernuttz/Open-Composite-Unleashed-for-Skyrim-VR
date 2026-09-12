// CommonLibVR headers MUST come before Windows.h (REX/W32/BASE.h enforces this)
#include <RE/B/BSInputDeviceManager.h>
#include <RE/B/BSInputEventQueue.h>
#include <RE/B/BSOpenVR.h>
#include <RE/B/ButtonEvent.h>
#include <RE/B/BSVirtualKeyboardDevice.h>
#include <RE/B/BSWin32VirtualKeyboardDevice.h>
#include <RE/B/BSTEvent.h>
#include <RE/B/BookMenu.h>         // Physical book/note model + native menu state
#include <RE/C/ControlMap.h>
#include <RE/C/ConfirmAndNameCallback.h>
#include <RE/F/FxDelegateArgs.h>
#include "RaceMenuKeyboardRecovery.h"
#include "RaceMenuNativeInput.h"
#include <RE/G/GFxEvent.h>
#include <RE/G/GFxValue.h>
// [EXPERIMENTAL — DISABLED] These headers were used by the VR laser→Scaleform
// mouse injection system (WM_OC_LASER handler). That system is disabled because
// accessing the wrong menu's Scaleform movie (especially StatsMenu during
// Sovngarde's constellation scene) permanently corrupts VR rendering — the
// "Sovngarde bug". See detailed notes at the WM_OC_LASER comment block below.
// #include <RE/G/GFxMovieDef.h>    // Was: GetMovieDef() for stage dimensions
#include <RE/G/GFxMovieView.h>      // Still needed: HandleEvent() for WM_OC_CHAR keyboard injection
// #include <RE/G/GMatrix3D.h>      // Was: perspective3D matrix reading
namespace RE { class GASGlobalContext; } // Forward decl needed by GFxMovieRoot.h
// #include <RE/G/GFxMovieRoot.h>   // Was: movieRoot->perspective3D
#include <RE/I/IMenu.h>             // Still needed: MenuWatcher accesses IMenu for OC_MENU_ACTIVE
#include <RE/M/MenuCursor.h>        // Laser cursor pump v2: cursor feedback + visibility (game singleton, NOT Scaleform)
#include <RE/M/MenuOpenCloseEvent.h>
#include <RE/M/Misc.h>             // Once-per-session DAPA startup failure message
#include <RE/N/NiNode.h>            // Laser cursor pump v2: uiNode plane export
#include <RE/B/BSTriShape.h>        // Actual Scaleform render mesh + model bound
#include <RE/B/bhkPickData.h>       // Console ref pick: havok ray into the world
#include <RE/B/bhkWorld.h>          // Console ref pick: world + read lock
#include <RE/C/Console.h>           // Console ref pick: SetSelectedRef
#include <RE/C/ConsoleLog.h>        // Console ref pick: print name + FormID
#include <RE/C/CollisionLayers.h>   // Console ref pick: LOS collision layer
#include <RE/T/TESHavokUtilities.h> // Console ref pick: collidable -> TESObjectREFR
#include <RE/T/TESObjectCELL.h>     // Console ref pick: cell -> bhkWorld
#include <RE/T/ThumbstickEvent.h>
#include <RE/N/NiCamera.h>
#include <RE/N/NiRTTI.h>
#include <RE/P/PlayerCamera.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/B/BSRenderPass.h>
#include <RE/R/Renderer.h>
#include <RE/R/RaceSexMenu.h>
#include <RE/R/RaceSexMenu.h>
#include <RE/S/State.h>             // Map beam: engine-owned default white texture
// BSShaderAccumulator: use raw offsets to avoid header dependency issues.
// VTable REL::VariantID(304459, 254680, 0x18fd880)
// firstPerson bool at offset 0x128
// FinishAccumulating at vtable slot 0x26
#include <RE/A/ActorEquipManager.h> // gesture spell equip
#include <RE/B/BGSEquipSlot.h>      // gesture spell equip (hand slots)
#include <RE/I/InterfaceStrings.h>  // console show/hide via UI queue
#include <RE/M/MagicCaster.h>       // gesture instant spell cast
#include <RE/M/MagicSystem.h>
#include <RE/S/SpellItem.h>
#include <RE/T/TESDataHandler.h>    // spell list dump + form resolution
#include <RE/A/AIProcess.h>              // combat haptics: current attack data (hand)
#include <RE/B/BGSAttackData.h>          // combat haptics: IsLeftAttack
#include <RE/H/HighProcessData.h>        // combat haptics: attackData holder
#include <RE/S/ScriptEventSourceHolder.h> // combat haptics: TESHitEvent source
#include <RE/T/TESForm.h>                // combat haptics: LookupByID
#include <RE/T/TESHitEvent.h>            // combat haptics: hit event struct
#include <RE/T/TESObjectWEAP.h>          // combat haptics: weapon type checks
#include <RE/B/BSAnimationGraphEvent.h>  // combat haptics: arrow release
#include <RE/T/TESSpellCastEvent.h>      // combat haptics: spell release pulse
#include <RE/U/UI.h>
#include <RE/U/UIMessageQueue.h>    // console show/hide via UI queue
#include <RE/U/UserEvents.h>        // Native PrevPage / NextPage actions
#include <SKSE/SKSE.h>
#include <algorithm> // laser cursor pump: std::clamp
#include <array>
#include <chrono> // gesture concentration-spell burst pacing
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <cwchar>
#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <thread> // gesture concentration-spell burst
#include <utility>

#include <spdlog/sinks/basic_file_sink.h>
#include <set>
#include <string>
#include <vector>
#include <mutex>

// Win32 API for WndProc hook (REX::W32 doesn't provide CallWindowProcW)
#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include "../../../OpenCompositeSkyrimVR/DrvOpenXR/DapaPlayerMaskGpu.h"
#include "DapaVtableSlot.h"
#include "DapaEngineDraw.h"
#include "DapaAcceptedDraw.h"
#include "DapaCsxDraw.h"
#include "DapaCsxApi.h"
#include "DapaHiggs.h"
#include "DapaSpellWheel.h"
#include "DapaCrossbow.h"
#include "DapaGeometryOwnership.h"
#include <RE/B/BSFadeNode.h>
#include <atomic>
#pragma comment(lib, "d3dcompiler.lib")

// =========================================================================
// Shared memory struct — read by Open Composite for laser plane positioning
// =========================================================================
#pragma pack(push, 1)
struct OCMenuTransform {
	static constexpr uint32_t MAGIC = 0x54434D4F; // 'OCMT'
	static constexpr uint32_t VERSION = 6;

	uint32_t magic;           // Must be MAGIC
	uint32_t version;         // Protocol version
	uint32_t updateCounter;   // Odd = writing, even = stable (seqlock)

	// Menu identification
	bool     active;          // Is a tracked menu open?
	char     menuName[64];    // Name of the top active menu
	int8_t   depthPriority;   // IMenu::depthPriority

	// Scaleform stage dimensions
	float    stageWidth;
	float    stageHeight;

	// Scaleform perspective3D matrix (4x4, row-major)
	bool     hasPerspective;
	float    perspectiveMatrix[4][4];

	// ---- v2: laser cursor bridge (NO Scaleform access on either side) ----

	// UI plane ground truth (SKSE -> DLL). The game's own uiNode transform,
	// expressed RoomNode-local and converted to OpenXR floor space (Y-up,
	// meters). Quad convention matches VRMenuLaser: +X right, +Y up,
	// +Z = plane normal toward the viewer.
	uint8_t  uiPlaneValid;    // 1 = pose below is fresh
	float    uiPlanePos[3];   // plane center, meters
	float    uiPlaneQuat[4];  // orientation x,y,z,w
	float    uiPlaneWidth;    // meters
	float    uiPlaneHeight;   // meters

	// Cursor feedback (SKSE -> DLL): MenuCursor state for closed-loop drive
	float    cursorPosX;      // MenuCursor current position
	float    cursorPosY;
	float    cursorRangeX;    // MenuCursor screenWidthX/Y (cursor space bounds)
	float    cursorRangeY;

	// Laser command (DLL -> SKSE). u,v in [0,1], Scaleform convention:
	// (0,0) = top-left (VRMenuLaser GetHitV already returns top-down V).
	uint8_t  laserActive;     // 1 = laser is hitting the quad this frame
	float    laserU;
	float    laserV;
	uint32_t laserPressSeq;   // DLL increments on trigger press edge
	uint32_t laserReleaseSeq; // DLL increments on trigger release edge
	uint32_t laserFrameSeq;   // DLL increments once per submitted frame
	uint8_t  laserShowCursor; // 1 = show the 2D arrow (diagnostic only; default
	                          // 0 — the laser dot on the quad IS the pointer)

	// v3: Skyrim HMD pose in the exact same RoomNode-local metric frame as
	// uiPlanePos/uiPlaneQuat. The compositor uses this to map the menu plane
	// into the live OpenXR Stage origin instead of assuming both origins match.
	uint8_t  roomHmdValid;
	float    roomHmdPos[3];
	float    roomHmdQuat[4];

	// Reserved v6 ABI space. The removed MapMenu bridge used these fields;
	// retain their layout so mixed-version shared-memory readers fail safely.
	uint8_t  mapPointerValid;
	float    mapPointerDistanceMeters;
	float    mapPointerReserved[2];

	// v5: physical OpenXR hand that owns the published trigger edge.
	// 0 = left, 1 = right, 0xFF = unavailable/legacy runtime.
	uint8_t  laserHand;
	uint8_t  laserTriggerHeld;
};
#pragma pack(pop)
static_assert(sizeof(OCMenuTransform) == 270);

// Console world-space laser bridge. This protocol is deliberately separate
// from OCMenuTransform so the locked menu quad/cursor calibration cannot be
// affected by console picking. OC writes controller rays; this plugin writes
// Havok hit feedback and commits a reference only on that hand's trigger edge.
#pragma pack(push, 1)
struct OCConsoleLaserBridge {
	static constexpr uint32_t MAGIC = 0x524C434F; // 'OCLR'
	static constexpr uint32_t VERSION = 1;

	uint32_t magic;
	uint32_t version;
	uint32_t byteSize;

	uint32_t runtimeSequence;
	uint32_t frameSequence;
	uint32_t triggerPressSequence[2];
	uint8_t  rayValid[2];
	uint8_t  runtimePad[2];
	float    rayOriginFromHmd[2][3];
	float    rayDirection[2][3];

	uint32_t gameSequence;
	uint32_t hitFormId[2];
	uint8_t  hitValid[2];
	uint8_t  gamePad[2];
	float    hitDistanceMeters[2];
	uint32_t selectedFormId;
	uint8_t  reserved[12];
};
#pragma pack(pop)
static_assert(offsetof(OCConsoleLaserBridge, runtimeSequence) % 4 == 0);
static_assert(offsetof(OCConsoleLaserBridge, gameSequence) % 4 == 0);
static_assert(sizeof(OCConsoleLaserBridge) == 120);

// =========================================================================
// Shared memory struct — render target bridge for Open Composite FSR 2/3
// Exposes ID3D11Texture2D* pointers for motion vectors and depth buffer.
// Both DLLs live in the same process, so raw pointers are valid.
// =========================================================================
#pragma pack(push, 1)
struct OCRenderTargetBridge {
	static constexpr uint32_t MAGIC = 0x56544F4D; // 'MOTV'
	static constexpr uint32_t VERSION = 2;

	uint32_t magic;
	uint32_t version;
	uint32_t byteSize;
	uint32_t publishSequence; // Odd = resource writer active, even = stable
	uint32_t status;        // 0=not ready, 1=ready, 2=error
	uint32_t resourceReaders; // Pins the published COM pointers while a reader AddRefs
	uint64_t resourceGeneration;
	uint32_t mvWidth;
	uint32_t mvHeight;
	uint32_t depthWidth;
	uint32_t depthHeight;

	// Motion vector render target
	uint64_t mvTexture;     // ID3D11Texture2D*
	uint64_t mvSRV;         // ID3D11ShaderResourceView*
	uint64_t mvUAV;         // ID3D11UnorderedAccessView*

	// Depth buffer
	uint64_t depthTexture;  // ID3D11Texture2D*
	uint64_t depthSRV;      // ID3D11ShaderResourceView*

	// D3D11 device and context (for receiver validation)
	uint64_t d3dDevice;     // ID3D11Device*
	uint64_t d3dContext;    // ID3D11DeviceContext*

	// Camera data for locomotion-aware motion vectors (added v1.1)
	uint64_t worldToCamPtr;   // float* → NiCamera::worldToCam[0][0] (row-major 4x4, 64 bytes)
	uint64_t playerPosPtr;    // float* → PlayerCamera::pos.x (3 floats: x, y, z)
	uint64_t playerYawPtr;    // float* → PlayerCamera::yaw (1 float, radians)
	uint8_t  isMainMenu;       // 1 = main menu active, 0 = gameplay
	uint8_t  isLoadingScreen;  // 1 = loading screen active, 0 = gameplay
	uint8_t  _pad1[6];        // padding to align next uint64_t
	uint64_t viewFrustumPtr;  // float* → NiFrustum (7 members: L,R,T,B,Near,Far + bool ortho)

	// RendererShadowState base address — compositor reads VP matrices at known offsets
	uint64_t rssBasePtr;      // uintptr_t → BSGraphics::RendererShadowState singleton

	// Actor position for stick locomotion correction (moves only with stick, not head tracking)
	uint64_t actorPosPtr;     // float* → PlayerCharacter::data.location.x (NiPoint3: x, y, z)
	uint64_t actorYawPtr;     // float* → PlayerCharacter::data.angle.z (actor heading, radians)

	// Camera world position (NiCamera::world.translate) — includes actorPos + eye height +
	// walk-cycle camera bob + HMD tracking. Updated by the engine each frame during scene
	// graph update. Z delta captures ALL vertical camera motion for MV compensation.
	uint64_t cameraPosPtr;    // float* → NiCamera::world.translate.x (NiPoint3: x, y, z)

	// Actor MV data — pointers to NiAVObject root nodes for nearby actors.
	// OC reads world/previousWorld transforms directly via known offsets each frame.
	// NiAVObject offsets (VR, verified): world.translate=+0xA0, previousWorld.translate=+0xD4,
	// worldBound.center=+0xE4, worldBound.radius=+0xF0
	static constexpr uint32_t MAX_ACTOR_MV = 32;
	uint32_t actorMvCount;                          // Number of valid entries
	uint32_t actorMvRefreshSeq;                     // Incremented on re-enumeration (SKSE writes)
	uint64_t actorMvRootPtrs[MAX_ACTOR_MV];         // NiAVObject* root node pointers
	uint32_t actorMvRequestRefresh;                 // OC sets to 1 to request SKSE re-enumerate
	uint32_t _padActorMv;

	// Stencil capture — R24G8_TYPELESS copy captured mid-frame before the game clears stencil.
	uint64_t stencilCaptureTexture;  // ID3D11Texture2D* (R24G8_TYPELESS, same size as depth)
	uint8_t  stencilCapturedThisFrame; // 1 = valid capture for current frame
	uint8_t  _padStencil[7];

	// Player first-person model — NiAVObject nodes for hand bounding sphere detection.
	uint64_t playerFirstPersonRootPtr;  // NiAVObject* → player's 1st-person skeleton root
	uint64_t playerFPLeftHandPtr;       // NiAVObject* → left hand node
	uint64_t playerFPRightHandPtr;      // NiAVObject* → right hand node
	uint64_t playerFPWeaponPtr;         // NiAVObject* → weapon node

	// First-person render pass detection.
	uint8_t fpRenderFinished; // set to 1 by hook when FP render done
	uint8_t _padFPRender[7];
	uint64_t finishAccumulatingAddr; // address of BSShaderAccumulator::FinishAccumulating (for OC to hook via MinHook)

	// Per-draw-call stencil injection — marks FP pixels with stencil=2
	uint8_t  fpStencilInjectionActive; // 1 = SetupGeometry hook is running, stencil=2 marks FP pixels
	uint8_t  fpStencilInjectNow;       // 1 = currently inside FP draw (set by SKSE, read by OC MinHook)
	uint8_t  _padFPStencil[6];
	uint32_t fpStencilDrawCount;       // Number of FP draw calls this frame (diagnostic)
	uint32_t fpStencilDrawCountTotal;  // Cumulative FP draws (diagnostic)

	// Pre-FP depth snapshot — depth buffer state before first-person geometry renders.
	// OC compares this against post-FP depth: where depth got closer → FP pixel.
	uint64_t preFPDepthTexture;        // ID3D11Texture2D* (R24G8_TYPELESS, same size as main DS)
	uint8_t  preFPDepthCaptured;       // 1 = valid capture for current frame
	uint8_t  _padPreFP[7];

	// FP geometry pointers — BSGeometry* addresses for positively-identified FP draws.
	// OC reads their worldBound LIVE at WarpFrame time (no stale data).
	uint64_t fpGeomPointers[16];       // Up to 16 BSGeometry* pointers
	uint32_t fpGeomCount;              // Number of valid pointers
	uint32_t _padGeom[3];              // Alignment to 16 bytes

	// FP draw replay — pointer to heap-allocated FPReplayData (same process, read by OC).
	// Double-buffered: SKSE writes captures, OC reads at warp time.
	uint64_t fpReplayDataPtr;          // FPReplayData* (cast to uint64_t)

	// Menu state — set by MenuWatcher when any gameplay menu is open.
	// ASW uses this to skip MV corrections (reprojection-only) during menus,
	// preventing UI element duplication on warp frames.
	uint8_t  isMenuOpen;               // 1 = a gameplay menu is open, 0 = gameplay
	uint8_t  isConsoleOpen;            // 1 = the game console menu is open (VR keyboard overlay sync)
	uint8_t  _padMenu[6];              // alignment
};
#pragma pack(pop)
static_assert(sizeof(OCRenderTargetBridge) == 704);
static_assert(offsetof(OCRenderTargetBridge, publishSequence) % alignof(uint32_t) == 0);
static_assert(offsetof(OCRenderTargetBridge, resourceReaders) % alignof(uint32_t) == 0);

namespace
{
	std::atomic<bool> g_diagnosticLogging{false};
	void SyncDiagnosticLogging()
	{
		using Query = int (*)();
		auto module = GetModuleHandleW(L"openvr_api.dll");
		if (!module) module = GetModuleHandleW(L"vrclient_x64.dll");
		auto query = module ? reinterpret_cast<Query>(GetProcAddress(module, "OCU_DebugLoggingEnabled")) : nullptr;
		const bool enabled = query && query() != 0;
		g_diagnosticLogging.store(enabled, std::memory_order_relaxed);
		spdlog::set_level(enabled ? spdlog::level::debug : spdlog::level::info);
	}
	// =========================================================================
	// WndProc hook globals
	// =========================================================================
	WNDPROC g_originalWndProc = nullptr;
	bool    g_hooked = false;

	// =========================================================================
	// Shared memory for menu transform (read by Open Composite)
	// =========================================================================
	HANDLE           g_hMapFile = nullptr;
	OCMenuTransform* g_pTransform = nullptr;
	HANDLE                  g_hConsoleLaserMap = nullptr;
	OCConsoleLaserBridge*   g_pConsoleLaser = nullptr;

	// =========================================================================
	// Shared memory for render target bridge (read by Open Composite for FSR)
	// =========================================================================
	HANDLE                g_hBridgeMapFile = nullptr;
	OCRenderTargetBridge* g_pBridge = nullptr;

	template <class T>
	T* RetainBridgeResource(T* a_resource)
	{
		if (a_resource)
			a_resource->AddRef();
		return a_resource;
	}

	struct BridgeResourceRefs
	{
		ID3D11Texture2D* mvTexture = nullptr;
		ID3D11ShaderResourceView* mvSRV = nullptr;
		ID3D11UnorderedAccessView* mvUAV = nullptr;
		ID3D11Texture2D* depthTexture = nullptr;
		ID3D11ShaderResourceView* depthSRV = nullptr;
		ID3D11Device* d3dDevice = nullptr;
		ID3D11DeviceContext* d3dContext = nullptr;
		uint32_t mvWidth = 0;
		uint32_t mvHeight = 0;
		uint32_t depthWidth = 0;
		uint32_t depthHeight = 0;

		BridgeResourceRefs() = default;
		BridgeResourceRefs(const BridgeResourceRefs&) = delete;
		BridgeResourceRefs& operator=(const BridgeResourceRefs&) = delete;

		BridgeResourceRefs(BridgeResourceRefs&& a_other) noexcept
		{
			Swap(a_other);
		}

		BridgeResourceRefs& operator=(BridgeResourceRefs&& a_other) noexcept
		{
			if (this != &a_other) {
				Reset();
				Swap(a_other);
			}
			return *this;
		}

		~BridgeResourceRefs()
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

			mvTexture = nullptr;
			mvSRV = nullptr;
			mvUAV = nullptr;
			depthTexture = nullptr;
			depthSRV = nullptr;
			d3dDevice = nullptr;
			d3dContext = nullptr;
			mvWidth = 0;
			mvHeight = 0;
			depthWidth = 0;
			depthHeight = 0;
		}

		void Swap(BridgeResourceRefs& a_other) noexcept
		{
			std::swap(mvTexture, a_other.mvTexture);
			std::swap(mvSRV, a_other.mvSRV);
			std::swap(mvUAV, a_other.mvUAV);
			std::swap(depthTexture, a_other.depthTexture);
			std::swap(depthSRV, a_other.depthSRV);
			std::swap(d3dDevice, a_other.d3dDevice);
			std::swap(d3dContext, a_other.d3dContext);
			std::swap(mvWidth, a_other.mvWidth);
			std::swap(mvHeight, a_other.mvHeight);
			std::swap(depthWidth, a_other.depthWidth);
			std::swap(depthHeight, a_other.depthHeight);
		}

		BridgeResourceRefs RetainCopy() const
		{
			BridgeResourceRefs copy;
			copy.mvTexture = RetainBridgeResource(mvTexture);
			copy.mvSRV = RetainBridgeResource(mvSRV);
			copy.mvUAV = RetainBridgeResource(mvUAV);
			copy.depthTexture = RetainBridgeResource(depthTexture);
			copy.depthSRV = RetainBridgeResource(depthSRV);
			copy.d3dDevice = RetainBridgeResource(d3dDevice);
			copy.d3dContext = RetainBridgeResource(d3dContext);
			copy.mvWidth = mvWidth;
			copy.mvHeight = mvHeight;
			copy.depthWidth = depthWidth;
			copy.depthHeight = depthHeight;
			return copy;
		}

		bool SameResources(const BridgeResourceRefs& a_other) const
		{
			return mvTexture == a_other.mvTexture &&
			       mvSRV == a_other.mvSRV &&
			       mvUAV == a_other.mvUAV &&
			       depthTexture == a_other.depthTexture &&
			       depthSRV == a_other.depthSRV &&
			       d3dDevice == a_other.d3dDevice &&
			       d3dContext == a_other.d3dContext &&
			       mvWidth == a_other.mvWidth &&
			       mvHeight == a_other.mvHeight &&
			       depthWidth == a_other.depthWidth &&
			       depthHeight == a_other.depthHeight;
		}
	};

	std::mutex g_bridgeResourceMutex;
	BridgeResourceRefs g_bridgeResources;
	uint64_t g_bridgeResourceGeneration = 0;

	uint32_t ReadBridgeAtomic(const uint32_t& a_value)
	{
		auto* value = reinterpret_cast<volatile LONG*>(const_cast<uint32_t*>(&a_value));
		return static_cast<uint32_t>(InterlockedCompareExchange(value, 0, 0));
	}

	void BeginBridgeResourcePublish()
	{
		auto* sequence = reinterpret_cast<volatile LONG*>(&g_pBridge->publishSequence);
		LONG writingSequence = InterlockedIncrement(sequence);
		if ((writingSequence & 1) == 0)
			InterlockedIncrement(sequence);

		// Readers increment before copying and AddRefing. Once the sequence is odd,
		// no new reader can enter, so zero means the previous COM set can be retired.
		while (ReadBridgeAtomic(g_pBridge->resourceReaders) != 0)
			SwitchToThread();
		MemoryBarrier();
	}

	void EndBridgeResourcePublish()
	{
		MemoryBarrier();
		InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_pBridge->publishSequence));
	}

	bool PublishBridgeResources(BridgeResourceRefs&& a_resources, uint32_t a_status)
	{
		if (!g_pBridge)
			return false;

		std::lock_guard<std::mutex> lock(g_bridgeResourceMutex);
		if (g_pBridge->status == a_status && g_bridgeResources.SameResources(a_resources))
			return false;

		BridgeResourceRefs previousResources = std::move(g_bridgeResources);
		g_bridgeResources = std::move(a_resources);

		BeginBridgeResourcePublish();
		g_pBridge->status = a_status;
		g_pBridge->resourceGeneration = ++g_bridgeResourceGeneration;
		g_pBridge->mvWidth = g_bridgeResources.mvWidth;
		g_pBridge->mvHeight = g_bridgeResources.mvHeight;
		g_pBridge->depthWidth = g_bridgeResources.depthWidth;
		g_pBridge->depthHeight = g_bridgeResources.depthHeight;
		g_pBridge->mvTexture = reinterpret_cast<uint64_t>(g_bridgeResources.mvTexture);
		g_pBridge->mvSRV = reinterpret_cast<uint64_t>(g_bridgeResources.mvSRV);
		g_pBridge->mvUAV = reinterpret_cast<uint64_t>(g_bridgeResources.mvUAV);
		g_pBridge->depthTexture = reinterpret_cast<uint64_t>(g_bridgeResources.depthTexture);
		g_pBridge->depthSRV = reinterpret_cast<uint64_t>(g_bridgeResources.depthSRV);
		g_pBridge->d3dDevice = reinterpret_cast<uint64_t>(g_bridgeResources.d3dDevice);
		g_pBridge->d3dContext = reinterpret_cast<uint64_t>(g_bridgeResources.d3dContext);
		EndBridgeResourcePublish();

		// previousResources releases only after the v2 snapshot is stable and all
		// readers that could have seen the old raw addresses hold their own COM refs.
		return true;
	}

	BridgeResourceRefs AcquirePublishedBridgeResources()
	{
		std::lock_guard<std::mutex> lock(g_bridgeResourceMutex);
		return g_bridgeResources.RetainCopy();
	}

	bool CaptureBridgeResources(BridgeResourceRefs& o_resources)
	{
		auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
		if (!renderer)
			return false;

		auto& runtimeData = renderer->GetRuntimeData();
		auto& mvRT = runtimeData.renderTargets[RE::RENDER_TARGET::kMOTION_VECTOR];
		auto* d3dDevice = reinterpret_cast<ID3D11Device*>(runtimeData.forwarder);
		auto* d3dContext = reinterpret_cast<ID3D11DeviceContext*>(runtimeData.context);
		if (!mvRT.texture || !d3dDevice || !d3dContext)
			return false;

		BridgeResourceRefs resources;
		resources.mvTexture = RetainBridgeResource(mvRT.texture);
		resources.mvSRV = RetainBridgeResource(mvRT.SRV);
		resources.mvUAV = RetainBridgeResource(mvRT.UAV);
		resources.d3dDevice = RetainBridgeResource(d3dDevice);
		resources.d3dContext = RetainBridgeResource(d3dContext);

		D3D11_TEXTURE2D_DESC mvDesc{};
		resources.mvTexture->GetDesc(&mvDesc);
		resources.mvWidth = mvDesc.Width;
		resources.mvHeight = mvDesc.Height;

		auto& depthData = renderer->GetDepthStencilData();
		auto& mainDepth = depthData.depthStencils[RE::RENDER_TARGET_DEPTHSTENCIL::kMAIN];
		if (mainDepth.texture) {
			resources.depthTexture = RetainBridgeResource(mainDepth.texture);
			resources.depthSRV = RetainBridgeResource(mainDepth.depthSRV);

			D3D11_TEXTURE2D_DESC depthDesc{};
			resources.depthTexture->GetDesc(&depthDesc);
			resources.depthWidth = depthDesc.Width;
			resources.depthHeight = depthDesc.Height;
		}

		o_resources = std::move(resources);
		return true;
	}

	void RefreshBridgeRenderTargets();

	void CreateSharedMemory()
	{
		g_hMapFile = CreateFileMappingW(
			INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
			0, sizeof(OCMenuTransform), L"Local\\OpenCompositeMenuTransform");

		if (!g_hMapFile) {
			SKSE::log::error("Failed to create shared memory (error: {})", GetLastError());
			return;
		}

		g_pTransform = static_cast<OCMenuTransform*>(
			MapViewOfFile(g_hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(OCMenuTransform)));

		if (!g_pTransform) {
			SKSE::log::error("Failed to map shared memory (error: {})", GetLastError());
			CloseHandle(g_hMapFile);
			g_hMapFile = nullptr;
			return;
		}

		// Initialize
		memset(g_pTransform, 0, sizeof(OCMenuTransform));
		g_pTransform->magic = OCMenuTransform::MAGIC;
		g_pTransform->version = OCMenuTransform::VERSION;
		g_pTransform->updateCounter = 0;
		g_pTransform->laserHand = 0xFF;
		SKSE::log::info("Shared memory created: Local\\OpenCompositeMenuTransform ({} bytes)", sizeof(OCMenuTransform));
	}

	void CreateConsoleLaserBridge()
	{
		g_hConsoleLaserMap = CreateFileMappingW(
			INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
			0, sizeof(OCConsoleLaserBridge),
			L"Local\\OpenCompositeConsoleLaser");
		if (!g_hConsoleLaserMap) {
			SKSE::log::error("Console laser bridge: CreateFileMapping failed ({})", GetLastError());
			return;
		}

		g_pConsoleLaser = static_cast<OCConsoleLaserBridge*>(
			MapViewOfFile(g_hConsoleLaserMap, FILE_MAP_ALL_ACCESS, 0, 0,
				sizeof(OCConsoleLaserBridge)));
		if (!g_pConsoleLaser) {
			SKSE::log::error("Console laser bridge: MapViewOfFile failed ({})", GetLastError());
			CloseHandle(g_hConsoleLaserMap);
			g_hConsoleLaserMap = nullptr;
			return;
		}

		memset(g_pConsoleLaser, 0, sizeof(OCConsoleLaserBridge));
		g_pConsoleLaser->magic = OCConsoleLaserBridge::MAGIC;
		g_pConsoleLaser->version = OCConsoleLaserBridge::VERSION;
		g_pConsoleLaser->byteSize = sizeof(OCConsoleLaserBridge);
		SKSE::log::debug("Console laser bridge created: Local\\OpenCompositeConsoleLaser ({} bytes)",
			sizeof(OCConsoleLaserBridge));
	}

	void CreateRenderTargetBridge()
	{
		// Create shared memory section
		g_hBridgeMapFile = CreateFileMappingW(
			INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
			0, sizeof(OCRenderTargetBridge), L"Local\\OpenCompositeRenderTargets");

		if (!g_hBridgeMapFile) {
			SKSE::log::error("RT Bridge: Failed to create shared memory (error: {})", GetLastError());
			return;
		}

		g_pBridge = static_cast<OCRenderTargetBridge*>(
			MapViewOfFile(g_hBridgeMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(OCRenderTargetBridge)));

		if (!g_pBridge) {
			SKSE::log::error("RT Bridge: Failed to map shared memory (error: {})", GetLastError());
			CloseHandle(g_hBridgeMapFile);
			g_hBridgeMapFile = nullptr;
			return;
		}

		memset(g_pBridge, 0, sizeof(OCRenderTargetBridge));
		g_pBridge->magic = OCRenderTargetBridge::MAGIC;
		g_pBridge->version = OCRenderTargetBridge::VERSION;
		g_pBridge->byteSize = sizeof(OCRenderTargetBridge);
		g_pBridge->status = 0; // Not ready yet

		RefreshBridgeRenderTargets();
		SKSE::log::info("RT Bridge v2: shared memory Local\\OpenCompositeRenderTargets ({} bytes)",
			sizeof(OCRenderTargetBridge));
	}

	// Re-capture the game's MV + depth render target pointers. Render-scale
	// mods (e.g. Community Shaders VR) destroy and recreate the game's render
	// targets mid-session ("relatch"); without this refresh the bridge keeps
	// serving freed texture pointers to the compositor (garbage ASW warps,
	// flashing, potential use-after-free). Called on the game thread ~1/sec.
	void RefreshBridgeRenderTargets()
	{
		if (!g_pBridge)
			return;

		BridgeResourceRefs resources;
		if (!CaptureBridgeResources(resources)) {
			BridgeResourceRefs unavailable;
			if (PublishBridgeResources(std::move(unavailable), 2)) {
				SKSE::log::warn("RT Bridge v2: resources unavailable; publication paused and refresh will retry");
			}
			return;
		}

		auto* mvTexture = resources.mvTexture;
		auto* depthTexture = resources.depthTexture;
		const uint32_t mvWidth = resources.mvWidth;
		const uint32_t mvHeight = resources.mvHeight;
		const uint32_t depthWidth = resources.depthWidth;
		const uint32_t depthHeight = resources.depthHeight;
		if (PublishBridgeResources(std::move(resources), 1)) {
			SKSE::log::info(
			    "RT Bridge v2: generation {} ready — MV={:p} {}x{}, depth={:p} {}x{}",
			    g_pBridge->resourceGeneration,
			    static_cast<void*>(mvTexture),
			    mvWidth,
			    mvHeight,
			    static_cast<void*>(depthTexture),
			    depthWidth,
			    depthHeight);
		}
	}

	// =========================================================================
	// RendererShadowState diagnostic — verify game VP matrices are accessible
	// =========================================================================
	// We avoid including RendererShadowState.h due to transitive TESFile.h
	// MAX_PATH macro conflict with Windows.h. Use REL::ID + raw offsets instead.
	// All offsets verified by static_assert in RendererShadowState.h (VR layout):
	//   posAdjust:     0x3A4  (EYE_POSITION<NiPoint3, 2>)
	//   prevPosAdjust: 0x3BC
	//   cameraData:    0x3E0  (EYE_POSITION<ViewData, 2>, each ViewData = 0x250)
	//     ViewData.viewMat:                          +0x30  (pure view 4x4)
	//     ViewData.projMatrixUnjittered:              +0x1B0
	//     ViewData.viewProjMatrixUnjittered:          +0x130
	//     ViewData.previousViewProjMatrixUnjittered:  +0x170
	void TestRendererShadowState()
	{
		// RELOCATION_ID(524773, 388819) = address of RendererShadowState singleton
		uintptr_t rssAddr = RELOCATION_ID(524773, 388819).address();
		if (!rssAddr) {
			SKSE::log::error("RSS Test: RELOCATION_ID returned 0!");
			return;
		}
		SKSE::log::info("RSS Test: Singleton at {:p}", (void*)rssAddr);

		uint8_t* base = reinterpret_cast<uint8_t*>(rssAddr);

		// posAdjust: offset 0x3A4, EYE_POSITION<NiPoint3, 2> = 2 × 12 bytes
		float* posAdj = reinterpret_cast<float*>(base + 0x3A4);
		SKSE::log::info("RSS Test: posAdjust L=({:.1f}, {:.1f}, {:.1f}) R=({:.1f}, {:.1f}, {:.1f})",
			posAdj[0], posAdj[1], posAdj[2], posAdj[3], posAdj[4], posAdj[5]);

		// previousPosAdjust: offset 0x3BC
		float* prevPosAdj = reinterpret_cast<float*>(base + 0x3BC);
		SKSE::log::info("RSS Test: prevPosAdjust L=({:.1f}, {:.1f}, {:.1f}) R=({:.1f}, {:.1f}, {:.1f})",
			prevPosAdj[0], prevPosAdj[1], prevPosAdj[2], prevPosAdj[3], prevPosAdj[4], prevPosAdj[5]);

		// Left eye ViewData starts at offset 0x3E0 (each ViewData is 0x250)
		// viewMat (pure view, no projection): ViewData + 0x30
		float* viewMatL = reinterpret_cast<float*>(base + 0x3E0 + 0x30);
		SKSE::log::info("RSS Test: Left viewMat (pure view, 4x4):");
		SKSE::log::info("  [{:.6f} {:.6f} {:.6f} {:.6f}]", viewMatL[0], viewMatL[1], viewMatL[2], viewMatL[3]);
		SKSE::log::info("  [{:.6f} {:.6f} {:.6f} {:.6f}]", viewMatL[4], viewMatL[5], viewMatL[6], viewMatL[7]);
		SKSE::log::info("  [{:.6f} {:.6f} {:.6f} {:.6f}]", viewMatL[8], viewMatL[9], viewMatL[10], viewMatL[11]);
		SKSE::log::info("  [{:.6f} {:.6f} {:.6f} {:.6f}]", viewMatL[12], viewMatL[13], viewMatL[14], viewMatL[15]);

		// projMatrixUnjittered: ViewData + 0x1B0
		float* projUnjitL = reinterpret_cast<float*>(base + 0x3E0 + 0x1B0);
		SKSE::log::info("RSS Test: Left projMatrixUnjittered:");
		SKSE::log::info("  [{:.6f} {:.6f} {:.6f} {:.6f}]", projUnjitL[0], projUnjitL[1], projUnjitL[2], projUnjitL[3]);
		SKSE::log::info("  [{:.6f} {:.6f} {:.6f} {:.6f}]", projUnjitL[4], projUnjitL[5], projUnjitL[6], projUnjitL[7]);
		SKSE::log::info("  [{:.6f} {:.6f} {:.6f} {:.6f}]", projUnjitL[8], projUnjitL[9], projUnjitL[10], projUnjitL[11]);
		SKSE::log::info("  [{:.6f} {:.6f} {:.6f} {:.6f}]", projUnjitL[12], projUnjitL[13], projUnjitL[14], projUnjitL[15]);

		// viewProjMatrixUnjittered: ViewData + 0x130
		float* vpUnjitL = reinterpret_cast<float*>(base + 0x3E0 + 0x130);
		SKSE::log::info("RSS Test: Left viewProjMatrixUnjittered:");
		SKSE::log::info("  [{:.6f} {:.6f} {:.6f} {:.6f}]", vpUnjitL[0], vpUnjitL[1], vpUnjitL[2], vpUnjitL[3]);
		SKSE::log::info("  [{:.6f} {:.6f} {:.6f} {:.6f}]", vpUnjitL[4], vpUnjitL[5], vpUnjitL[6], vpUnjitL[7]);
		SKSE::log::info("  [{:.6f} {:.6f} {:.6f} {:.6f}]", vpUnjitL[8], vpUnjitL[9], vpUnjitL[10], vpUnjitL[11]);
		SKSE::log::info("  [{:.6f} {:.6f} {:.6f} {:.6f}]", vpUnjitL[12], vpUnjitL[13], vpUnjitL[14], vpUnjitL[15]);

		// previousViewProjMatrixUnjittered: ViewData + 0x170
		float* prevVPL = reinterpret_cast<float*>(base + 0x3E0 + 0x170);
		SKSE::log::info("RSS Test: Left previousViewProjMatrixUnjittered:");
		SKSE::log::info("  [{:.6f} {:.6f} {:.6f} {:.6f}]", prevVPL[0], prevVPL[1], prevVPL[2], prevVPL[3]);
		SKSE::log::info("  [{:.6f} {:.6f} {:.6f} {:.6f}]", prevVPL[4], prevVPL[5], prevVPL[6], prevVPL[7]);
		SKSE::log::info("  [{:.6f} {:.6f} {:.6f} {:.6f}]", prevVPL[8], prevVPL[9], prevVPL[10], prevVPL[11]);
		SKSE::log::info("  [{:.6f} {:.6f} {:.6f} {:.6f}]", prevVPL[12], prevVPL[13], prevVPL[14], prevVPL[15]);
	}

	// =========================================================================
	// NiCamera lookup — deferred until scene graph is available
	// =========================================================================
	bool g_niCameraFound = false;

	void FindAndStoreNiCamera()
	{
		if (g_niCameraFound || !g_pBridge) return;

		auto* playerCamera = RE::PlayerCamera::GetSingleton();
		if (!playerCamera) {
			SKSE::log::warn("RT Bridge: PlayerCamera singleton not available yet");
			return;
		}

		// Store PlayerCamera pos/yaw pointers (always available once singleton exists)
		g_pBridge->playerPosPtr = reinterpret_cast<uint64_t>(&playerCamera->pos.x);
		g_pBridge->playerYawPtr = reinterpret_cast<uint64_t>(&playerCamera->yaw);
		SKSE::log::info("RT Bridge: PlayerCamera pos={:p} yaw={:p}",
			reinterpret_cast<void*>(g_pBridge->playerPosPtr),
			reinterpret_cast<void*>(g_pBridge->playerYawPtr));

		// Store PlayerCharacter actor position pointer for stick locomotion correction.
		// Unlike PlayerCamera::pos (which includes head tracking), actor position moves
		// only with stick locomotion and game physics — clean signal for ASW.
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (player) {
			g_pBridge->actorPosPtr = reinterpret_cast<uint64_t>(&player->data.location.x);
			g_pBridge->actorYawPtr = reinterpret_cast<uint64_t>(&player->data.angle.z);
			SKSE::log::info("RT Bridge: PlayerCharacter actorPos={:p} actorYaw={:p} ({:.1f}, {:.1f}, {:.1f}) yaw={:.4f}",
				reinterpret_cast<void*>(g_pBridge->actorPosPtr),
				reinterpret_cast<void*>(g_pBridge->actorYawPtr),
				player->data.location.x, player->data.location.y, player->data.location.z,
				player->data.angle.z);
		// First-person node tree — walk to find hand/weapon nodes with tighter bounds.
			auto* fpRoot = player->Get3D(true);
			if (fpRoot) {
				g_pBridge->playerFirstPersonRootPtr = reinterpret_cast<uint64_t>(fpRoot);
				auto& wb = fpRoot->worldBound;
				SKSE::log::info("RT Bridge: 1st-person root={:p} worldBound=({:.1f},{:.1f},{:.1f}) r={:.1f}",
				    static_cast<void*>(fpRoot), wb.center.x, wb.center.y, wb.center.z, wb.radius);

				// Recursive walk: find hand/weapon nodes and log all children for discovery
				std::function<void(RE::NiAVObject*, int)> walkNodes = [&](RE::NiAVObject* node, int depth) {
					if (!node || depth > 4) return;
					auto name = node->name;
					auto& b = node->worldBound;
					std::string indent(depth * 2, ' ');
					if (depth <= 2 || (name.data() && (
					    strstr(name.data(), "Hand") || strstr(name.data(), "hand") ||
					    strstr(name.data(), "Weapon") || strstr(name.data(), "weapon") ||
					    strstr(name.data(), "Wand") || strstr(name.data(), "Shield") ||
					    strstr(name.data(), "Arm") || strstr(name.data(), "arm")))) {
						SKSE::log::info("FP Node: {}{} bound=({:.1f},{:.1f},{:.1f}) r={:.1f}",
						    indent, name.data() ? name.data() : "(null)",
						    b.center.x, b.center.y, b.center.z, b.radius);
					}
					// Use "Hands [Ovl0]" — gender-neutral, same bounds as HandMaleVR/HandFemaleVR.
					if (name.data()) {
						if (strstr(name.data(), "Hands [Ovl0]") && !g_pBridge->playerFPLeftHandPtr)
							g_pBridge->playerFPLeftHandPtr = reinterpret_cast<uint64_t>(node);
						if (strstr(name.data(), "Weapon") || strstr(name.data(), "WEAPON"))
							g_pBridge->playerFPWeaponPtr = reinterpret_cast<uint64_t>(node);
					}
					// Recurse into NiNode children
					auto* ninode = node->AsNode();
					if (ninode) {
						for (auto& child : ninode->children) {
							if (child.get()) walkNodes(child.get(), depth + 1);
						}
					}
				};
				walkNodes(fpRoot, 0);
				SKSE::log::info("RT Bridge: FP nodes — LHand={:p} RHand={:p} Weapon={:p}",
				    reinterpret_cast<void*>(static_cast<uintptr_t>(g_pBridge->playerFPLeftHandPtr)),
				    reinterpret_cast<void*>(static_cast<uintptr_t>(g_pBridge->playerFPRightHandPtr)),
				    reinterpret_cast<void*>(static_cast<uintptr_t>(g_pBridge->playerFPWeaponPtr)));
			} else {
				SKSE::log::warn("RT Bridge: Get3D(true) returned null — 1st person node not loaded yet");
			}
		} else {
			SKSE::log::warn("RT Bridge: PlayerCharacter singleton not available yet");
		}

		// Find NiCamera in scene graph via cameraRoot
		auto* cameraRoot = playerCamera->cameraRoot.get();
		if (!cameraRoot) {
			SKSE::log::warn("RT Bridge: cameraRoot is null (scene not loaded yet, will retry)");
			return;
		}

		for (uint32_t i = 0; i < cameraRoot->children.size(); i++) {
			auto* child = cameraRoot->children[i].get();
			if (!child) continue;
			auto* rtti = child->GetRTTI();
			if (rtti && std::string_view(rtti->name) == "NiCamera") {
				auto* niCam = static_cast<RE::NiCamera*>(child);
				auto& rtData = niCam->GetRuntimeData();
				g_pBridge->worldToCamPtr = reinterpret_cast<uint64_t>(&rtData.worldToCam[0][0]);
				g_pBridge->cameraPosPtr = reinterpret_cast<uint64_t>(&niCam->world.translate.x);
				// Store viewFrustum pointer (inline frustum from RUNTIME_DATA2)
				auto& rtData2 = niCam->GetRuntimeData2();
				g_pBridge->viewFrustumPtr = reinterpret_cast<uint64_t>(&rtData2.viewFrustum);
				SKSE::log::info("RT Bridge: NiCamera found at {:p}, worldToCam at {:p}, cameraPos at {:p}, viewFrustum at {:p}",
					reinterpret_cast<void*>(niCam),
					reinterpret_cast<void*>(g_pBridge->worldToCamPtr),
					reinterpret_cast<void*>(g_pBridge->cameraPosPtr),
					reinterpret_cast<void*>(g_pBridge->viewFrustumPtr));

				// Log NiCamera world transform (pure rotation + position, no projection)
				auto& w = niCam->world;
				SKSE::log::info("RT Bridge: NiCamera world.translate=({:.2f}, {:.2f}, {:.2f}) scale={:.4f}",
					w.translate.x, w.translate.y, w.translate.z, w.scale);
				SKSE::log::info("RT Bridge: NiCamera world.rotate row0=({:.6f}, {:.6f}, {:.6f})",
					w.rotate.entry[0][0], w.rotate.entry[0][1], w.rotate.entry[0][2]);
				SKSE::log::info("RT Bridge: NiCamera world.rotate row1=({:.6f}, {:.6f}, {:.6f})",
					w.rotate.entry[1][0], w.rotate.entry[1][1], w.rotate.entry[1][2]);
				SKSE::log::info("RT Bridge: NiCamera world.rotate row2=({:.6f}, {:.6f}, {:.6f})",
					w.rotate.entry[2][0], w.rotate.entry[2][1], w.rotate.entry[2][2]);

				auto& pw = niCam->previousWorld;
				SKSE::log::info("RT Bridge: NiCamera previousWorld.translate=({:.2f}, {:.2f}, {:.2f})",
					pw.translate.x, pw.translate.y, pw.translate.z);

				g_niCameraFound = true;

				// Store RendererShadowState base address for compositor to read VP matrices
				{
					uintptr_t rssAddr = RELOCATION_ID(524773, 388819).address();
					if (rssAddr) {
						g_pBridge->rssBasePtr = static_cast<uint64_t>(rssAddr);
						SKSE::log::info("RT Bridge: RSS base at {:p}", (void*)rssAddr);
					} else {
						SKSE::log::warn("RT Bridge: RSS RELOCATION_ID returned 0");
					}
				}

				// Run RSS diagnostic now that we know the scene is loaded
				TestRendererShadowState();
				return;
			}
		}

		SKSE::log::warn("RT Bridge: NiCamera not found in cameraRoot ({} children, will retry)",
			cameraRoot->children.size());
	}

	// Forward declaration — defined after g_activeTrackedMenus
	void UpdateMenuTransform();

	// =========================================================================
	// Virtual keyboard bridge globals
	// =========================================================================

	// Custom Windows messages shared between Open Composite and this plugin
	constexpr UINT WM_OC_KEYBOARD = WM_APP + 0x4F43;
	constexpr UINT WM_OC_LASER = WM_APP + 0x4F44;
	constexpr UINT WM_OC_CHAR = WM_APP + 0x4F45;
	constexpr UINT WM_OC_BUTTON = WM_APP + 0x4F46;

	// GFxCharEvent — not defined in CommonLibVR but the kCharEvent enum value
	// exists.  Layout matches Scaleform GFx 4.x: EventType + wcharCode + keyboardIndex.
	struct GFxCharEvent : RE::GFxEvent {
		std::uint32_t wcharCode;     // 04
		std::uint8_t  keyboardIndex; // 08

		GFxCharEvent(std::uint32_t a_code, std::uint8_t a_ki = 0)
			: GFxEvent(EventType::kCharEvent), wcharCode(a_code), keyboardIndex(a_ki) {}
	};

	// Cached game window handle for SetProp
	HWND g_gameHwnd = nullptr;

	// Stored callbacks from BSVirtualKeyboardDevice::Start()
	// (Fix 2.1: Protected by mutex - written by game thread, read by WndProc thread)
	using DoneCallback_t = RE::BSVirtualKeyboardDevice::kbInfo::DoneCallback;
	using CancelCallback_t = RE::BSVirtualKeyboardDevice::kbInfo::CancelCallback;

	std::mutex        g_callbackMutex;  // Fix 2.1: Protects callback state across threads
	DoneCallback_t*   g_doneCallback = nullptr;
	CancelCallback_t* g_cancelCallback = nullptr;
	void*             g_userParam = nullptr;
	bool              g_waitingForKeyboard = false;

	// Original vtable entry (no-op in VR, but saved for completeness)
	using Start_t = void(__thiscall*)(RE::BSVirtualKeyboardDevice*, const RE::BSVirtualKeyboardDevice::kbInfo*);
	Start_t g_originalStart = nullptr;

	// =========================================================================
	// Menu state watcher — signals Open Composite when menus are active
	// =========================================================================

	// Menu names we care about for laser pointer activation
	// WARNING: Do NOT add StatsMenu here! The level-up menu uses a special
	// Sovngarde constellation scene that corrupts rendering if we access
	// ANY Scaleform MovieDef data while it's open.
	static constexpr std::string_view kTrackedMenus[] = {
		"Journal Menu",
		"InventoryMenu",
		"MagicMenu",
		"TweenMenu",
		"ContainerMenu",
		"BarterMenu",
		"FavoritesMenu",
		"Crafting Menu",
		"Dialogue Menu",
		"Book Menu",
		"GiftMenu",
		"Sleep/Wait Menu",
		"Lockpicking Menu",
		"Training Menu",
		"MessageBoxMenu",
		"RaceSex Menu", // vanilla/RaceMenu character creation
		"CustomMenu", // SkyUI MCM host
		// StatsMenu excluded — Sovngarde constellation bug
	};

	// Track which of our target menus are currently open (avoids calling
	// ui->IsMenuOpen from inside the event handler, which deadlocks because
	// Bethesda's UI holds a lock during MenuOpenCloseEvent dispatch).
	std::set<std::string> g_activeTrackedMenus;
	// MenuOpenCloseEvent is ordered. Keep that order instead of resolving the
	// active movie from std::set, which made InventoryMenu beat MagicMenu
	// alphabetically during their hand-off even when Magic was actually on top.
	std::vector<std::string> g_trackedMenuOpenOrder;
	std::atomic<bool> g_consoleOpen{ false }; // game-thread event, scheduler read
	// Observe native menu controls without consuming them. The pump uses this
	// serial as a sticky last-input arbiter; a resting laser ray is not intent.
	std::atomic<std::uint64_t> g_controllerMenuIntentSerial{ 0 };
	// Incremented for every tracked-menu stack change. The pump is normally
	// queued only while a menu is active, so it cannot depend on observing an
	// inactive tick to distinguish closing and reopening the same menu.
	std::uint32_t g_menuPlaneGeneration = 0;
	bool g_statsMenuOpen = false; // StatsMenu opens ON TOP of TweenMenu — laser must go dormant
	bool g_mapMenuOpen = false; // Native-only exclusion, never an OCU interaction target.

	class MenuInputIntentWatcher : public RE::BSTEventSink<RE::InputEvent*>
	{
	public:
		static MenuInputIntentWatcher* GetSingleton()
		{
			static MenuInputIntentWatcher singleton;
			return &singleton;
		}

		RE::BSEventNotifyControl ProcessEvent(
		    RE::InputEvent* const* a_events,
		    RE::BSTEventSource<RE::InputEvent*>* /*a_source*/) override
		{
			if (!a_events || !*a_events)
				return RE::BSEventNotifyControl::kContinue;

			bool controllerIntent = false;
			for (auto event = *a_events; event && !controllerIntent; event = event->next) {
				const auto device = event->GetDevice();
				const bool controllerDevice = device == RE::INPUT_DEVICE::kGamepad ||
				    device == RE::INPUT_DEVICE::kVivePrimary ||
				    device == RE::INPUT_DEVICE::kViveSecondary ||
				    device == RE::INPUT_DEVICE::kOculusPrimary ||
				    device == RE::INPUT_DEVICE::kOculusSecondary ||
				    device == RE::INPUT_DEVICE::kWMRPrimary ||
				    device == RE::INPUT_DEVICE::kWMRSecondary;
				if (!controllerDevice)
					continue;

				if (event->GetEventType() == RE::INPUT_EVENT_TYPE::kThumbstick) {
					if (const auto stick = event->AsThumbstickEvent()) {
						constexpr float kIntentDeadzone = 0.40f;
						controllerIntent = stick->xValue * stick->xValue +
						    stick->yValue * stick->yValue >= kIntentDeadzone * kIntentDeadzone;
					}
					continue;
				}

				if (event->GetEventType() != RE::INPUT_EVENT_TYPE::kButton)
					continue;
				const auto button = event->AsButtonEvent();
				if (!button || !button->IsDown())
					continue;

				// Trigger (0x21) is OCU's laser click and has its own press serial.
				// Counting it here would cancel the same laser click one frame later.
				if (button->GetIDCode() == 0x21)
					continue;

				const std::string_view action = button->GetUserEvent().c_str();
				controllerIntent = action == "Accept" || action == "Cancel" ||
				    action == "Cancel Alt" || action == "Up" || action == "Down" ||
				    action == "Left" || action == "Right" ||
				    action == "Left Stick" || action == "Right Stick" ||
				    action == "XButton" || action == "YButton";
			}

			if (controllerIntent)
				g_controllerMenuIntentSerial.fetch_add(1, std::memory_order_release);
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	// Update shared memory with the active menu's 3D transform data
	void UpdateMenuTransform()
	{
		if (!g_pTransform)
			return;

		++g_menuPlaneGeneration;

		auto ui = RE::UI::GetSingleton();
		if (!ui)
			return;

		// StatsMenu on top forces "no menu" for the laser export — the DLL must
		// not keep a quad alive in the Sovngarde constellation view. WASD
		// blocking (active flag below) still sees the pause, so that behavior
		// is unchanged.
		const std::string* topTrackedMenu = nullptr;
		for (auto it = g_trackedMenuOpenOrder.rbegin(); it != g_trackedMenuOpenOrder.rend(); ++it) {
			if (g_activeTrackedMenus.count(*it) != 0) {
				topTrackedMenu = &*it;
				break;
			}
		}
		bool anyActive = topTrackedMenu != nullptr && !g_statsMenuOpen && !g_mapMenuOpen;
		bool gamePaused = ui->GameIsPaused();

		// Begin write (odd counter = writing)
		g_pTransform->updateCounter++;
		// The previous menu's interaction plane is dead immediately. The pump
		// republishes after the newly opened render mesh has a coherent pose.
		g_pTransform->uiPlaneValid = 0;
		g_pTransform->mapPointerValid = 0;

		// Allow WASD when ANY menu is active OR game is paused (kPausesGame menu like text boxes)
		g_pTransform->active = anyActive || gamePaused || g_mapMenuOpen;
		SKSE::log::debug("  SharedMem write: anyActive={}, gamePaused={}, active={}",
		    anyActive, gamePaused, g_pTransform->active);

		if (g_mapMenuOpen) {
			// Exclusion sentinel only: the runtime must suppress even a stale
			// underlying menu or the calibration override. No map plane is exported.
			strcpy_s(g_pTransform->menuName, "MapMenu");
			g_pTransform->hasPerspective = false;
			g_pTransform->updateCounter++;
			return;
		}

		if (!anyActive) {
			g_pTransform->menuName[0] = '\0';
			g_pTransform->hasPerspective = false;
			g_pTransform->updateCounter++; // End write (even = stable)
			return;
		}

		// Write the actual last-opened tracked menu so geometry and input resolve
		// against the same top movie. No Scaleform data is touched here.
		strncpy_s(g_pTransform->menuName, topTrackedMenu->c_str(), 63);
		g_pTransform->menuName[63] = '\0';

		// [EXPERIMENTAL — DISABLED] Scaleform access for stage dimensions.
		// Accessing GetMenu/GetMovieDef/depthPriority/stageWidth/stageHeight
		// on ANY menu risks the Sovngarde bug. All Scaleform access from SKSE
		// is disabled until we have a proven-safe approach.
		// g_pTransform->depthPriority = safeMenu->depthPriority;
		// auto def = safeMenu->uiMovie->GetMovieDef();
		// if (def) {
		// 	g_pTransform->stageWidth = def->GetWidth();
		// 	g_pTransform->stageHeight = def->GetHeight();
		// }
		g_pTransform->hasPerspective = false;

		// End write (even counter = stable)
		g_pTransform->updateCounter++;
	}

	class MenuWatcher : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		RE::BSEventNotifyControl ProcessEvent(
		    const RE::MenuOpenCloseEvent* a_event,
		    RE::BSTEventSource<RE::MenuOpenCloseEvent>* /*a_source*/) override
		{
			if (!g_gameHwnd || !a_event)
				return RE::BSEventNotifyControl::kContinue;

			std::string_view name = a_event->menuName.c_str();

			// Observe lifecycle solely to disable OCU. Never register the map as
			// an interaction target or inspect its movie, geometry, or native laser.
			if (name == "MapMenu") {
				g_mapMenuOpen = a_event->opening;
				UpdateMenuTransform();
			}

			// Track specific menus for laser pointer system
			bool isTracked = false;
			for (auto& m : kTrackedMenus) {
				if (name == m) {
					isTracked = true;
					break;
				}
			}

			if (isTracked) {
				const std::string menuName(name);
				if (a_event->opening) {
					g_activeTrackedMenus.insert(menuName);
					std::erase(g_trackedMenuOpenOrder, menuName);
					g_trackedMenuOpenOrder.push_back(menuName);
				} else {
					g_activeTrackedMenus.erase(menuName);
					std::erase(g_trackedMenuOpenOrder, menuName);
				}

				// Update shared memory whenever tracked menus change
				UpdateMenuTransform();
			}

			// StatsMenu (level-up constellation) opens ON TOP of TweenMenu, which
			// stays open underneath — so menuName stayed 'TweenMenu' and the DLL
			// kept a stale fallback quad floating in Sovngarde (2026-07-25
			// screenshot). Treat StatsMenu-open as "no menu" for the laser: clear
			// the export while it's up, restore from the tracked set on close.
			if (name == "StatsMenu") {
				g_statsMenuOpen = a_event->opening;
				UpdateMenuTransform();
			}

			// Track console open/close for WM_CHAR suppression, and publish it
			// to the bridge so the VR keyboard's console overlay follows the
			// REAL console state instead of guessing from its own tilde toggle.
			if (name == "Console") {
				g_consoleOpen = a_event->opening;
				if (g_pBridge)
					g_pBridge->isConsoleOpen = a_event->opening ? 1 : 0;
			}

			// Track main menu state for FSR3 (disable temporal upscaling on main menu)
			if (name == "Main Menu" && g_pBridge) {
				g_pBridge->isMainMenu = a_event->opening ? 1 : 0;
				SKSE::log::info("RT Bridge: isMainMenu = {}", (int)g_pBridge->isMainMenu);
			}

			// Track loading screen state for FSR3 + retry NiCamera lookup on close
			if (name == "Loading Menu" && g_pBridge) {
				g_pBridge->isLoadingScreen = a_event->opening ? 1 : 0;
				SKSE::log::info("RT Bridge: isLoadingScreen = {}", (int)g_pBridge->isLoadingScreen);
				if (!a_event->opening) {
					FindAndStoreNiCamera();
					// Retry first-person node lookup — Get3D(true) returns null until gameplay.
					if (g_pBridge && !g_pBridge->playerFirstPersonRootPtr) {
						bool saved = g_niCameraFound;
						g_niCameraFound = false;
						FindAndStoreNiCamera();
						if (!g_niCameraFound) g_niCameraFound = saved;
					}
				}
			}

			// Set OC_MENU_ACTIVE for ALL menus (for WASD blocking in OpenComposite)
			// IsShowingMenus() returns false in SkyrimVR — use tracked menus + GameIsPaused instead
			auto ui = RE::UI::GetSingleton();
			if (ui) {
				bool anyMenuVisible = !g_activeTrackedMenus.empty() || ui->GameIsPaused();
				SetPropW(g_gameHwnd, L"OC_MENU_ACTIVE",
				    (HANDLE)(intptr_t)(anyMenuVisible ? 1 : 0));
				SKSE::log::debug("Menu {} {} - active:{} gamePaused:{}",
				    name, a_event->opening ? "opened" : "closed",
				    !g_activeTrackedMenus.empty(), ui->GameIsPaused());

				// Update ASW menu flag — when any menu is visible, ASW skips MV
				// corrections to prevent UI duplication on warp frames.
				if (g_pBridge)
					g_pBridge->isMenuOpen = anyMenuVisible ? 1 : 0;
			}

			return RE::BSEventNotifyControl::kContinue;
		}
	};

	// =========================================================================
	// VR laser cursor pump — Scaleform-free menu pointing (Sovngarde-safe)
	//
	// Replaces the old WM_OC_LASER GFxMouseEvent injection. Design rules:
	//   1. NEVER touch any menu's uiMovie/MovieDef. The Sovngarde bug came
	//      from out-of-band Scaleform calls (wrong thread, wrong movie) while
	//      StatsMenu's constellation scene owned the shared renderer state.
	//   2. Everything here runs on the game thread (via SKSE task queue).
	//   3. Input goes through BSInputEventQueue, so the game itself routes
	//      mouse moves/clicks to the topmost menu with its own locking.
	//
	// Per pump tick:
	//   - Export the game's own uiNode plane (RoomNode-local -> OpenXR floor
	//     space) so the DLL raycasts against the REAL menu plane.
	//   - Export MenuCursor position/range as feedback.
	//   - If the DLL reports a laser hit, drive the cursor toward the target
	//     with closed-loop MouseMoveEvents and forward trigger press/release
	//     as mouse button 0 events.
	// =========================================================================

	// 1 meter = 69.99125 Skyrim units (Bethesda's VR world scale)
	constexpr float kSkyrimUnitsPerMeter = 69.99125f;

	std::atomic<bool> g_laserPumpRunning{ false };
	// The scheduler runs independently from Skyrim's main thread. Never allow it
	// to queue another copy of a game-thread job while the previous copy is still
	// waiting or executing. A blocked Scaleform/menu frame otherwise accumulates
	// hundreds of stale pumps, producing the reported multi-second menu stalls.
	std::atomic<bool> g_laserPumpTaskPending{ false };
	std::atomic<bool> g_consolePickTaskPending{ false };
	std::atomic<bool> g_renderTargetRefreshTaskPending{ false };

	// Skyrim (X right, Y forward, Z up) -> OpenXR floor space (X right, Y up, Z back)
	inline void MapSkyrimToXr(const RE::NiPoint3& s, float out[3])
	{
		out[0] = s.x;
		out[1] = s.z;
		out[2] = -s.y;
	}

	// Build quaternion (x,y,z,w) from three orthonormal OpenXR-space columns:
	// col0 = quad right, col1 = quad up, col2 = quad normal (toward viewer)
	void QuatFromBasis(const float right[3], const float up[3], const float normal[3], float q[4])
	{
		float m00 = right[0], m01 = up[0], m02 = normal[0];
		float m10 = right[1], m11 = up[1], m12 = normal[1];
		float m20 = right[2], m21 = up[2], m22 = normal[2];
		float trace = m00 + m11 + m22;
		if (trace > 0.0f) {
			float s = sqrtf(trace + 1.0f) * 2.0f;
			q[3] = 0.25f * s;
			q[0] = (m21 - m12) / s;
			q[1] = (m02 - m20) / s;
			q[2] = (m10 - m01) / s;
		} else if (m00 > m11 && m00 > m22) {
			float s = sqrtf(1.0f + m00 - m11 - m22) * 2.0f;
			q[3] = (m21 - m12) / s;
			q[0] = 0.25f * s;
			q[1] = (m01 + m10) / s;
			q[2] = (m02 + m20) / s;
		} else if (m11 > m22) {
			float s = sqrtf(1.0f + m11 - m00 - m22) * 2.0f;
			q[3] = (m02 - m20) / s;
			q[0] = (m01 + m10) / s;
			q[1] = 0.25f * s;
			q[2] = (m12 + m21) / s;
		} else {
			float s = sqrtf(1.0f + m22 - m00 - m11) * 2.0f;
			q[3] = (m10 - m01) / s;
			q[0] = (m02 + m20) / s;
			q[1] = (m12 + m21) / s;
			q[2] = 0.25f * s;
		}
	}

	// Column extraction: image of local axis a (0=X,1=Y,2=Z) under rotation
	inline RE::NiPoint3 MatColumn(const RE::NiMatrix3& r, int a)
	{
		return { r.entry[0][a], r.entry[1][a], r.entry[2][a] };
	}

	// R^T * v for NiMatrix3 (entry[row][col], columns = rotated basis vectors)
	inline RE::NiPoint3 TransposeMul(const RE::NiMatrix3& r, const RE::NiPoint3& v)
	{
		return {
			r.entry[0][0] * v.x + r.entry[1][0] * v.y + r.entry[2][0] * v.z,
			r.entry[0][1] * v.x + r.entry[1][1] * v.y + r.entry[2][1] * v.z,
			r.entry[0][2] * v.x + r.entry[1][2] * v.y + r.entry[2][2] * v.z
		};
	}

	// Export the game's UI plane (uiNode) into shared memory, RoomNode-local,
	// converted to OpenXR axes in Skyrim's app-selected tracking-origin space.
	// Returns true if a valid plane was written.
	// Compose immutable/local scene transforms up to an ancestor. Reading
	// uiNode.world and RoomNode.world independently can mix two scene-update
	// frames and manufacture a large transient displacement.
	bool BuildLocalToAncestor(RE::NiAVObject* object, RE::NiNode* ancestor, RE::NiTransform& out)
	{
		if (!object || !ancestor)
			return false;

		out = RE::NiTransform{};
		RE::NiAVObject* current = object;
		for (int depth = 0; current && current != ancestor && depth < 32; ++depth) {
			out = current->local * out;
			current = current->parent;
		}
		return current == ancestor;
	}

	// Dialogue is loaded from skyVR_dialogue.nif rather than the normal
	// InWorldUIQuadGeo pointer.  Prefer the named stock mesh, but retain a
	// scene-graph fallback for replacement NIFs that rename it.
	RE::BSTriShape* FindLargestVisibleTriShape(RE::NiAVObject* object, int depth = 0)
	{
		if (!object || depth > 16)
			return nullptr;

		RE::BSTriShape* best = nullptr;
		float bestRadius = -1.0f;
		if (auto* tri = object->AsTriShape(); tri && !tri->GetAppCulled()) {
			const float r = tri->GetModelData().modelBound.radius;
			if (std::isfinite(r) && r > 0.01f) {
				best = tri;
				bestRadius = r;
			}
		}

		if (auto* node = object->AsNode()) {
			for (auto& child : node->GetChildren()) {
				auto* candidate = FindLargestVisibleTriShape(child.get(), depth + 1);
				if (!candidate)
					continue;
				const float r = candidate->GetModelData().modelBound.radius;
				if (r > bestRadius) {
					best = candidate;
					bestRadius = r;
				}
			}
		}
		return best;
	}

	bool ExportUiPlane(bool logDiagnostics, bool dialogueOpen, bool bookOpen)
	{
		auto pending = [logDiagnostics](const char* reason) {
			if (logDiagnostics)
				SKSE::log::debug("LASER plane pending: {}", reason);
			return false;
		};

		auto pc = RE::PlayerCharacter::GetSingleton();
		if (!pc)
			return pending("PlayerCharacter unavailable");
		auto vrData = pc->GetVRNodeData();
		if (!vrData)
			return pending("VR node data unavailable");

		RE::NiNode* roomNode = vrData->RoomNode.get();
		if (!roomNode)
			return pending("RoomNode unavailable");

		// Book Menu is neither the normal Scaleform quad nor DialogueUINode. Its
		// text is rendered onto a physical 3D book/note model owned by BookMenu.
		// Use that live model so the ray follows the game's actual reading depth.
		RE::GPtr<RE::BookMenu> bookMenu;
		RE::NiAVObject* bookModel = nullptr;
		bool bookIsNote = false;
		if (bookOpen) {
			auto* ui = RE::UI::GetSingleton();
			if (!ui)
				return pending("UI unavailable for Book Menu");
			bookMenu = ui->GetMenu<RE::BookMenu>();
			if (!bookMenu)
				return pending("BookMenu unavailable");
			auto& bookData = bookMenu->GetRuntimeData();
			if (!bookData.bookInitialized || !bookData.bookModel)
				return pending("BookMenu physical model is not initialized");
			bookModel = bookData.bookModel.get();
			bookIsNote = bookData.isNote;
		}

		// Dialogue is not rendered on the normal in-world UI quad. Skyrim VR
		// exposes a dedicated, much closer DialogueUINode; using uiNode here sends
		// the laser through the choices and into the NPC/world behind them.
		RE::NiNode* uiNode = bookOpen ? nullptr :
		    (dialogueOpen ? vrData->DialogueUINode.get() : vrData->uiNode.get());
		if (!bookOpen && !uiNode)
			return pending(dialogueOpen ? "DialogueUINode unavailable" : "uiNode unavailable");

		RE::BSTriShape* quadGeo = (dialogueOpen || bookOpen) ? nullptr : vrData->InWorldUIQuadGeo.get();
		if (dialogueOpen) {
			if (auto* named = uiNode->GetObjectByName(RE::BSFixedString("skyVR_dialogue")))
				quadGeo = named->AsTriShape();
			if (!quadGeo)
				quadGeo = FindLargestVisibleTriShape(uiNode);
		}
		// If a replacement dialogue mesh is not parented beneath RoomNode, its
		// world transform is still more authoritative than DialogueUINode's
		// parent basis. Normal menus retain the proven uiNode fallback.
		RE::NiAVObject* fallbackPlaneObject = bookOpen ? bookModel :
		    (dialogueOpen && quadGeo ? static_cast<RE::NiAVObject*>(quadGeo) :
		                               static_cast<RE::NiAVObject*>(uiNode));

		// InWorldUIQuadGeo is not a RoomNode descendant in every menu. Requiring
		// that parent chain made plane export fail forever and blanked the quad
		// plus both lasers. uiNode.world is the proven render pose. The caller
		// samples it only during a short stabilization window, then freezes the
		// accepted RoomNode-relative plane for the entire menu lifetime. That
		// avoids both the missing-geometry deadlock and the old head-pose drift.
		const RE::NiTransform& planeW = fallbackPlaneObject->world;
		const RE::NiTransform& roomW = roomNode->world;
		if (!std::isfinite(roomW.scale) || fabsf(roomW.scale) < 1e-5f)
			return pending("RoomNode has invalid scale");
		if (!std::isfinite(fallbackPlaneObject->worldBound.radius) ||
		    fallbackPlaneObject->worldBound.radius < 1.0f)
			return pending("UI plane world bound is not ready");
		const float roomScale = roomW.scale;

		// Stale-node filter: on the first frame(s) of a menu the uiNode still
		// carries its previous/unplaced transform far from the playspace.
		// Exporting that garbage froze the display once (invalid layer pose).
		RE::NiPoint3 rel = fallbackPlaneObject->worldBound.center - roomW.translate;
		float relDist = sqrtf(rel.x * rel.x + rel.y * rel.y + rel.z * rel.z);
		if (!std::isfinite(relDist) || relDist > 700.0f)
			return pending("uiNode transform is stale or outside the playspace");

		// RoomNode-local pose of the UI plane. Use worldBound.center for the
		// plane center — the node origin can be an off-center pivot.
		// centerLocal came from the actual render mesh's model bound and local
		// transform chain, not separately updated world transforms.

		// Local rotation = R_room^T * R_ui. The game's uiNode world matrix can
		// contain a REFLECTION (negative determinant — observed live), so we
		// only trust two axes and REBUILD an orthonormal right-handed basis
		// with cross products. A hand-rolled quat from a reflected basis is
		// non-unit -> XR_ERROR_POSE_INVALID -> frozen display. Never again.
		RE::NiTransform geoToRoom;
		const bool coherentGeometry = !bookOpen && quadGeo && BuildLocalToAncestor(quadGeo, roomNode, geoToRoom);
		RE::NiTransform bookToRoom;
		const bool coherentBook = bookOpen && BuildLocalToAncestor(bookModel, roomNode, bookToRoom);
		RE::NiPoint3 centerLocal;
		RE::NiPoint3 fwdS;
		RE::NiPoint3 upS;
		RE::NiPoint3 normalS;
		float planeRadiusRoom;
		if (bookOpen) {
			// Reading assets use local X=right, Y=up, Z=page normal. The model's
			// live world bound follows Skyrim's VR book/note placement, including
			// replacement meshes and its configured reading distance.
			centerLocal = TransposeMul(roomW.rotate, rel);
			centerLocal /= roomScale;
			if (coherentBook) {
				upS = MatColumn(bookToRoom.rotate, 1);
				normalS = MatColumn(bookToRoom.rotate, 2);
			} else {
				upS = TransposeMul(roomW.rotate, MatColumn(planeW.rotate, 1));
				normalS = TransposeMul(roomW.rotate, MatColumn(planeW.rotate, 2));
			}
			planeRadiusRoom = fallbackPlaneObject->worldBound.radius / fabsf(roomScale);

			// Keep the page upright and make its exported +normal face the HMD.
			// The runtime's final face guard is then a safety net, not a source of
			// left/right page mirroring.
			if (upS.z < 0.0f)
				upS = { -upS.x, -upS.y, -upS.z };
			if (auto* hmdNode = vrData->UprightHmdNode.get()) {
				RE::NiPoint3 hmdRel = hmdNode->world.translate - roomW.translate;
				RE::NiPoint3 hmdLocal = TransposeMul(roomW.rotate, hmdRel);
				hmdLocal /= roomScale;
				const RE::NiPoint3 toHmd = hmdLocal - centerLocal;
				const float facing = normalS.x * toHmd.x + normalS.y * toHmd.y + normalS.z * toHmd.z;
				if (facing < 0.0f)
					normalS = { -normalS.x, -normalS.y, -normalS.z };
			}
		} else if (coherentGeometry) {
			const auto& modelBound = quadGeo->GetModelData().modelBound;
			centerLocal = geoToRoom * modelBound.center;
			fwdS = MatColumn(geoToRoom.rotate, 1); // local +Y
			upS = MatColumn(geoToRoom.rotate, 2);  // local +Z
			normalS = { -fwdS.x, -fwdS.y, -fwdS.z }; // toward viewer
			planeRadiusRoom = modelBound.radius * fabsf(geoToRoom.scale);
		} else {
			centerLocal = TransposeMul(roomW.rotate, rel);
			centerLocal /= roomScale;
			fwdS = TransposeMul(roomW.rotate, MatColumn(planeW.rotate, 1));
			upS = TransposeMul(roomW.rotate, MatColumn(planeW.rotate, 2));
			normalS = { -fwdS.x, -fwdS.y, -fwdS.z }; // toward viewer
			planeRadiusRoom = fallbackPlaneObject->worldBound.radius / fabsf(roomScale);
		}

		auto norm3 = [](RE::NiPoint3& v) -> bool {
			float m = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
			if (m < 1e-4f)
				return false;
			v.x /= m; v.y /= m; v.z /= m;
			return true;
		};
		auto cross3 = [](const RE::NiPoint3& a, const RE::NiPoint3& b) -> RE::NiPoint3 {
			return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
		};
		if (!norm3(upS) || !norm3(normalS))
			return false;
		RE::NiPoint3 rightS = cross3(upS, normalS);   // right-handed: X = Y x Z
		if (!norm3(rightS))
			return false;
		upS = cross3(normalS, rightS);                // re-orthogonalize
		if (!norm3(upS))
			return false;

		// Convert to OpenXR axes (the axis map is a proper rotation, so
		// the basis stays orthonormal and right-handed).
		float rightXr[3], upXr[3], normalXr[3], posXr[3], quat[4];
		MapSkyrimToXr(rightS, rightXr);
		MapSkyrimToXr(upS, upXr);
		MapSkyrimToXr(normalS, normalXr);
		MapSkyrimToXr(centerLocal, posXr);
		posXr[0] /= kSkyrimUnitsPerMeter;
		posXr[1] /= kSkyrimUnitsPerMeter;
		posXr[2] /= kSkyrimUnitsPerMeter;
		QuatFromBasis(rightXr, upXr, normalXr, quat);

		// Guaranteed by construction, but never ship a non-unit quat to the
		// compositor: renormalize and bail if degenerate.
		float qn = sqrtf(quat[0] * quat[0] + quat[1] * quat[1] + quat[2] * quat[2] + quat[3] * quat[3]);
		if (qn < 0.5f || !std::isfinite(qn))
			return false;
		for (int i = 0; i < 4; i++)
			quat[i] /= qn;

		// v3 reference-frame bridge: export Skyrim's upright HMD in the same
		// RoomNode-local metric frame as the menu plane. OpenXR's Stage origin
		// is not guaranteed to be RoomNode's origin (recenter and room setup can
		// translate/rotate it), so the compositor maps plane-relative-to-HMD
		// instead of treating RoomNode coordinates as Stage coordinates.
		g_pTransform->roomHmdValid = 0;
		auto* hmdNode = vrData->UprightHmdNode.get();
		if (hmdNode) {
			RE::NiTransform hmdToRoom;
			const bool coherentHmd = BuildLocalToAncestor(hmdNode, roomNode, hmdToRoom);
			RE::NiPoint3 hmdPosS;
			RE::NiPoint3 hmdFwdS;
			RE::NiPoint3 hmdUpS;
			if (coherentHmd) {
				hmdPosS = hmdToRoom.translate;
				hmdFwdS = MatColumn(hmdToRoom.rotate, 1);
				hmdUpS = MatColumn(hmdToRoom.rotate, 2);
			} else {
				RE::NiPoint3 hmdRel = hmdNode->world.translate - roomW.translate;
				hmdPosS = TransposeMul(roomW.rotate, hmdRel);
				hmdPosS /= roomScale;
				hmdFwdS = TransposeMul(roomW.rotate, MatColumn(hmdNode->world.rotate, 1));
				hmdUpS = TransposeMul(roomW.rotate, MatColumn(hmdNode->world.rotate, 2));
			}

			if (norm3(hmdFwdS) && norm3(hmdUpS)) {
				RE::NiPoint3 hmdBackS = { -hmdFwdS.x, -hmdFwdS.y, -hmdFwdS.z };
				RE::NiPoint3 hmdRightS = cross3(hmdUpS, hmdBackS);
				if (norm3(hmdRightS)) {
					hmdUpS = cross3(hmdBackS, hmdRightS);
					if (norm3(hmdUpS)) {
						float hRightXr[3], hUpXr[3], hBackXr[3], hPosXr[3], hQuat[4];
						MapSkyrimToXr(hmdRightS, hRightXr);
						MapSkyrimToXr(hmdUpS, hUpXr);
						MapSkyrimToXr(hmdBackS, hBackXr);
						MapSkyrimToXr(hmdPosS, hPosXr);
						for (int i = 0; i < 3; ++i)
							hPosXr[i] /= kSkyrimUnitsPerMeter;
						QuatFromBasis(hRightXr, hUpXr, hBackXr, hQuat);
						float hqn = sqrtf(hQuat[0] * hQuat[0] + hQuat[1] * hQuat[1] + hQuat[2] * hQuat[2] + hQuat[3] * hQuat[3]);
						if (std::isfinite(hqn) && hqn > 0.5f) {
							for (int i = 0; i < 3; ++i)
								g_pTransform->roomHmdPos[i] = hPosXr[i];
							for (int i = 0; i < 4; ++i)
								g_pTransform->roomHmdQuat[i] = hQuat[i] / hqn;
							g_pTransform->roomHmdValid = 1;
						}
					}
				}
			}
		}

		// MapMenu is a hard native bypass. These retained ABI fields are always
		// cleared; OCU never reads its pointer geometry or publishes map depth.
		g_pTransform->mapPointerValid = 0;
		g_pTransform->mapPointerDistanceMeters = 0.0f;

		// Plane extents from the node's bounding sphere. The plane geometry
		// ('In World UI Quad Geometry') is a 16:9 quad (verified live: local
		// half-extents 1.0 x 0.5625, corner radius 1.1473 — matches
		// worldBound.radius / world.scale exactly). Do NOT use the cursor
		// range for aspect: that's the square 2048x2048 render target.
		// skyVR_dialogue.nif is a square. Treating its bounding sphere as 16:9
		// shrank the vertical hit region enough that dialogue choices missed the
		// quad and the renderer fell back to its visibly short 0.5 m beam.
		// Physical reading models are different again. A stock note is a portrait
		// sheet (~0.76:1); an open book is two facing pages (~1.5:1). Depth and
		// overall scale still come from the live model bound, never these ratios.
		const float aspect = bookOpen ? (bookIsNote ? 0.761f : 1.5f) :
		    (dialogueOpen ? 1.0f : 16.0f / 9.0f);
		float radiusM = planeRadiusRoom / kSkyrimUnitsPerMeter;
		float heightM = 2.0f * radiusM / sqrtf(1.0f + aspect * aspect);
		float widthM = heightM * aspect;

		g_pTransform->uiPlanePos[0] = posXr[0];
		g_pTransform->uiPlanePos[1] = posXr[1];
		g_pTransform->uiPlanePos[2] = posXr[2];
		for (int i = 0; i < 4; i++)
			g_pTransform->uiPlaneQuat[i] = quat[i];
		g_pTransform->uiPlaneWidth = widthM;
		g_pTransform->uiPlaneHeight = heightM;

		if (logDiagnostics) {
			const char* planeSource = bookOpen ?
			    (coherentBook ? (bookIsNote ? "note-model-local" : "book-model-local") :
			                    (bookIsNote ? "note-model-world" : "book-model-world")) :
			    (dialogueOpen ? (coherentGeometry ? "dialogue-geometry-local" : "dialogue-geometry-world") :
			                    (coherentGeometry ? "geometry-local" : "world-fallback"));
			SKSE::log::debug("LASER live-plane source={} world center({:.2f},{:.2f},{:.2f}) r={:.2f} uiFrame={} roomFrame={}",
			    planeSource,
			    fallbackPlaneObject->worldBound.center.x, fallbackPlaneObject->worldBound.center.y,
			    fallbackPlaneObject->worldBound.center.z, fallbackPlaneObject->worldBound.radius,
			    fallbackPlaneObject->lastUpdatedFrameCounter, roomNode->lastUpdatedFrameCounter);
			SKSE::log::debug("LASER live plane app-space pos({:.3f},{:.3f},{:.3f}) quat({:.3f},{:.3f},{:.3f},{:.3f}) size {:.3f}x{:.3f}m",
			    posXr[0], posXr[1], posXr[2], quat[0], quat[1], quat[2], quat[3], widthM, heightM);
			if (g_pTransform->roomHmdValid) {
				SKSE::log::debug("LASER RoomNode HMD pos({:.3f},{:.3f},{:.3f}) quat({:.3f},{:.3f},{:.3f},{:.3f})",
				    g_pTransform->roomHmdPos[0], g_pTransform->roomHmdPos[1], g_pTransform->roomHmdPos[2],
				    g_pTransform->roomHmdQuat[0], g_pTransform->roomHmdQuat[1],
				    g_pTransform->roomHmdQuat[2], g_pTransform->roomHmdQuat[3]);
			}
			// First-level children of uiNode — identifies the actual menu geometry
			if (bookOpen && bookMenu) {
				auto& bookData = bookMenu->GetRuntimeData();
				SKSE::log::debug("LASER BookMenu isNote={} initialized={} startAnimating={} model='{}' bound r={:.2f}",
				    bookData.isNote, bookData.bookInitialized, bookData.startAnimating,
				    bookModel->name.c_str(), bookModel->worldBound.radius);
				if (auto* textGeo = bookData.pageTextGeo.get()) {
					SKSE::log::debug("LASER BookMenu PageText template '{}' center({:.2f},{:.2f},{:.2f}) r={:.2f} culled={}",
					    textGeo->name.c_str(), textGeo->worldBound.center.x, textGeo->worldBound.center.y,
					    textGeo->worldBound.center.z, textGeo->worldBound.radius, textGeo->GetAppCulled());
				}
			} else if (uiNode) {
				for (auto& child : uiNode->GetChildren()) {
					if (child) {
						SKSE::log::debug("LASER uiNode child '{}' bound r={:.2f} culled={}",
						    child->name.c_str(), child->worldBound.radius, child->GetAppCulled());
					}
				}
			}
		}
		return true;
	}

	// Route laser page gestures through Skyrim's Book input context. This keeps
	// the physical page animation, sound, page bounds, and mod hooks in native
	// code; BookMenu's Scaleform movie has no useful mouse hit targets.
	bool QueueBookPageAction(bool nextPage)
	{
		auto* queue = RE::BSInputEventQueue::GetSingleton();
		if (!queue)
			return false;

		const std::string_view actionName = nextPage ? "NextPage" : "PrevPage";
		RE::BSFixedString action(actionName);
		if (auto* userEvents = RE::UserEvents::GetSingleton())
			action = nextPage ? userEvents->nextPage : userEvents->prevPage;

		std::uint32_t key = nextPage ? 0xCDu : 0xCBu; // stock Right / Left arrows
		if (auto* controls = RE::ControlMap::GetSingleton()) {
			const auto mapped = controls->GetMappedKey(actionName, RE::INPUT_DEVICE::kKeyboard,
			    RE::UserEvents::INPUT_CONTEXT_ID::kBook);
			if (mapped != RE::ControlMap::kInvalid)
				key = mapped;
		}

		queue->AddButtonEvent(RE::INPUT_DEVICE::kKeyboard, 0, static_cast<std::int32_t>(key),
		    1.0f, 0.0f, action);
		queue->AddButtonEvent(RE::INPUT_DEVICE::kKeyboard, 0, static_cast<std::int32_t>(key),
		    0.0f, 0.06f, action);
		SKSE::log::debug("LASER BookMenu native action={} key=0x{:X}", actionName, key);
		return true;
	}

	// TweenMenu's mouse hit rectangles and visible selection clip disagree in
	// Skyrim VR: a click over visible Magic can reach Items (and vice versa).
	// The movie already records the authoritative visible highlight in
	// Selections_mc._currentframe. Activate that semantic selection directly so
	// pointer geometry never has to be mirrored or guessed.
	bool ActivateHighlightedTweenSelection(RE::GFxMovieView& movie)
	{
		RE::GFxValue currentFrame;
		if (movie.GetVariable(&currentFrame,
		        "_root.TweenMenu_mc.Selections_mc._currentframe") &&
		    currentFrame.IsNumber()) {
			const int selection = static_cast<int>(currentFrame.GetNumber()) - 1;
			if (selection >= 1 && selection <= 4) {
				RE::GFxValue arg;
				arg.SetNumber(static_cast<double>(selection));
				if (movie.Invoke("_root.TweenMenu_mc.onInputRectClick", nullptr, &arg, 1)) {
					SKSE::log::debug("LASER Tween semantic ACTIVATE selection={} ({})",
					    selection, selection == 1 ? "Skills" : selection == 2 ? "Magic" :
					    selection == 3 ? "Items" : "Map");
					return true;
				}
			}
		}

		// Fail safe: TweenMenu's own Enter handler also opens its current visual
		// selection. Never fall back to the mismatched mouse-down hit rectangle.
		RE::GFxKeyEvent down(RE::GFxEvent::EventType::kKeyDown,
		    RE::GFxKey::kReturn, 0, 0, {}, 0);
		RE::GFxKeyEvent up(RE::GFxEvent::EventType::kKeyUp,
		    RE::GFxKey::kReturn, 0, 0, {}, 0);
		movie.HandleEvent(down);
		movie.HandleEvent(up);
		SKSE::log::warn("LASER Tween semantic lookup failed; used current-highlight Enter fallback");
		return true;
	}

	void SendGFxKeyPulse(RE::GFxMovieView& movie, RE::GFxKey::Code key)
	{
		RE::GFxKeyEvent down(RE::GFxEvent::EventType::kKeyDown, key, 0, 0, {}, 0);
		RE::GFxKeyEvent up(RE::GFxEvent::EventType::kKeyUp, key, 0, 0, {}, 0);
		movie.HandleEvent(down);
		movie.HandleEvent(up);
	}

	bool GetNumberVariable(RE::GFxMovieView& movie, const char* firstPath,
	    const char* secondPath, int& result)
	{
		RE::GFxValue value;
		if ((!movie.GetVariable(&value, firstPath) || !value.IsNumber()) &&
		    (!secondPath || !movie.GetVariable(&value, secondPath) || !value.IsNumber())) {
			return false;
		}
		result = static_cast<int>(value.GetNumber());
		return true;
	}

	// AS2 MovieClip.hitTest() expects root/movie coordinates, while the laser and
	// NotifyMouseState use viewport pixels. Resolve the loaded clip and convert
	// through the movie's live viewport so interface replacers may move/scale it.
	bool ViewportToMovieRootPoint(RE::GFxMovieView& movie, float viewportX,
	    float viewportY, float& rootX, float& rootY)
	{
		RE::GViewport viewport{};
		movie.GetViewport(&viewport);
		const RE::GRectF visibleFrame = movie.GetVisibleFrameRect();
		const float frameWidth = visibleFrame.right - visibleFrame.left;
		const float frameHeight = visibleFrame.bottom - visibleFrame.top;
		if (viewport.width <= 0 || viewport.height <= 0 ||
		    !std::isfinite(frameWidth) || !std::isfinite(frameHeight) ||
		    frameWidth <= 0.0f || frameHeight <= 0.0f) {
			return false;
		}

		rootX = visibleFrame.left +
		    (viewportX - static_cast<float>(viewport.left)) * frameWidth /
		        static_cast<float>(viewport.width);
		rootY = visibleFrame.top +
		    (viewportY - static_cast<float>(viewport.top)) * frameHeight /
		        static_cast<float>(viewport.height);
		return std::isfinite(rootX) && std::isfinite(rootY);
	}

	bool DisplayObjectHitAtRootPoint(RE::GFxValue& clip, float rootX, float rootY,
	    bool shapeFlag = false)
	{
		if (!clip.IsObject() && !clip.IsDisplayObject())
			return false;
		std::array<RE::GFxValue, 3> hitArgs;
		hitArgs[0].SetNumber(rootX);
		hitArgs[1].SetNumber(rootY);
		hitArgs[2].SetBoolean(shapeFlag);
		RE::GFxValue hit;
		return clip.Invoke("hitTest", &hit, hitArgs) && hit.IsBool() && hit.GetBool();
	}

	bool RootPointToDisplayObjectLocal(RE::GFxMovieView& movie,
	    RE::GFxValue& clip, float rootX, float rootY, double& localX,
	    double& localY)
	{
		if (!clip.IsObject() && !clip.IsDisplayObject())
			return false;

		RE::GFxValue point;
		movie.CreateObject(&point);
		RE::GFxValue xValue;
		RE::GFxValue yValue;
		xValue.SetNumber(rootX);
		yValue.SetNumber(rootY);
		point.SetMember("x", xValue);
		point.SetMember("y", yValue);
		if (!clip.Invoke("globalToLocal", nullptr, &point, 1) ||
		    !point.GetMember("x", &xValue) || !xValue.IsNumber() ||
		    !point.GetMember("y", &yValue) || !yValue.IsNumber()) {
			return false;
		}

		localX = xValue.GetNumber();
		localY = yValue.GetNumber();
		return std::isfinite(localX) && std::isfinite(localY);
	}

	// Several RaceMenu 0.4.20 clips have visible geometry but no AS2 hit area in
	// Skyrim VR. MovieClip.getBounds() still reports their transformed rectangle,
	// so use it as a geometry fallback instead of allowing the click to fall
	// through to the broken VR Mouse singleton.
	bool GetDisplayObjectRootBounds(RE::GFxMovieView& movie,
	    RE::GFxValue& clip, double& xMin, double& xMax, double& yMin,
	    double& yMax)
	{
		if (!clip.IsObject() && !clip.IsDisplayObject())
			return false;

		RE::GFxValue root;
		if (!movie.GetVariable(&root, "_root") ||
		    (!root.IsObject() && !root.IsDisplayObject())) {
			return false;
		}
		RE::GFxValue bounds;
		if (!clip.Invoke("getBounds", &bounds, &root, 1) ||
		    (!bounds.IsObject() && !bounds.IsDisplayObject())) {
			return false;
		}

		auto numberMember = [&](const char* name, double& result) {
			RE::GFxValue value;
			if (!bounds.GetMember(name, &value) || !value.IsNumber())
				return false;
			result = value.GetNumber();
			return std::isfinite(result);
		};
		if (!numberMember("xMin", xMin) || !numberMember("xMax", xMax) ||
		    !numberMember("yMin", yMin) || !numberMember("yMax", yMax) ||
		    xMax <= xMin || yMax <= yMin) {
			return false;
		}
		return true;
	}

	bool DisplayObjectBoundsHitAtRootPoint(RE::GFxMovieView& movie,
	    RE::GFxValue& clip, float rootX, float rootY)
	{
		double xMin = 0.0;
		double xMax = 0.0;
		double yMin = 0.0;
		double yMax = 0.0;
		if (!GetDisplayObjectRootBounds(
		        movie, clip, xMin, xMax, yMin, yMax)) {
			return false;
		}
		return static_cast<double>(rootX) >= xMin &&
		    static_cast<double>(rootX) <= xMax &&
		    static_cast<double>(rootY) >= yMin &&
		    static_cast<double>(rootY) <= yMax;
	}

	bool DisplayObjectGeometryHitAtRootPoint(RE::GFxMovieView& movie,
	    RE::GFxValue& clip, float rootX, float rootY)
	{
		return DisplayObjectHitAtRootPoint(clip, rootX, rootY) ||
		    DisplayObjectBoundsHitAtRootPoint(movie, clip, rootX, rootY);
	}

	bool RootPointToDisplayObjectBoundsRatio(RE::GFxMovieView& movie,
	    RE::GFxValue& clip, float rootX, float rootY, double& ratioX,
	    double& ratioY, bool requireInside)
	{
		double xMin = 0.0;
		double xMax = 0.0;
		double yMin = 0.0;
		double yMax = 0.0;
		if (!GetDisplayObjectRootBounds(
		        movie, clip, xMin, xMax, yMin, yMax)) {
			return false;
		}
		const double x = static_cast<double>(rootX);
		const double y = static_cast<double>(rootY);
		if (requireInside &&
		    (x < xMin || x > xMax || y < yMin || y > yMax)) {
			return false;
		}
		ratioX = (x - xMin) / (xMax - xMin);
		ratioY = (y - yMin) / (yMax - yMin);
		return std::isfinite(ratioX) && std::isfinite(ratioY);
	}

	bool RootPointToRaceMenuSculptPoint(RE::GFxMovieView& movie,
	    RE::GFxValue& foreground, float rootX, float rootY, double& localX,
	    double& localY)
	{
		RE::GFxValue widthValue;
		RE::GFxValue heightValue;
		if (!foreground.GetMember("fixedWidth", &widthValue) ||
		    !widthValue.IsNumber() ||
		    !foreground.GetMember("fixedHeight", &heightValue) ||
		    !heightValue.IsNumber()) {
			return false;
		}
		const double width = widthValue.GetNumber();
		const double height = heightValue.GetNumber();
		if (!std::isfinite(width) || !std::isfinite(height) ||
		    width <= 0.0 || height <= 0.0) {
			return false;
		}

		// The image loader applies an internal scale to foreground. globalToLocal()
		// therefore returns values outside fixedWidth/fixedHeight even over the visible
		// image. Normalize through the loaded image's real root-space rectangle.
		RE::GFxValue wireframe;
		RE::GFxValue* boundsClip = &foreground;
		if (foreground.GetMember("wireframe", &wireframe) &&
		    (wireframe.IsObject() || wireframe.IsDisplayObject())) {
			boundsClip = &wireframe;
		}
		double ratioX = 0.0;
		double ratioY = 0.0;
		if (RootPointToDisplayObjectBoundsRatio(movie, *boundsClip, rootX, rootY,
		        ratioX, ratioY, true)) {
			localX = std::clamp(ratioX, 0.0, 1.0) * width;
			localY = std::clamp(ratioY, 0.0, 1.0) * height;
			return true;
		}

		// Keep a strict local-coordinate fallback for interface replacers that do not
		// publish useful getBounds data.
		if (!RootPointToDisplayObjectLocal(
		        movie, foreground, rootX, rootY, localX, localY)) {
			return false;
		}
		return localX >= 0.0 && localX <= width &&
		    localY >= 0.0 && localY <= height;
	}

	bool GetListScrollBar(RE::GFxValue& list, RE::GFxValue& scrollBar)
	{
		// SkyUI/RaceMenu lists normally publish `scrollbar`; Bethesda-derived
		// lists and interface replacers also use the other spellings below.
		constexpr std::array<const char*, 4> memberNames = {
		    "scrollbar", "scrollBar", "ListScrollbar", "_scrollBar"
		};
		for (const char* memberName : memberNames) {
			if (list.GetMember(memberName, &scrollBar) &&
			    (scrollBar.IsObject() || scrollBar.IsDisplayObject())) {
				return true;
			}
		}
		return false;
	}

	bool DisplayObjectIsUsable(RE::GFxValue& clip)
	{
		RE::GFxValue visible;
		if (clip.GetMember("_visible", &visible) && visible.IsBool() &&
		    !visible.GetBool()) {
			return false;
		}
		RE::GFxValue disabled;
		return !clip.GetMember("disabled", &disabled) || !disabled.IsBool() ||
		    !disabled.GetBool();
	}

	bool MovieClipHitAtViewportPoint(RE::GFxMovieView& movie,
	    const std::array<const char*, 4>& clipPaths, float viewportX, float viewportY)
	{
		float rootX = 0.0f;
		float rootY = 0.0f;
		if (!ViewportToMovieRootPoint(movie, viewportX, viewportY, rootX, rootY))
			return false;

		for (const char* path : clipPaths) {
			if (!path)
				continue;
			RE::GFxValue clip;
			if (!movie.GetVariable(&clip, path) ||
			    (!clip.IsObject() && !clip.IsDisplayObject())) {
				continue;
			}
			if (DisplayObjectHitAtRootPoint(clip, rootX, rootY))
				return true;
		}
		return false;
	}

	enum class RaceMenuLaserTargetKind
	{
		kNone,
		kModeTab,
		kCategory,
		kItem,
		kSlider,
		kScrollBar,
		kButton,
		kClipAction,
		kSculptCanvas
	};

	struct RaceMenuLaserTarget
	{
		RaceMenuLaserTargetKind kind = RaceMenuLaserTargetKind::kNone;
		int index = -1;
		RE::GFxValue owner;
		RE::GFxValue clip;
		RE::GFxValue slider;
	};

	bool GetRaceMenuPanel(RE::GFxMovieView& movie, RE::GFxValue& panel)
	{
		constexpr std::array<const char*, 2> panelPaths = {
		    "_root.RaceSexMenuBaseInstance.RaceSexPanelsInstance",
		    "_root.RaceSexPanelsInstance"
		};
		for (const char* path : panelPaths) {
			if (movie.GetVariable(&panel, path) &&
			    (panel.IsObject() || panel.IsDisplayObject())) {
				return true;
			}
		}
		return false;
	}

	// FxDelegateArgs contains only the semantic parameters. The response ID is
	// separate, exactly as FxDelegate::Callback strips it off GameDelegate calls.
	bool InvokeNativeMenuCallback(RE::IMenu& menu, RE::GFxMovieView& movie,
	    const char* method, const RE::GFxValue* values, std::uint32_t count)
	{
		if (menu.uiMovie.get() != &movie || !menu.fxDelegate)
			return false;
		const auto* entry = menu.fxDelegate->callbacks.GetAlt(method);
		if (!entry || !entry->handler || !entry->callback)
			return false;
		// Keep the owner alive if the callback closes its menu synchronously.
		auto owner = entry->handler;
		const auto callback = entry->callback;
		RE::GFxValue responseID;
		responseID.SetNumber(0.0);
		RE::FxDelegateArgs args(responseID, owner.get(), &movie, values, count);
		callback(args);
		return true;
	}

	ocu::RaceMenuKeyboardRecovery g_raceKeyboardRecovery;
	// Only our semantic laser click sets this; native controller confirmations
	// continue through the game's callback without any recovery being armed.
	thread_local bool g_laserMessageBoxDispatch = false;
	using ConfirmAndNameRun = void (*)(RE::ConfirmAndNameCallback*, RE::IMessageBoxCallback::Message);
	ConfirmAndNameRun g_originalConfirmAndNameRun = nullptr;
	thread_local RE::FxDelegateHandler::CallbackFn* g_observedRaceKeyboardCallback = nullptr;
	thread_local bool g_raceNativeKeyboardRequested = false;

	void ObserveRaceNativeKeyboard(const RE::FxDelegateArgs& args)
	{
		g_raceNativeKeyboardRequested = true;
		g_raceKeyboardRecovery.NativeRequest(reinterpret_cast<std::uintptr_t>(args.GetMovie()));
		if (auto* callback = g_observedRaceKeyboardCallback)
			callback(args);
	}

	void HookedConfirmAndName(RE::ConfirmAndNameCallback* self, RE::IMessageBoxCallback::Message message)
	{
		auto* ui = RE::UI::GetSingleton();
		auto menu = ui ? ui->GetMenu("RaceSex Menu") : nullptr;
		if (!g_laserMessageBoxDispatch || message != RE::IMessageBoxCallback::Message::kUnk0 ||
		    !menu || menu.get() != self->menu || !menu->uiMovie || !menu->fxDelegate) {
			g_originalConfirmAndNameRun(self, message);
			return;
		}
		auto movie = menu->uiMovie;
		auto delegate = menu->fxDelegate;
		g_raceKeyboardRecovery.ConfirmAccepted(
		    reinterpret_cast<std::uintptr_t>(movie.get()), GetTickCount64());

		// The native VR route invokes ShowVirtualKeyboard directly, bypassing
		// BSVirtualKeyboardDevice::Start. Observe that actual callback while the
		// confirmation runs. This can merely arm a deferred Accept-release:
		// observing it must not consume the keyboard recovery state.
		auto* keyboard = delegate->callbacks.GetAlt("ShowVirtualKeyboard");
		auto* originalKeyboard = keyboard ? keyboard->callback : nullptr;
		auto* previousObserver = g_observedRaceKeyboardCallback;
		g_observedRaceKeyboardCallback = originalKeyboard;
		g_raceNativeKeyboardRequested = false;
		if (keyboard && originalKeyboard)
			keyboard->callback = ObserveRaceNativeKeyboard;
		const bool onStack = menu->OnStack();
		g_originalConfirmAndNameRun(self, message);
		// Re-resolve after the callback: registering an AS handler may rehash it.
		keyboard = delegate->callbacks.GetAlt("ShowVirtualKeyboard");
		if (keyboard && keyboard->callback == ObserveRaceNativeKeyboard)
			keyboard->callback = originalKeyboard;
		g_observedRaceKeyboardCallback = previousObserver;
		SKSE::log::info("RACEMENU laser naming confirmed onStack={} nativeKeyboardRequested={}",
		    onStack, g_raceNativeKeyboardRequested);
	}

	void RecoverRaceMenuKeyboard(RE::UI& ui, bool canInspect)
	{
		// A modal can temporarily change the native stack flags. The open/close
		// event lifetime is authoritative for discarding an accepted request.
		if (!g_activeTrackedMenus.contains("RaceSex Menu")) {
			g_raceKeyboardRecovery = {};
			return;
		}
		// Do not inspect a movie behind another modal, MapMenu, or StatsMenu.
		if (!canInspect || ui.IsMenuOpen("MessageBoxMenu"))
			return;
		auto menu = ui.GetMenu<RE::RaceSexMenu>();
		if (!menu || !menu->uiMovie)
			return;
		auto movie = menu->uiMovie;
		RE::GFxValue panel, textEntry, enabled;
		const bool desktopNaming = GetRaceMenuPanel(*movie, panel) &&
		    panel.GetMember("textEntry", &textEntry) &&
		    (textEntry.IsObject() || textEntry.IsDisplayObject()) &&
		    textEntry.GetMember("enabled", &enabled) && enabled.IsBool() &&
		    enabled.GetBool() && DisplayObjectIsUsable(textEntry);
		bool keyboardBusy;
		{
			std::lock_guard<std::mutex> lock(g_callbackMutex);
			keyboardBusy = g_waitingForKeyboard;
		}
		keyboardBusy = keyboardBusy || (g_gameHwnd &&
		    GetPropW(g_gameHwnd, L"OC_KB_ACTIVE") != nullptr);
		if (!g_raceKeyboardRecovery.Observe(reinterpret_cast<std::uintptr_t>(movie.get()),
		        desktopNaming, keyboardBusy, g_pTransform->laserTriggerHeld != 0,
		        GetTickCount64()))
			return;

		// In Skyrim VR, ShowVirtualKeyboard arms a flag. RaceSexMenu's
		// ProcessButton opens the overlay only when it sees Accept released.
		// The laser's atomic MessageBox click consumes that physical gesture,
		// leaving the flag armed indefinitely. Finish the native handoff once,
		// after physical release and with the confirmation modal gone.
		// If the normal native release already ran, the flag is clear and this
		// targeted release is a no-op, including while its overlay is queued.
		const bool nativeRequestObserved = g_raceKeyboardRecovery.NativeRequestObserved();
		const bool dispatched = nativeRequestObserved || InvokeNativeMenuCallback(
		    *menu, *movie, "ShowVirtualKeyboard", nullptr, 0);
		auto* events = RE::UserEvents::GetSingleton();
		auto* handler = menu->AsMenuEventHandler();
		bool nativeHandlerAccepted = false;
		if (dispatched && events && handler) {
			// Deliver directly to this naming handler. No global input queue,
			// mouse coordinates, controller press, or click on the editor.
			auto* release = RE::ButtonEvent::Create(
			    RE::INPUT_DEVICE::kKeyboard, events->accept, 0, 0.0f, 0.01f);
			if (release) {
				// CommonLib's ordinary virtual call targets flat slot 5, a
				// no-op in VR. Use the actual VR button-handler slot (8).
				nativeHandlerAccepted = ocu::DispatchRaceMenuButton(handler, release);
				// Create uses Skyrim's allocator; release its string before free.
				release->SetUserEvent(RE::BSFixedString());
				RE::free(release);
			}
		}
		SKSE::log::info("RACEMENU naming release v4 desktopNaming={} nativeRequestObserved={} nativeCallback={} nativeHandlerAccepted={}",
		    desktopNaming, nativeRequestObserved, dispatched, nativeHandlerAccepted);
	}

	bool GetRaceMenuButtonPanelTarget(RE::GFxMovieView& movie,
	    RE::GFxValue& buttonPanel, float rootX,
	    float rootY, int indexBase, RaceMenuLaserTarget& target,
	    std::uint32_t maxButtons = 16)
	{
		if ((!buttonPanel.IsObject() && !buttonPanel.IsDisplayObject()) ||
		    !DisplayObjectIsUsable(buttonPanel)) {
			return false;
		}

		RE::GFxValue buttons;
		if (!buttonPanel.GetMember("buttons", &buttons) || !buttons.IsArray())
			return false;

		const auto count = std::min<std::uint32_t>(buttons.GetArraySize(), maxButtons);
		for (std::uint32_t i = 0; i < count; ++i) {
			RE::GFxValue button;
			if (!buttons.GetElement(i, &button) || !DisplayObjectIsUsable(button))
				continue;
			// RaceMenu VR's dynamically-created buttons often have visible bounds but
			// no dependable AS2 hitTest area. Accept either source so the final
			// character-name Accept/Cancel buttons can be targeted by the VR laser.
			if (DisplayObjectGeometryHitAtRootPoint(
			        movie, button, rootX, rootY)) {
				target.kind = RaceMenuLaserTargetKind::kButton;
				target.index = indexBase + static_cast<int>(i);
				target.owner = buttonPanel;
				target.clip = button;
				return true;
			}
		}
		return false;
	}

	bool RaceMenuObjectIsActive(RE::GFxValue& object)
	{
		if ((!object.IsObject() && !object.IsDisplayObject()) ||
		    !DisplayObjectIsUsable(object)) {
			return false;
		}
		RE::GFxValue enabled;
		return !object.GetMember("enabled", &enabled) || !enabled.IsBool() ||
		    enabled.GetBool();
	}

	bool GetRaceMenuListTarget(RE::GFxMovieView& movie, RE::GFxValue& list,
	    float rootX, float rootY,
	    bool permitSliders, RaceMenuLaserTargetKind rowKind,
	    RaceMenuLaserTarget& target)
	{
		auto memberHit = [&](RE::GFxValue& object,
		                     std::initializer_list<const char*> memberNames) {
			for (const char* memberName : memberNames) {
				RE::GFxValue member;
				if (object.GetMember(memberName, &member) &&
				    (member.IsObject() || member.IsDisplayObject()) &&
				    DisplayObjectIsUsable(member) &&
				    DisplayObjectGeometryHitAtRootPoint(
				        movie, member, rootX, rootY)) {
					return true;
				}
			}
			return false;
		};

		// RaceMenu's AS2 mouse singleton is mapped incorrectly in VR, so its
		// vertical list scrollbar needs the same semantic treatment as its row
		// sliders. Resolve the published list scrollbar before the list rows.
		RE::GFxValue scrollBar;
		if (GetListScrollBar(list, scrollBar) && DisplayObjectIsUsable(scrollBar) &&
		    DisplayObjectGeometryHitAtRootPoint(
		        movie, scrollBar, rootX, rootY)) {
			target.kind = RaceMenuLaserTargetKind::kScrollBar;
			target.owner = list;
			target.clip = scrollBar;
			target.slider = scrollBar;
			return true;
		}

		for (int clipIndex = 0; clipIndex < 40; ++clipIndex) {
			RE::GFxValue clipArg;
			clipArg.SetNumber(static_cast<double>(clipIndex));
			RE::GFxValue clip;
			if (!list.Invoke("getClipByIndex", &clip, &clipArg, 1) ||
			    (!clip.IsObject() && !clip.IsDisplayObject())) {
				continue;
			}

			RE::GFxValue visible;
			if (clip.GetMember("_visible", &visible) && visible.IsBool() &&
			    !visible.GetBool()) {
				continue;
			}
			RE::GFxValue itemIndex;
			int resolvedItemIndex = -1;
			if (clip.GetMember("itemIndex", &itemIndex) && itemIndex.IsNumber() &&
			    itemIndex.GetNumber() >= 0.0) {
				resolvedItemIndex = static_cast<int>(itemIndex.GetNumber());
			} else if (rowKind == RaceMenuLaserTargetKind::kCategory) {
				// TextCategoryList assigns clips and entries one-to-one. Some VR GFx
				// builds do not expose the dynamic itemIndex member back to native code.
				resolvedItemIndex = clipIndex;
			} else {
				continue;
			}

			// SliderListEntry places color, glow, and active-state actions beside
			// the row trigger. Their own AS2 handlers distinguish ordinary color
			// selection from the auxiliary glow action.
			constexpr std::array<const char*, 3> rowActionNames = {
			    "colorSquare", "glowSquare", "activeIndicator"
			};
			for (const char* actionName : rowActionNames) {
				RE::GFxValue action;
				if (clip.GetMember(actionName, &action) &&
				    RaceMenuObjectIsActive(action) &&
				    action.HasMember("onPress") &&
				    DisplayObjectHitAtRootPoint(action, rootX, rootY)) {
					target.kind = RaceMenuLaserTargetKind::kClipAction;
					target.index = resolvedItemIndex;
					target.owner = list;
					target.clip = action;
					return true;
				}
			}

			if (permitSliders) {
				RE::GFxValue slider;
				if (clip.GetMember("SliderInstance", &slider) &&
				    (slider.IsObject() || slider.IsDisplayObject())) {
					RE::GFxValue sliderVisible;
					RE::GFxValue sliderDisabled;
					const bool isVisible = !slider.GetMember("_visible", &sliderVisible) ||
					    !sliderVisible.IsBool() || sliderVisible.GetBool();
					const bool isDisabled = slider.GetMember("disabled", &sliderDisabled) &&
					    sliderDisabled.IsBool() && sliderDisabled.GetBool();
					// RaceMenuSlider is a container. In the installed 0.4.20 SWF its
					// container has no reliable hit area in VR, while its track/thumb do.
					// BrushListEntry also puts a transparent row trigger over the property,
					// so treat any hit on that slider row as an intentional slider drag.
					const bool sliderHit = DisplayObjectGeometryHitAtRootPoint(
					    movie, slider, rootX, rootY) ||
					    memberHit(slider, { "track", "thumb" });
					const bool sliderRowHit = memberHit(clip,
					    { "trigger", "textField", "valueField", "selectIndicator",
					        "focusIndicator" }) ||
					    DisplayObjectGeometryHitAtRootPoint(
					        movie, clip, rootX, rootY);
					if (isVisible && !isDisabled && (sliderHit || sliderRowHit)) {
						target.kind = RaceMenuLaserTargetKind::kSlider;
						target.index = resolvedItemIndex;
						target.owner = list;
						target.clip = clip;
						target.slider = slider;
						return true;
					}
				}
			}

			// TextCategoryListEntry, used by Sculpt's Smooth/Deflate/Move strip,
			// deliberately has no `trigger`. Its actual hit geometry is the resized
			// background/text pair, not the otherwise empty container MovieClip.
			const bool rowHit = memberHit(clip,
			    { "trigger", "background", "textField", "valueField" }) ||
			    DisplayObjectGeometryHitAtRootPoint(movie, clip, rootX, rootY);
			if (rowHit) {
				target.kind = rowKind;
				target.index = resolvedItemIndex;
				target.owner = list;
				target.clip = clip;
				return true;
			}
		}
		return false;
	}

	bool GetRaceMenuSculptCategoryTarget(RE::GFxMovieView& movie,
	    RE::GFxValue& list, float rootX, float rootY,
	    RaceMenuLaserTarget& target)
	{
		// TextCategoryListEntry has no trigger in the installed RaceMenu SWF. Its
		// background and label are the authoritative hit rectangles. Do not use the
		// container clip here because its tweened bounds can span other Sculpt panes.
		for (int clipIndex = 0; clipIndex < 12; ++clipIndex) {
			RE::GFxValue arg;
			arg.SetNumber(static_cast<double>(clipIndex));
			RE::GFxValue clip;
			if (!list.Invoke("getClipByIndex", &clip, &arg, 1) ||
			    (!clip.IsObject() && !clip.IsDisplayObject())) {
				continue;
			}

			RE::GFxValue visible;
			if (clip.GetMember("_visible", &visible) && visible.IsBool() &&
			    !visible.GetBool()) {
				continue;
			}
			RE::GFxValue itemIndexValue;
			const int itemIndex = clip.GetMember("itemIndex", &itemIndexValue) &&
			        itemIndexValue.IsNumber() && itemIndexValue.GetNumber() >= 0.0 ?
			    static_cast<int>(itemIndexValue.GetNumber()) : clipIndex;

			constexpr std::array<const char*, 2> hitMembers = {
			    "background", "textField"
			};
			for (const char* memberName : hitMembers) {
				RE::GFxValue member;
				if (clip.GetMember(memberName, &member) &&
				    (member.IsObject() || member.IsDisplayObject()) &&
				    DisplayObjectBoundsHitAtRootPoint(
				        movie, member, rootX, rootY)) {
					target.kind = RaceMenuLaserTargetKind::kCategory;
					target.index = itemIndex;
					target.owner = list;
					target.clip = clip;
					return true;
				}
			}
		}
		return false;
	}

	bool GetRaceMenuSculptBrushTarget(RE::GFxMovieView& movie,
	    RE::GFxValue& list, float rootX, float rootY,
	    RaceMenuLaserTarget& target)
	{
		RE::GFxValue scrollBar;
		if (GetListScrollBar(list, scrollBar) && DisplayObjectIsUsable(scrollBar) &&
		    DisplayObjectGeometryHitAtRootPoint(
		        movie, scrollBar, rootX, rootY)) {
			target.kind = RaceMenuLaserTargetKind::kScrollBar;
			target.owner = list;
			target.clip = scrollBar;
			target.slider = scrollBar;
			return true;
		}

		// BrushListEntry rows are all sliders. Only their real slider geometry may
		// claim a hit. A whole-row/container fallback is what allowed Sculpt input to
		// mutate the ordinary face sliders behind the editor.
		for (int clipIndex = 0; clipIndex < 40; ++clipIndex) {
			RE::GFxValue arg;
			arg.SetNumber(static_cast<double>(clipIndex));
			RE::GFxValue clip;
			if (!list.Invoke("getClipByIndex", &clip, &arg, 1) ||
			    (!clip.IsObject() && !clip.IsDisplayObject())) {
				continue;
			}
			RE::GFxValue itemIndexValue;
			if (!clip.GetMember("itemIndex", &itemIndexValue) ||
			    !itemIndexValue.IsNumber() || itemIndexValue.GetNumber() < 0.0) {
				continue;
			}

			RE::GFxValue slider;
			if (!clip.GetMember("SliderInstance", &slider) ||
			    (!slider.IsObject() && !slider.IsDisplayObject())) {
				continue;
			}
			RE::GFxValue sliderVisible;
			RE::GFxValue sliderDisabled;
			const bool isVisible = !slider.GetMember("_visible", &sliderVisible) ||
			    !sliderVisible.IsBool() || sliderVisible.GetBool();
			const bool isDisabled = slider.GetMember("disabled", &sliderDisabled) &&
			    sliderDisabled.IsBool() && sliderDisabled.GetBool();
			if (!isVisible || isDisabled)
				continue;

			bool hit = DisplayObjectBoundsHitAtRootPoint(
			    movie, slider, rootX, rootY);
			constexpr std::array<const char*, 2> sliderMembers = { "track", "thumb" };
			for (const char* memberName : sliderMembers) {
				RE::GFxValue member;
				if (!hit && slider.GetMember(memberName, &member) &&
				    (member.IsObject() || member.IsDisplayObject())) {
					hit = DisplayObjectBoundsHitAtRootPoint(
					    movie, member, rootX, rootY);
				}
			}
			if (hit) {
				target.kind = RaceMenuLaserTargetKind::kSlider;
				target.index = static_cast<int>(itemIndexValue.GetNumber());
				target.owner = list;
				target.clip = clip;
				target.slider = slider;
				return true;
			}
		}
		return false;
	}

	bool GetRaceMenuMeshListTarget(RE::GFxMovieView& movie,
	    RE::GFxValue& list, float rootX, float rootY,
	    RaceMenuLaserTarget& target, bool permitRows = true)
	{
		RE::GFxValue scrollBar;
		if (GetListScrollBar(list, scrollBar) && DisplayObjectIsUsable(scrollBar) &&
		    DisplayObjectGeometryHitAtRootPoint(
		        movie, scrollBar, rootX, rootY)) {
			target.kind = RaceMenuLaserTargetKind::kScrollBar;
			target.owner = list;
			target.clip = scrollBar;
			target.slider = scrollBar;
			return true;
		}

		// MeshListEntry exposes four independent AS2 controls inside each row.
		// Invoking their own handlers preserves RaceMenu's visibility, wireframe,
		// lock, and wire-color behavior, including the status text callbacks.
		constexpr std::array<const char*, 4> controlNames = {
		    "visibleToggle", "wireToggle", "lockToggle", "wireColor"
		};
		for (int clipIndex = 0; clipIndex < 40; ++clipIndex) {
			RE::GFxValue clipArg;
			clipArg.SetNumber(static_cast<double>(clipIndex));
			RE::GFxValue clip;
			if (!list.Invoke("getClipByIndex", &clip, &clipArg, 1) ||
			    (!clip.IsObject() && !clip.IsDisplayObject()) ||
			    !DisplayObjectIsUsable(clip)) {
				continue;
			}

			RE::GFxValue itemIndex;
			if (!clip.GetMember("itemIndex", &itemIndex) || !itemIndex.IsNumber() ||
			    itemIndex.GetNumber() < 0.0) {
				continue;
			}
			for (const char* controlName : controlNames) {
				RE::GFxValue control;
				if (!clip.GetMember(controlName, &control) ||
				    !RaceMenuObjectIsActive(control)) {
					continue;
				}
				if (DisplayObjectGeometryHitAtRootPoint(
				        movie, control, rootX, rootY)) {
					target.kind = RaceMenuLaserTargetKind::kClipAction;
					target.index = static_cast<int>(itemIndex.GetNumber());
					target.owner = list;
					target.clip = control;
					return true;
				}
			}

			if (!permitRows)
				continue;

			// The transparent trigger's published bounds are oversized in the VR SWF
			// and overlap the 768x768 head canvas. The visible row label is stable and
			// provides the intended selection target without stealing paint strokes.
			RE::GFxValue textField;
			if (clip.GetMember("textField", &textField) &&
			    (textField.IsObject() || textField.IsDisplayObject()) &&
			    DisplayObjectBoundsHitAtRootPoint(
			        movie, textField, rootX, rootY)) {
				target.kind = RaceMenuLaserTargetKind::kItem;
				target.index = static_cast<int>(itemIndex.GetNumber());
				target.owner = list;
				target.clip = clip;
				return true;
			}
		}
		return false;
	}

	// RaceMenu VR ships a square 1024x1024 AS2 movie inside Skyrim's 2048x2048
	// render target. Its list rows and CLIK sliders consume semantic callbacks;
	// feeding the viewport coordinates to NotifyMouseState leaves its AS2 Mouse
	// singleton pinned to an edge. Resolve the installed movie's real controls in
	// root coordinates and call the same callbacks its SWF wires to a PC mouse.
	void LogRaceMenuSculptState(RE::GFxMovieView& movie,
	    RE::GFxValue& vertexEditor)
	{
		// Target resolution runs every frame. Probe the AS2 object graph at most
		// four times per second so diagnostics cannot become another menu bottleneck.
		static ULONGLONG lastProbeTick = 0;
		const ULONGLONG now = GetTickCount64();
		if (now - lastProbeTick < 250)
			return;
		lastProbeTick = now;

		auto boolMember = [](RE::GFxValue& object, const char* name,
		                      bool fallback) {
			RE::GFxValue value;
			return object.GetMember(name, &value) && value.IsBool() ?
			    value.GetBool() : fallback;
		};
		auto numberMember = [](RE::GFxValue& object, const char* name,
		                        double fallback) {
			RE::GFxValue value;
			return object.GetMember(name, &value) && value.IsNumber() ?
			    value.GetNumber() : fallback;
		};

		RE::GFxValue charGen;
		const bool hasCharGen = movie.GetVariable(
		    &charGen, "_global.skse.plugins.CharGen") &&
		    (charGen.IsObject() || charGen.IsDisplayObject());
		const bool hasCreateMorph = hasCharGen && charGen.HasMember("CreateMorphEditor");
		const bool hasPaintMorph = hasCharGen && charGen.HasMember("BeginPaintMesh") &&
		    charGen.HasMember("DoPaintMesh") && charGen.HasMember("EndPaintMesh");

		RE::GFxValue brushWindow;
		const bool hasBrushWindow = vertexEditor.GetMember("brushWindow", &brushWindow) &&
		    (brushWindow.IsObject() || brushWindow.IsDisplayObject());
		const bool brushLoaded = hasBrushWindow &&
		    boolMember(brushWindow, "bLoadedAssets", false);
		RE::GFxValue brushes;
		const int brushCount = hasBrushWindow && brushWindow.GetMember("brushes", &brushes) &&
		    brushes.IsArray() ? static_cast<int>(brushes.GetArraySize()) : -1;

		RE::GFxValue wireframeDisplay;
		RE::GFxValue foreground;
		const bool hasDisplay = vertexEditor.GetMember(
		    "wireframeDisplay", &wireframeDisplay) &&
		    (wireframeDisplay.IsObject() || wireframeDisplay.IsDisplayObject());
		const bool hasForeground = hasDisplay && wireframeDisplay.GetMember(
		    "foreground", &foreground) &&
		    (foreground.IsObject() || foreground.IsDisplayObject());
		const bool wireLoaded = hasDisplay &&
		    boolMember(wireframeDisplay, "bLoadedAssets", false);
		const bool inputDisabled = hasDisplay &&
		    boolMember(wireframeDisplay, "disableInput", false);

		RE::GFxValue editorData;
		const bool hasEditorData = hasDisplay && wireframeDisplay.GetMember(
		    "editorData", &editorData) &&
		    (editorData.IsObject() || editorData.IsDisplayObject());
		const int editorWidth = hasEditorData ?
		    static_cast<int>(numberMember(editorData, "width", -1.0)) : -1;
		const int editorHeight = hasEditorData ?
		    static_cast<int>(numberMember(editorData, "height", -1.0)) : -1;
		const int fixedWidth = hasForeground ?
		    static_cast<int>(numberMember(foreground, "fixedWidth", -1.0)) : -1;
		const int fixedHeight = hasForeground ?
		    static_cast<int>(numberMember(foreground, "fixedHeight", -1.0)) : -1;
		RE::GFxValue wireframe;
		const bool hasWireframe = hasForeground && foreground.GetMember(
		    "wireframe", &wireframe) &&
		    (wireframe.IsObject() || wireframe.IsDisplayObject());
		const int imageWidth = hasWireframe ?
		    static_cast<int>(numberMember(wireframe, "_width", -1.0)) : -1;
		const int imageHeight = hasWireframe ?
		    static_cast<int>(numberMember(wireframe, "_height", -1.0)) : -1;

		const int stateBits = (hasCharGen ? 1 : 0) | (hasCreateMorph ? 2 : 0) |
		    (hasPaintMorph ? 4 : 0) | (brushLoaded ? 8 : 0) |
		    (wireLoaded ? 16 : 0) | (inputDisabled ? 32 : 0) |
		    (hasEditorData ? 64 : 0) | (hasWireframe ? 128 : 0);
		static int lastStateBits = -1;
		static int lastBrushCount = -2;
		static int lastEditorWidth = -2;
		static int lastEditorHeight = -2;
		static int lastFixedWidth = -2;
		static int lastFixedHeight = -2;
		static int lastImageWidth = -2;
		static int lastImageHeight = -2;
		static ULONGLONG lastLogTick = 0;
		const bool changed = stateBits != lastStateBits || brushCount != lastBrushCount ||
		    editorWidth != lastEditorWidth || editorHeight != lastEditorHeight ||
		    fixedWidth != lastFixedWidth || fixedHeight != lastFixedHeight ||
		    imageWidth != lastImageWidth || imageHeight != lastImageHeight;
		if (!changed && now - lastLogTick < 15000)
			return;

		lastStateBits = stateBits;
		lastBrushCount = brushCount;
		lastEditorWidth = editorWidth;
		lastEditorHeight = editorHeight;
		lastFixedWidth = fixedWidth;
		lastFixedHeight = fixedHeight;
		lastImageWidth = imageWidth;
		lastImageHeight = imageHeight;
		lastLogTick = now;
		const bool imageReady = hasEditorData && hasWireframe && fixedWidth > 0 &&
		    fixedHeight > 0 && imageWidth > 0 && imageHeight > 0;
		if (imageReady) {
			SKSE::log::info(
			    "RACEMENU SCULPT ready charGen={} create={} paint={} brushes={}/{} wireLoaded={} disabled={} editor={}x{} fixed={}x{} image={}x{}",
			    hasCharGen, hasCreateMorph, hasPaintMorph, brushLoaded, brushCount,
			    wireLoaded, inputDisabled, editorWidth, editorHeight, fixedWidth,
			    fixedHeight, imageWidth, imageHeight);
		} else {
			SKSE::log::warn(
			    "RACEMENU SCULPT incomplete charGen={} create={} paint={} brushes={}/{} wireLoaded={} disabled={} editor={}x{} fixed={}x{} image={}x{}",
			    hasCharGen, hasCreateMorph, hasPaintMorph, brushLoaded, brushCount,
			    wireLoaded, inputDisabled, editorWidth, editorHeight, fixedWidth,
			    fixedHeight, imageWidth, imageHeight);
		}

		if (hasWireframe) {
			double xMin = 0.0;
			double xMax = 0.0;
			double yMin = 0.0;
			double yMax = 0.0;
			const bool hasBounds = GetDisplayObjectRootBounds(
			    movie, wireframe, xMin, xMax, yMin, yMax);
			SKSE::log::info(
			    "RACEMENU SCULPT canvas bounds={} root=({:.1f},{:.1f})..({:.1f},{:.1f})",
			    hasBounds, xMin, yMin, xMax, yMax);
		}

		RE::GFxValue categoryList;
		RE::GFxValue categoryEntries;
		const bool hasCategoryList = hasBrushWindow &&
		    brushWindow.GetMember("categoryList", &categoryList) &&
		    (categoryList.IsObject() || categoryList.IsDisplayObject());
		const int categoryCount = hasCategoryList &&
		        categoryList.GetMember("entryList", &categoryEntries) &&
		        categoryEntries.IsArray() ?
		    static_cast<int>(categoryEntries.GetArraySize()) : -1;
		SKSE::log::info(
		    "RACEMENU SCULPT categories list={} entries={}",
		    hasCategoryList, categoryCount);
		if (hasCategoryList) {
			const int clipsToLog = categoryCount < 6 ? categoryCount : 6;
			for (int clipIndex = 0; clipIndex < clipsToLog; ++clipIndex) {
				RE::GFxValue arg;
				arg.SetNumber(static_cast<double>(clipIndex));
				RE::GFxValue clip;
				if (!categoryList.Invoke("getClipByIndex", &clip, &arg, 1) ||
				    (!clip.IsObject() && !clip.IsDisplayObject())) {
					SKSE::log::warn(
					    "RACEMENU SCULPT category clip={} unavailable", clipIndex);
					continue;
				}
				RE::GFxValue item;
				const int itemIndex = clip.GetMember("itemIndex", &item) &&
				        item.IsNumber() ?
				    static_cast<int>(item.GetNumber()) : -1;
				RE::GFxValue background;
				RE::GFxValue* boundsClip = &clip;
				if (clip.GetMember("background", &background) &&
				    (background.IsObject() || background.IsDisplayObject())) {
					boundsClip = &background;
				}
				double xMin = 0.0;
				double xMax = 0.0;
				double yMin = 0.0;
				double yMax = 0.0;
				const bool hasBounds = GetDisplayObjectRootBounds(
				    movie, *boundsClip, xMin, xMax, yMin, yMax);
				SKSE::log::info(
				    "RACEMENU SCULPT category clip={} item={} bounds={} root=({:.1f},{:.1f})..({:.1f},{:.1f})",
				    clipIndex, itemIndex, hasBounds, xMin, yMin, xMax, yMax);
			}
		}
	}

	bool ResolveRaceMenuLaserTarget(RE::GFxMovieView& movie, float viewportX,
	    float viewportY, RaceMenuLaserTarget& target)
	{
		target = {};
		float rootX = 0.0f;
		float rootY = 0.0f;
		if (!ViewportToMovieRootPoint(movie, viewportX, viewportY, rootX, rootY))
			return false;

		// RaceMenu attaches both FileViewerDialog and ImportDialog directly to
		// _root as "dialog". Handle either modal before the editor so preset files,
		// head export/import, part matching, and every dialog button remain usable
		// without allowing a click to pass through to controls underneath it.
		RE::GFxValue dialog;
		if (movie.GetVariable(&dialog, "_root.dialog") &&
		    (dialog.IsObject() || dialog.IsDisplayObject()) &&
		    DisplayObjectIsUsable(dialog)) {
			constexpr std::array<const char*, 2> dialogListNames = {
			    "fileList", "importList"
			};
			for (const char* listName : dialogListNames) {
				RE::GFxValue list;
				if (dialog.GetMember(listName, &list) &&
				    (list.IsObject() || list.IsDisplayObject()) &&
				    DisplayObjectIsUsable(list) &&
				    GetRaceMenuListTarget(movie, list, rootX, rootY, false,
				        RaceMenuLaserTargetKind::kItem, target)) {
					return true;
				}
			}

			RE::GFxValue buttonPanel;
			if (dialog.GetMember("buttonPanel", &buttonPanel) &&
			    GetRaceMenuButtonPanelTarget(
			        movie, buttonPanel, rootX, rootY, 300, target)) {
				return true;
			}

			// The modal covers the main editor. Consume blank modal space as a real
			// RaceMenu hit so the generic NotifyMouseState fallback cannot click a
			// control underneath the dialog.
			target.kind = RaceMenuLaserTargetKind::kNone;
			target.owner = dialog;
			target.clip = dialog;
			return true;
		}

		RE::GFxValue panel;
		if (!GetRaceMenuPanel(movie, panel))
			return false;

		// Character naming is a modal TextEntryField layered over every RaceMenu
		// mode. Resolve it before mode-specific controls: the selected mode can be
		// stale while this field is visible. Store the TextEntryField itself as the
		// owner so activation can invoke its documented onAccept/onCancel methods.
		RE::GFxValue textEntry;
		if (panel.GetMember("textEntry", &textEntry) &&
		    (textEntry.IsObject() || textEntry.IsDisplayObject()) &&
		    DisplayObjectIsUsable(textEntry)) {
			RE::GFxValue buttonPanel;
			if (textEntry.GetMember("buttonPanel", &buttonPanel) &&
			    GetRaceMenuButtonPanelTarget(
			        movie, buttonPanel, rootX, rootY, 500, target)) {
				target.owner = textEntry;
				return true;
			}
			target.kind = RaceMenuLaserTargetKind::kNone;
			target.owner = textEntry;
			target.clip = textEntry;
			return true;
		}

		// ModeSwitcher is authoritative. Read its already-published fields directly
		// instead of invoking AS2 while RaceMenu may still be initializing categories.
		// Sculpt's child windows are constructed with enabled=false and shown later
		// only through TweenLite autoAlpha, so their enabled property is not visibility.
		int selectedMode = -1;
		RE::GFxValue modeSelectState;
		if (panel.GetMember("modeSelect", &modeSelectState) &&
		    (modeSelectState.IsObject() || modeSelectState.IsDisplayObject())) {
			RE::GFxValue modes;
			RE::GFxValue buttonGroup;
			RE::GFxValue selectedButton;
			if (modeSelectState.GetMember("_modes", &modes) && modes.IsArray() &&
			    modeSelectState.GetMember("buttonGroup", &buttonGroup) &&
			    (buttonGroup.IsObject() || buttonGroup.IsDisplayObject()) &&
			    buttonGroup.GetMember("selectedButton", &selectedButton) &&
			    (selectedButton.IsObject() || selectedButton.IsDisplayObject())) {
				const auto count = std::min<std::uint32_t>(modes.GetArraySize(), 8);
				for (std::uint32_t i = 0; i < count; ++i) {
					RE::GFxValue mode;
					if (modes.GetElement(i, &mode) && mode == selectedButton) {
						selectedMode = static_cast<int>(i);
						break;
					}
				}
			}
		}
		const bool sculptModeSelected = selectedMode == 3;

		// Mode tabs must remain reachable while Sculpt consumes the rest of its
		// surface, otherwise the user cannot leave the tab with the laser.
		if (modeSelectState.IsObject() || modeSelectState.IsDisplayObject()) {
			RE::GFxValue modes;
			if (modeSelectState.GetMember("_modes", &modes) && modes.IsArray()) {
				const auto count = std::min<std::uint32_t>(modes.GetArraySize(), 8);
				for (std::uint32_t i = 0; i < count; ++i) {
					RE::GFxValue tab;
					if (modes.GetElement(i, &tab) &&
					    DisplayObjectHitAtRootPoint(tab, rootX, rootY)) {
						target.kind = RaceMenuLaserTargetKind::kModeTab;
						target.index = static_cast<int>(i);
						target.owner = modeSelectState;
						target.clip = tab;
						return true;
					}
				}
			}
		}

		// The color editor is a modal child layered over the main RaceMenu lists.
		// Its HSV/alpha sliders and dynamically-created mapped buttons are not
		// members of itemList, so resolve them first and never click through the
		// visible field into the list beneath it.
		RE::GFxValue colorField;
		if (!sculptModeSelected && panel.GetMember("colorField", &colorField) &&
		    (colorField.IsObject() || colorField.IsDisplayObject())) {
			RE::GFxValue visible;
			const bool colorFieldVisible =
			    !colorField.GetMember("_visible", &visible) || !visible.IsBool() ||
			    visible.GetBool();
			if (colorFieldVisible) {
				RE::GFxValue selector;
				if (colorField.GetMember("colorSelector", &selector) &&
				    (selector.IsObject() || selector.IsDisplayObject())) {
					constexpr std::array<const char*, 4> sliderNames = {
					    "hSlider", "sSlider", "vSlider", "aSlider"
					};
					for (std::size_t i = 0; i < sliderNames.size(); ++i) {
						RE::GFxValue slider;
						if (!selector.GetMember(sliderNames[i], &slider) ||
						    (!slider.IsObject() && !slider.IsDisplayObject())) {
							continue;
						}
						RE::GFxValue sliderVisible;
						RE::GFxValue sliderDisabled;
						const bool isVisible =
						    !slider.GetMember("_visible", &sliderVisible) ||
						    !sliderVisible.IsBool() || sliderVisible.GetBool();
						const bool isDisabled =
						    slider.GetMember("disabled", &sliderDisabled) &&
						    sliderDisabled.IsBool() && sliderDisabled.GetBool();
						if (isVisible && !isDisabled &&
						    DisplayObjectHitAtRootPoint(slider, rootX, rootY)) {
							target.kind = RaceMenuLaserTargetKind::kSlider;
							target.index = 100 + static_cast<int>(i);
							target.owner = colorField;
							target.clip = slider;
							target.slider = slider;
							return true;
						}
					}
				}

				constexpr std::array<const char*, 2> panelNames = {
				    "buttonPanel", "presetPanel"
				};
				for (std::size_t panelIndex = 0; panelIndex < panelNames.size(); ++panelIndex) {
					RE::GFxValue buttonPanel;
					if (!colorField.GetMember(panelNames[panelIndex], &buttonPanel) ||
					    (!buttonPanel.IsObject() && !buttonPanel.IsDisplayObject())) {
						continue;
					}
					if (GetRaceMenuButtonPanelTarget(movie, buttonPanel, rootX, rootY,
					        static_cast<int>(panelIndex * 100), target)) {
						return true;
					}
				}
				target.kind = RaceMenuLaserTargetKind::kNone;
				target.owner = colorField;
				target.clip = colorField;
				return true;
			}
		}

		// Texture/makeup selection and the character-name field are independent
		// modal overlays. Cover their lists and Accept/Cancel panels before any
		// editor or main RaceMenu control.
		RE::GFxValue makeupPanel;
		if (!sculptModeSelected && panel.GetMember("makeupPanel", &makeupPanel) &&
		    (makeupPanel.IsObject() || makeupPanel.IsDisplayObject()) &&
		    DisplayObjectIsUsable(makeupPanel)) {
			RE::GFxValue makeupList;
			if (makeupPanel.GetMember("makeupList", &makeupList) &&
			    (makeupList.IsObject() || makeupList.IsDisplayObject()) &&
			    DisplayObjectIsUsable(makeupList) &&
			    GetRaceMenuListTarget(movie, makeupList, rootX, rootY, false,
			        RaceMenuLaserTargetKind::kItem, target)) {
				return true;
			}
			RE::GFxValue buttonPanel;
			if (makeupPanel.GetMember("buttonPanel", &buttonPanel) &&
			    GetRaceMenuButtonPanelTarget(
			        movie, buttonPanel, rootX, rootY, 400, target)) {
				return true;
			}
			target.kind = RaceMenuLaserTargetKind::kNone;
			target.owner = makeupPanel;
			target.clip = makeupPanel;
			return true;
		}

		// Sculpt mode has three separate windows plus two bottom button panels.
		// The static panel is where Head Export, Head Import, and Clear Sculpt live.
		RE::GFxValue vertexEditor;
		if (panel.GetMember("vertexEditor", &vertexEditor) &&
		    (vertexEditor.IsObject() || vertexEditor.IsDisplayObject()) &&
		    (sculptModeSelected || RaceMenuObjectIsActive(vertexEditor))) {
			RE::GFxValue bottomBar;
			RE::GFxValue staticPanel;
			if (vertexEditor.GetMember("bottomBar", &bottomBar) &&
			    (bottomBar.IsObject() || bottomBar.IsDisplayObject()) &&
			    bottomBar.GetMember("staticPanel", &staticPanel) &&
			    GetRaceMenuButtonPanelTarget(
			        movie, staticPanel, rootX, rootY, 600, target)) {
				return true;
			}
			RE::GFxValue navPanel;
			if (vertexEditor.GetMember("navPanel", &navPanel) &&
			    GetRaceMenuButtonPanelTarget(
			        movie, navPanel, rootX, rootY, 620, target, 1)) {
				return true;
			}

			RE::GFxValue brushWindow;
			if (vertexEditor.GetMember("brushWindow", &brushWindow) &&
			    (brushWindow.IsObject() || brushWindow.IsDisplayObject()) &&
			    DisplayObjectIsUsable(brushWindow)) {
				RE::GFxValue categoryList;
				if (brushWindow.GetMember("categoryList", &categoryList) &&
				    (categoryList.IsObject() || categoryList.IsDisplayObject()) &&
				    GetRaceMenuSculptCategoryTarget(
				        movie, categoryList, rootX, rootY, target)) {
					return true;
				}
				RE::GFxValue brushList;
				if (brushWindow.GetMember("brushList", &brushList) &&
				    (brushList.IsObject() || brushList.IsDisplayObject()) &&
				    GetRaceMenuSculptBrushTarget(
				        movie, brushList, rootX, rootY, target)) {
					return true;
				}
			}

			// Resolve the four small mesh toggles before the canvas, but defer its
			// oversized row triggers until after the real 768x768 head image.
			RE::GFxValue meshWindow;
			RE::GFxValue meshList;
			if (vertexEditor.GetMember("meshWindow", &meshWindow) &&
			    (meshWindow.IsObject() || meshWindow.IsDisplayObject()) &&
			    DisplayObjectIsUsable(meshWindow) &&
			    meshWindow.GetMember("meshList", &meshList) &&
			    (meshList.IsObject() || meshList.IsDisplayObject()) &&
			    GetRaceMenuMeshListTarget(
			        movie, meshList, rootX, rootY, target, false)) {
				return true;
			}

			// WireframeDisplay's AS2 handlers read foreground._xmouse/_ymouse.
			// Those values are pinned by Skyrim VR's 1024-to-2048 viewport mapping,
			// so recognize the real head image here and drive CharGen with explicit
			// foreground-local coordinates in the cursor pump below.
			RE::GFxValue wireframeDisplay;
			RE::GFxValue foreground;
			if (vertexEditor.GetMember("wireframeDisplay", &wireframeDisplay) &&
			    (wireframeDisplay.IsObject() || wireframeDisplay.IsDisplayObject()) &&
			    DisplayObjectIsUsable(wireframeDisplay) &&
			    wireframeDisplay.GetMember("foreground", &foreground) &&
			    (foreground.IsObject() || foreground.IsDisplayObject()) &&
			    DisplayObjectIsUsable(foreground)) {
				RE::GFxValue loaded;
				RE::GFxValue disableInput;
				const bool assetsLoaded =
				    wireframeDisplay.GetMember("bLoadedAssets", &loaded) &&
				    loaded.IsBool() && loaded.GetBool();
				const bool inputDisabled =
				    wireframeDisplay.GetMember("disableInput", &disableInput) &&
				    disableInput.IsBool() && disableInput.GetBool();
				double localX = 0.0;
				double localY = 0.0;
				const bool canvasHit = RootPointToRaceMenuSculptPoint(
				    movie, foreground, rootX, rootY, localX, localY);
				if (assetsLoaded && !inputDisabled && canvasHit) {
					target.kind = RaceMenuLaserTargetKind::kSculptCanvas;
					target.owner = wireframeDisplay;
					target.clip = foreground;
					return true;
				}
			}

			// History and mesh row selection live outside the image. They are checked
			// after the canvas because the VR SWF reports oversized container bounds.
			RE::GFxValue historyWindow;
			RE::GFxValue historyList;
			if (vertexEditor.GetMember("historyWindow", &historyWindow) &&
			    (historyWindow.IsObject() || historyWindow.IsDisplayObject()) &&
			    DisplayObjectIsUsable(historyWindow) &&
			    historyWindow.GetMember("historyList", &historyList) &&
			    (historyList.IsObject() || historyList.IsDisplayObject()) &&
			    GetRaceMenuListTarget(movie, historyList, rootX, rootY, false,
			        RaceMenuLaserTargetKind::kItem, target)) {
				return true;
			}
			if ((meshList.IsObject() || meshList.IsDisplayObject()) &&
			    GetRaceMenuMeshListTarget(
			        movie, meshList, rootX, rootY, target, true)) {
				return true;
			}

			// Sculpt replaces the normal slider editor. Consuming its blank space is
			// essential: otherwise NotifyMouseState or the main itemList resolver clicks
			// the hidden face morph controls underneath this tab.
			target.kind = RaceMenuLaserTargetKind::kNone;
			target.owner = vertexEditor;
			target.clip = vertexEditor;
			return true;
		}

		// Camera and Presets each replace the main editor and publish their own
		// bottom navigation. Presets also owns a scrollable preview item list.
		RE::GFxValue cameraEditor;
		if (panel.GetMember("cameraEditor", &cameraEditor) &&
		    RaceMenuObjectIsActive(cameraEditor)) {
			RE::GFxValue navPanel;
			if (cameraEditor.GetMember("navPanel", &navPanel) &&
			    GetRaceMenuButtonPanelTarget(
			        movie, navPanel, rootX, rootY, 700, target, 1)) {
				return true;
			}
		}

		RE::GFxValue presetEditor;
		if (panel.GetMember("presetEditor", &presetEditor) &&
		    RaceMenuObjectIsActive(presetEditor)) {
			RE::GFxValue navPanel;
			if (presetEditor.GetMember("navPanel", &navPanel) &&
			    GetRaceMenuButtonPanelTarget(
			        movie, navPanel, rootX, rootY, 720, target)) {
				return true;
			}
			RE::GFxValue itemList;
			if (presetEditor.GetMember("itemList", &itemList) &&
			    (itemList.IsObject() || itemList.IsDisplayObject()) &&
			    GetRaceMenuListTarget(movie, itemList, rootX, rootY, false,
			        RaceMenuLaserTargetKind::kItem, target)) {
				return true;
			}
		}

		RE::GFxValue modeSelect;
		if (panel.GetMember("modeSelect", &modeSelect) &&
		    (modeSelect.IsObject() || modeSelect.IsDisplayObject())) {
			RE::GFxValue modes;
			if (modeSelect.GetMember("_modes", &modes) && modes.IsArray()) {
				const auto count = std::min<std::uint32_t>(modes.GetArraySize(), 8);
				for (std::uint32_t i = 0; i < count; ++i) {
					RE::GFxValue tab;
					if (modes.GetElement(i, &tab) &&
					    DisplayObjectHitAtRootPoint(tab, rootX, rootY)) {
						target.kind = RaceMenuLaserTargetKind::kModeTab;
						target.index = static_cast<int>(i);
						target.owner = modeSelect;
						target.clip = tab;
						return true;
					}
				}
			}
		}

		RE::GFxValue itemList;
		if (panel.GetMember("itemList", &itemList) &&
		    (itemList.IsObject() || itemList.IsDisplayObject()) &&
		    GetRaceMenuListTarget(movie, itemList, rootX, rootY, true,
		        RaceMenuLaserTargetKind::kItem, target)) {
			return true;
		}

		RE::GFxValue categoryList;
		if (panel.GetMember("categoryList", &categoryList) &&
		    (categoryList.IsObject() || categoryList.IsDisplayObject()) &&
		    GetRaceMenuListTarget(movie, categoryList, rootX, rootY, false,
		        RaceMenuLaserTargetKind::kCategory, target)) {
			return true;
		}

		// Sliders mode's dynamically-created bottom buttons include Done, Search,
		// Zoom, Light, Change Race, Choose Color, and Choose Texture.
		RE::GFxValue navPanel;
		if (panel.GetMember("navPanel", &navPanel) &&
		    GetRaceMenuButtonPanelTarget(
		        movie, navPanel, rootX, rootY, 800, target)) {
			return true;
		}

		RE::GFxValue categoryButtons;
		if (panel.GetMember("categoryButtons", &categoryButtons) &&
		    (categoryButtons.IsObject() || categoryButtons.IsDisplayObject())) {
			constexpr std::array<const char*, 2> categoryTriggerNames = {
			    "triggerLeft", "triggerRight"
			};
			for (const char* triggerName : categoryTriggerNames) {
				RE::GFxValue trigger;
				if (categoryButtons.GetMember(triggerName, &trigger) &&
				    RaceMenuObjectIsActive(trigger) && trigger.HasMember("onPress") &&
				    DisplayObjectHitAtRootPoint(trigger, rootX, rootY)) {
					target.kind = RaceMenuLaserTargetKind::kClipAction;
					target.owner = categoryButtons;
					target.clip = trigger;
					return true;
				}
			}
		}
		return false;
	}

	bool HoverRaceMenuLaserTarget(RaceMenuLaserTarget& target)
	{
		if (target.kind == RaceMenuLaserTargetKind::kButton) {
			RE::GFxValue controller;
			controller.SetNumber(0.0);
			return target.clip.Invoke(
			    "handleMouseRollOver", nullptr, &controller, 1);
		}
		if (target.kind == RaceMenuLaserTargetKind::kModeTab) {
			RE::GFxValue controller;
			controller.SetNumber(0.0);
			return target.clip.Invoke("handleMouseRollOver", nullptr, &controller, 1);
		}
		if (target.kind == RaceMenuLaserTargetKind::kClipAction)
			return target.clip.Invoke("onRollOver", nullptr, nullptr, 0);
		if (target.kind == RaceMenuLaserTargetKind::kCategory ||
		    target.kind == RaceMenuLaserTargetKind::kItem ||
		    target.kind == RaceMenuLaserTargetKind::kSlider) {
			RE::GFxValue index;
			index.SetNumber(static_cast<double>(target.index));
			return target.owner.Invoke("onItemRollOver", nullptr, &index, 1);
		}
		return false;
	}

	bool InvokeRaceMenuCharGen(RE::GFxMovieView& movie, const char* method,
	    RE::GFxValue* result, const RE::GFxValue* args, std::uint32_t argCount)
	{
		RE::GFxValue charGen;
		if (movie.GetVariable(&charGen, "_global.skse.plugins.CharGen") &&
		    (charGen.IsObject() || charGen.IsDisplayObject()) &&
		    charGen.Invoke(method, result, args, argCount)) {
			return true;
		}

		// Some Scaleform builds expose native plugin functions to Invoke but do
		// not return the intermediate _global object through GetVariable.
		std::string path = "_global.skse.plugins.CharGen.";
		path += method;
		return movie.Invoke(path.c_str(), result, args, argCount);
	}

	bool GetRaceMenuSculptPoint(RE::GFxMovieView& movie,
	    RE::GFxValue& foreground, float viewportX, float viewportY,
	    double& localX, double& localY)
	{
		float rootX = 0.0f;
		float rootY = 0.0f;
		if (!ViewportToMovieRootPoint(movie, viewportX, viewportY, rootX, rootY))
			return false;

		return RootPointToRaceMenuSculptPoint(
		    movie, foreground, rootX, rootY, localX, localY);
	}

	void DispatchRaceMenuSculptEvent(RE::GFxMovieView& movie,
	    RE::GFxValue& wireframeDisplay, const char* eventType)
	{
		RE::GFxValue event;
		movie.CreateObject(&event);
		RE::GFxValue type;
		type.SetString(eventType);
		event.SetMember("type", type);
		wireframeDisplay.Invoke("dispatchEvent", nullptr, &event, 1);
	}

	bool HoverRaceMenuSculptCanvas(RE::GFxMovieView& movie,
	    RE::GFxValue& foreground, float viewportX, float viewportY,
	    double* outLocalX = nullptr, double* outLocalY = nullptr)
	{
		double localX = 0.0;
		double localY = 0.0;
		if (!GetRaceMenuSculptPoint(
		        movie, foreground, viewportX, viewportY, localX, localY)) {
			return false;
		}
		std::array<RE::GFxValue, 2> args;
		args[0].SetNumber(localX);
		args[1].SetNumber(localY);
		if (!InvokeRaceMenuCharGen(movie, "DoHoverMesh", nullptr,
		        args.data(), static_cast<std::uint32_t>(args.size()))) {
			return false;
		}
		if (outLocalX)
			*outLocalX = localX;
		if (outLocalY)
			*outLocalY = localY;
		return true;
	}

	bool BeginRaceMenuSculptStroke(RE::GFxMovieView& movie,
	    RaceMenuLaserTarget& target, float viewportX, float viewportY,
	    double& localX, double& localY)
	{
		if (!HoverRaceMenuSculptCanvas(movie, target.clip, viewportX, viewportY,
		        &localX, &localY)) {
			return false;
		}

		std::array<RE::GFxValue, 2> args;
		args[0].SetNumber(localX);
		args[1].SetNumber(localY);
		RE::GFxValue began;
		if (!InvokeRaceMenuCharGen(movie, "BeginPaintMesh", &began,
		        args.data(), static_cast<std::uint32_t>(args.size())) ||
		    !began.IsBool() || !began.GetBool()) {
			return false;
		}

		RE::GFxValue painting;
		painting.SetBoolean(true);
		target.clip.SetMember("painting", painting);

		// Mirror WireframeDisplay.beginPaintMesh's handler state. OCU drives the
		// native CharGen calls with corrected VR coordinates, but the foreground
		// clip still needs to remain in the same painting state its ActionScript
		// expects until the physical trigger is released.
		RE::GFxValue paintHandler;
		if (target.clip.GetMember("doPaintMesh", &paintHandler))
			target.clip.SetMember("onMouseMove", paintHandler);
		RE::GFxValue releaseHandler;
		if (target.clip.GetMember("endPaintMesh", &releaseHandler)) {
			target.clip.SetMember("onRelease", releaseHandler);
			target.clip.SetMember("onReleaseOutside", releaseHandler);
		}
		DispatchRaceMenuSculptEvent(movie, target.owner, "beginPainting");
		return true;
	}

	bool ContinueRaceMenuSculptStroke(RE::GFxMovieView& movie,
	    RE::GFxValue& foreground, float viewportX, float viewportY,
	    double& localX, double& localY)
	{
		if (!HoverRaceMenuSculptCanvas(movie, foreground, viewportX, viewportY,
		        &localX, &localY)) {
			return false;
		}
		std::array<RE::GFxValue, 2> args;
		args[0].SetNumber(localX);
		args[1].SetNumber(localY);
		return InvokeRaceMenuCharGen(movie, "DoPaintMesh", nullptr,
		    args.data(), static_cast<std::uint32_t>(args.size()));
	}

	void EndRaceMenuSculptStroke(RE::GFxMovieView& movie,
	    RE::GFxValue& wireframeDisplay, RE::GFxValue& foreground)
	{
		RE::GFxValue painting;
		painting.SetBoolean(false);
		foreground.SetMember("painting", painting);
		RE::GFxValue hoverHandler;
		if (foreground.GetMember("doHoverMesh", &hoverHandler))
			foreground.SetMember("onMouseMove", hoverHandler);
		RE::GFxValue nullHandler;
		nullHandler.SetNull();
		foreground.SetMember("onRelease", nullHandler);
		foreground.SetMember("onReleaseOutside", nullHandler);
		DispatchRaceMenuSculptEvent(movie, wireframeDisplay, "endPainting");
		InvokeRaceMenuCharGen(movie, "EndPaintMesh", nullptr, nullptr, 0);
	}

	bool SetVerticalScrollBarAtViewportPoint(RE::GFxMovieView& movie,
	    RE::GFxValue& scrollBar, float viewportX, float viewportY)
	{
		float rootX = 0.0f;
		float rootY = 0.0f;
		if (!ViewportToMovieRootPoint(movie, viewportX, viewportY, rootX, rootY))
			return false;

		RE::GFxValue point;
		movie.CreateObject(&point);
		RE::GFxValue xValue;
		RE::GFxValue yValue;
		xValue.SetNumber(rootX);
		yValue.SetNumber(rootY);
		point.SetMember("x", xValue);
		point.SetMember("y", yValue);
		if (!scrollBar.Invoke("globalToLocal", nullptr, &point, 1) ||
		    !point.GetMember("y", &yValue) || !yValue.IsNumber()) {
			return false;
		}

		auto numberMember = [&](RE::GFxValue& object, const char* name,
		                        double fallback) {
			RE::GFxValue value;
			return object.GetMember(name, &value) && value.IsNumber() ?
			    value.GetNumber() : fallback;
		};
		const double minimum = numberMember(scrollBar, "minPosition",
		    numberMember(scrollBar, "minimum", 0.0));
		const double maximum = numberMember(scrollBar, "maxPosition",
		    numberMember(scrollBar, "maximum", 0.0));
		if (!std::isfinite(minimum) || !std::isfinite(maximum) || maximum <= minimum)
			return false;

		RE::GFxValue track;
		RE::GFxValue thumb;
		const bool hasTrack = scrollBar.GetMember("track", &track) &&
		    (track.IsObject() || track.IsDisplayObject());
		const bool hasThumb = scrollBar.GetMember("thumb", &thumb) &&
		    (thumb.IsObject() || thumb.IsDisplayObject());
		const double trackY = hasTrack ? numberMember(track, "_y", 0.0) : 0.0;
		const double thumbHeight = hasThumb ?
		    numberMember(thumb, "__height", numberMember(thumb, "_height", 0.0)) :
		    0.0;
		double availableHeight = numberMember(scrollBar, "availableHeight", -1.0);
		if (!std::isfinite(availableHeight) || availableHeight <= 0.001) {
			const double height = numberMember(scrollBar, "__height",
			    numberMember(scrollBar, "_height", 0.0));
			availableHeight = height - thumbHeight;
		}
		if (!std::isfinite(availableHeight) || availableHeight <= 0.001)
			return false;

		// Centering the thumb under the ray also makes a track click jump directly
		// to the requested page. Holding trigger continuously updates this value,
		// which produces the expected grab-and-drag behavior.
		double boundsRatioX = 0.0;
		double boundsRatioY = 0.0;
		const bool hasBoundsRatio = hasTrack &&
		    RootPointToDisplayObjectBoundsRatio(movie, track, rootX, rootY,
		        boundsRatioX, boundsRatioY, false);
		const double thumbTop = yValue.GetNumber() - thumbHeight * 0.5;
		const double ratio = std::clamp(hasBoundsRatio ? boundsRatioY :
		    (thumbTop - trackY) / availableHeight, 0.0, 1.0);
		const double position = minimum + ratio * (maximum - minimum);
		RE::GFxValue newPosition;
		newPosition.SetNumber(position);
		if (!scrollBar.SetMember("position", newPosition))
			return false;
		scrollBar.Invoke("updateThumb", nullptr, nullptr, 0);
		return true;
	}

	bool SetRaceMenuSliderAtViewportPoint(RE::GFxMovieView& movie,
	    RE::GFxValue& slider, float viewportX, float viewportY)
	{
		float rootX = 0.0f;
		float rootY = 0.0f;
		if (!ViewportToMovieRootPoint(movie, viewportX, viewportY, rootX, rootY))
			return false;

		double localX = 0.0;
		double localY = 0.0;
		const bool hasLocalPoint = RootPointToDisplayObjectLocal(
		    movie, slider, rootX, rootY, localX, localY);

		auto numberMember = [&](const char* name, double fallback) {
			RE::GFxValue value;
			return slider.GetMember(name, &value) && value.IsNumber() ?
			    value.GetNumber() : fallback;
		};
		const double minimum = numberMember("minimum", 0.0);
		const double maximum = numberMember("maximum", 1.0);
		const double offsetLeft = numberMember("offsetLeft", 0.0);
		const double offsetRight = numberMember("offsetRight", 0.0);
		const double width = numberMember("__width", numberMember("_width", 1.0));
		const double usableWidth = width - offsetLeft - offsetRight;
		if (!std::isfinite(usableWidth) || usableWidth <= 0.001 ||
		    !std::isfinite(minimum) || !std::isfinite(maximum) || maximum <= minimum) {
			return false;
		}

		RE::GFxValue track;
		RE::GFxValue* boundsClip = &slider;
		if (slider.GetMember("track", &track) &&
		    (track.IsObject() || track.IsDisplayObject())) {
			boundsClip = &track;
		}
		double boundsRatioX = 0.0;
		double boundsRatioY = 0.0;
		const bool hasBoundsRatio = RootPointToDisplayObjectBoundsRatio(
		    movie, *boundsClip, rootX, rootY, boundsRatioX, boundsRatioY, false);
		if (!hasBoundsRatio && !hasLocalPoint)
			return false;
		// RaceMenu's regular sliders attach value text and other row geometry to
		// the track clip. getBounds() therefore reports almost twice the visible
		// track width on those rows. The slider's own local width and offsets are
		// the coordinates used by gfx.controls.Slider and remain correct for both
		// regular morph sliders and the smaller sculpt brush sliders.
		const double localRatio = (localX - offsetLeft) / usableWidth;
		const double sliderRatio = hasLocalPoint ? localRatio : boundsRatioX;
		double position = minimum + std::clamp(sliderRatio, 0.0, 1.0) *
		    (maximum - minimum);
		RE::GFxValue snapping;
		if (slider.GetMember("snapping", &snapping) && snapping.IsBool() &&
		    snapping.GetBool()) {
			const double interval = numberMember("snapInterval", 0.0);
			if (std::isfinite(interval) && interval > 0.0)
				position = std::round(position / interval) * interval;
		}
		position = std::clamp(position, minimum, maximum);

		RE::GFxValue oldValue;
		const bool hadOldValue = slider.GetMember("value", &oldValue) &&
		    oldValue.IsNumber();
		RE::GFxValue newValue;
		newValue.SetNumber(position);
		// RaceMenuSlider derives from gfx.controls.Slider. Its official mouse path
		// writes `value`, then dispatches a `change` event. Follow that route so the
		// thumb, BrushListEntry value text, brush data, and native callback all update.
		if (!slider.SetMember("value", newValue) &&
		    !slider.SetMember("position", newValue)) {
			return false;
		}
		slider.Invoke("updateThumb", nullptr, nullptr, 0);
		if (!hadOldValue || fabs(oldValue.GetNumber() - position) > 1.0e-6) {
			RE::GFxValue event;
			movie.CreateObject(&event);
			RE::GFxValue type;
			type.SetString("change");
			event.SetMember("type", type);
			const bool dispatched =
			    slider.Invoke("dispatchEventAndSound", nullptr, &event, 1);
			const bool callbackFallback = !dispatched &&
			    slider.HasMember("changedCallback") &&
			    slider.Invoke("changedCallback", nullptr, nullptr, 0);

			RE::GFxValue acceptedValue;
			const double accepted = slider.GetMember("value", &acceptedValue) &&
			        acceptedValue.IsNumber() ?
			    acceptedValue.GetNumber() : -DBL_MAX;
			static ULONGLONG lastSliderLogTick = 0;
			const ULONGLONG now = GetTickCount64();
			if (now - lastSliderLogTick >= 100) {
				lastSliderLogTick = now;
				SKSE::log::info(
				    "RACEMENU SLIDER change rootX={:.2f} boundsRatio={:.3f} localRatio={:.3f} source={} localX={:.2f} width={:.2f} range={:.3f}..{:.3f} old={:.3f} requested={:.3f} accepted={:.3f} event={} fallback={}",
				    rootX, hasBoundsRatio ? boundsRatioX : -DBL_MAX, localRatio,
				    hasLocalPoint ? "local" : "bounds", localX, width, minimum, maximum,
				    hadOldValue ? oldValue.GetNumber() : -DBL_MAX, position,
				    accepted, dispatched, callbackFallback);
			}
		}
		return true;
	}

	bool ActivateRaceMenuLaserTarget(RE::GFxMovieView& movie,
	    RaceMenuLaserTarget& target, float viewportX, float viewportY,
	    RE::GFxValue* dragSlider)
	{
		HoverRaceMenuLaserTarget(target);
		if (target.kind == RaceMenuLaserTargetKind::kButton) {
			// RaceMenu VR's final name dialog owns the semantic callbacks; invoke
			// those directly instead of relying on mouse handlers that the VR SWF
			// does not consistently publish for these two buttons.
			if (target.index == 500 || target.index == 501) {
				const char* method = target.index == 500 ? "onAccept" : "onCancel";
				if (target.owner.Invoke(method, nullptr, nullptr, 0))
					return true;
			}
			std::array<RE::GFxValue, 3> args;
			args[0].SetNumber(0.0); // controller index
			args[1].SetNumber(0.0); // mouse source
			args[2].SetNumber(0.0); // primary button
			const bool pressed = target.clip.Invoke(
			    "handleMousePress", nullptr, args);
			const bool released = target.clip.Invoke(
			    "handleMouseRelease", nullptr, args);
			return pressed || released;
		}
		if (target.kind == RaceMenuLaserTargetKind::kModeTab) {
			RE::GFxValue index;
			index.SetNumber(static_cast<double>(target.index));
			return target.owner.Invoke("setMode", nullptr, &index, 1);
		}
		if (target.kind == RaceMenuLaserTargetKind::kClipAction)
			return target.clip.Invoke("onPress", nullptr, nullptr, 0);
		if (target.kind == RaceMenuLaserTargetKind::kSlider) {
			if (dragSlider)
				*dragSlider = target.slider;
			return SetRaceMenuSliderAtViewportPoint(
			    movie, target.slider, viewportX, viewportY);
		}
		if (target.kind == RaceMenuLaserTargetKind::kScrollBar) {
			if (dragSlider)
				*dragSlider = target.slider;
			return SetVerticalScrollBarAtViewportPoint(
			    movie, target.slider, viewportX, viewportY);
		}
		if (target.kind == RaceMenuLaserTargetKind::kCategory ||
		    target.kind == RaceMenuLaserTargetKind::kItem) {
			std::array<RE::GFxValue, 2> args;
			args[0].SetNumber(static_cast<double>(target.index));
			args[1].SetNumber(0.0); // BasicList.SELECT_MOUSE
			return target.owner.Invoke("onItemPress", nullptr, args);
		}
		return false;
	}

	enum class JournalLeftPaneAction
	{
		kNone,
		kQuestTitles,
		kSystemCategory,
		kReturnSystemCategories
	};

	bool JournalMainFaderIsInteractive(RE::GFxMovieView& movie)
	{
		// SkyUI's embedded MCM ConfigPanel remains inside Journal Menu but fades the
		// journal fader out and moves a different focus tree on top. Positive proof
		// that the journal is hidden must cancel our pane recovery; missing members
		// simply mean an interface replacer uses another hierarchy, so other
		// feature-detection below decides whether the recovery is supported.
		RE::GFxValue visible;
		if (movie.GetVariable(&visible, "_root.QuestJournalFader._visible") &&
		    visible.IsBool() && !visible.GetBool()) {
			return false;
		}
		RE::GFxValue alpha;
		if (movie.GetVariable(&alpha, "_root.QuestJournalFader._alpha") &&
		    alpha.IsNumber() && alpha.GetNumber() <= 1.0) {
			return false;
		}
		return true;
	}

	bool ResolveMCMScrollTarget(RE::GFxMovieView& movie, float viewportX,
	    float viewportY, bool& listHit, RE::GFxValue& scrollBar)
	{
		listHit = false;
		scrollBar.SetUndefined();

		// SkyUI's MCM is an overlay inside Journal Menu. Its ConfigPanel object
		// exists even while the ordinary Journal is visible, so require positive
		// proof that the Journal fader has yielded before claiming its lists.
		if (JournalMainFaderIsInteractive(movie))
			return false;

		RE::GFxValue panel;
		if (!movie.GetVariable(&panel, "_root.ConfigPanelFader.configPanel") ||
		    (!panel.IsObject() && !panel.IsDisplayObject())) {
			return false;
		}
		if (!DisplayObjectIsUsable(panel))
			return false;

		float rootX = 0.0f;
		float rootY = 0.0f;
		if (!ViewportToMovieRootPoint(movie, viewportX, viewportY, rootX, rootY))
			return false;

		// ConfigPanel publishes these three lists directly. Using the members keeps
		// this independent of their screen placement and supports both the mod list
		// and an opened mod's submenu/options list.
		constexpr std::array<const char*, 3> listMembers = {
		    "_modList", "_subList", "_optionsList"
		};
		for (const char* listMember : listMembers) {
			RE::GFxValue list;
			if (!panel.GetMember(listMember, &list) ||
			    (!list.IsObject() && !list.IsDisplayObject()) ||
			    !DisplayObjectIsUsable(list)) {
				continue;
			}

			const bool thisListHit = DisplayObjectHitAtRootPoint(list, rootX, rootY);
			listHit = listHit || thisListHit;
			RE::GFxValue candidate;
			if (GetListScrollBar(list, candidate) &&
			    DisplayObjectIsUsable(candidate) &&
			    DisplayObjectHitAtRootPoint(candidate, rootX, rootY)) {
				listHit = true;
				scrollBar = candidate;
				return true;
			}
		}
		return false;
	}

	// The Journal's mouse selection and keyboard/gamepad focus are independent.
	// Clicking a quest title can leave FocusHandler on ObjectiveList, and the
	// System page deliberately disables its visible CategoryList while a right
	// submenu owns focus. Feature-detect the real list clips instead of guessing
	// a left-column X threshold (SkyUI and Dear Diary reposition these panels).
	JournalLeftPaneAction ResolveJournalLeftPaneAction(RE::GFxMovieView& movie,
	    float targetX, float targetY, int& systemState)
	{
		if (!JournalMainFaderIsInteractive(movie))
			return JournalLeftPaneAction::kNone;

		int currentTab = -1;
		if (!GetNumberVariable(movie,
		        "_root.QuestJournalFader.Menu_mc.iCurrentTab",
		        "_root.Menu_mc.iCurrentTab", currentTab)) {
			return JournalLeftPaneAction::kNone;
		}

		if (currentTab == 0) {
			constexpr std::array<const char*, 4> titleLists = {
			    "_root.QuestJournalFader.Menu_mc.QuestsFader.Page_mc.TitleList",
			    "_root.QuestJournalFader.Menu_mc.QuestsFader.Page_mc.TitleList_mc.List_mc",
			    "_root.Menu_mc.QuestsFader.Page_mc.TitleList",
			    "_root.Menu_mc.QuestsFader.Page_mc.TitleList_mc.List_mc"
			};
			if (MovieClipHitAtViewportPoint(movie, titleLists, targetX, targetY))
				return JournalLeftPaneAction::kQuestTitles;
			return JournalLeftPaneAction::kNone;
		}

		if (currentTab == 2) {
			constexpr std::array<const char*, 4> categoryLists = {
			    "_root.QuestJournalFader.Menu_mc.SystemFader.Page_mc.CategoryList",
			    "_root.QuestJournalFader.Menu_mc.SystemFader.Page_mc.CategoryList_mc.List_mc",
			    "_root.Menu_mc.SystemFader.Page_mc.CategoryList",
			    "_root.Menu_mc.SystemFader.Page_mc.CategoryList_mc.List_mc"
			};
			if (!MovieClipHitAtViewportPoint(movie, categoryLists, targetX, targetY))
				return JournalLeftPaneAction::kNone;

			if (!GetNumberVariable(movie,
			        "_root.QuestJournalFader.Menu_mc.SystemFader.Page_mc.iCurrentState",
			        "_root.Menu_mc.SystemFader.Page_mc.iCurrentState", systemState)) {
				return JournalLeftPaneAction::kNone;
			}
			if (systemState != 0)
				return JournalLeftPaneAction::kReturnSystemCategories;
			return JournalLeftPaneAction::kSystemCategory;
		}

		return JournalLeftPaneAction::kNone;
	}

	enum class JournalSystemConfirmAction
	{
		kNone,
		kAccept,
		kCancel
	};

	// SkyUI keeps load/quit/delete/default confirmations inside Journal Menu's
	// SystemPage instead of opening a separate MessageBoxMenu. Its input handler
	// explicitly consumes L2/R2 while confirming, so a laser trigger must invoke
	// the actual Yes/No control rather than masquerade as another gamepad trigger.
	JournalSystemConfirmAction ResolveJournalSystemConfirmAction(
	    RE::GFxMovieView& movie, float targetX, float targetY, int& systemState)
	{
		if (!JournalMainFaderIsInteractive(movie))
			return JournalSystemConfirmAction::kNone;

		int currentTab = -1;
		if (!GetNumberVariable(movie,
		        "_root.QuestJournalFader.Menu_mc.iCurrentTab",
		        "_root.Menu_mc.iCurrentTab", currentTab) ||
		    currentTab != 2 ||
		    !GetNumberVariable(movie,
		        "_root.QuestJournalFader.Menu_mc.SystemFader.Page_mc.iCurrentState",
		        "_root.Menu_mc.SystemFader.Page_mc.iCurrentState", systemState)) {
			return JournalSystemConfirmAction::kNone;
		}

		const bool confirming = systemState == 2 || systemState == 5 ||
		    systemState == 7 || systemState == 9 || systemState == 10;
		if (!confirming)
			return JournalSystemConfirmAction::kNone;

		// ButtonPanel creates Yes as button0 and No as button1. Keep the direct
		// references too: SkyUI stores both returned clips on SystemPage.
		constexpr std::array<const char*, 4> acceptButtons = {
		    "_root.QuestJournalFader.Menu_mc.SystemFader.Page_mc._acceptButton",
		    "_root.Menu_mc.SystemFader.Page_mc._acceptButton",
		    "_root.QuestJournalFader.Menu_mc.SystemFader.Page_mc.ConfirmPanel.buttonPanel.button0",
		    "_root.Menu_mc.SystemFader.Page_mc.ConfirmPanel.buttonPanel.button0"
		};
		constexpr std::array<const char*, 4> cancelButtons = {
		    "_root.QuestJournalFader.Menu_mc.SystemFader.Page_mc._cancelButton",
		    "_root.Menu_mc.SystemFader.Page_mc._cancelButton",
		    "_root.QuestJournalFader.Menu_mc.SystemFader.Page_mc.ConfirmPanel.buttonPanel.button1",
		    "_root.Menu_mc.SystemFader.Page_mc.ConfirmPanel.buttonPanel.button1"
		};
		if (MovieClipHitAtViewportPoint(movie, acceptButtons, targetX, targetY))
			return JournalSystemConfirmAction::kAccept;
		if (MovieClipHitAtViewportPoint(movie, cancelButtons, targetX, targetY))
			return JournalSystemConfirmAction::kCancel;
		return JournalSystemConfirmAction::kNone;
	}

	bool ActivateJournalSystemConfirmation(RE::GFxMovieView& movie,
	    JournalSystemConfirmAction action)
	{
		if (action == JournalSystemConfirmAction::kNone)
			return false;

		const char* primary = action == JournalSystemConfirmAction::kAccept ?
		    "_root.QuestJournalFader.Menu_mc.SystemFader.Page_mc.onAcceptMousePress" :
		    "_root.QuestJournalFader.Menu_mc.SystemFader.Page_mc.onCancelMousePress";
		const char* fallback = action == JournalSystemConfirmAction::kAccept ?
		    "_root.Menu_mc.SystemFader.Page_mc.onAcceptMousePress" :
		    "_root.Menu_mc.SystemFader.Page_mc.onCancelMousePress";
		bool invoked = movie.Invoke(primary, nullptr, nullptr, 0) ||
		    movie.Invoke(fallback, nullptr, nullptr, 0);
		if (!invoked) {
			// Interface replacers can preserve the state contract while relocating the
			// page object. Enter/Tab reaches the same official Yes/No handler.
			SendGFxKeyPulse(movie, action == JournalSystemConfirmAction::kAccept ?
			    RE::GFxKey::kReturn : RE::GFxKey::kTab);
		}
		SKSE::log::debug("LASER Journal System confirmation {} via {}",
		    action == JournalSystemConfirmAction::kAccept ? "YES" : "NO",
		    invoked ? "SystemPage handler" : "key fallback");
		return true;
	}

	// A mouse click can change SystemPage state without moving Scaleform's
	// controller focus off CategoryList. Native A then activates the stale
	// QuickSave row even though Settings/Load/etc. is visibly open. Re-run the
	// page's own focus resolver once per settled state; do not force it every
	// frame, because the laser and controller must still be free to hand focus
	// back and forth naturally.
	bool RepairJournalSystemFocus(RE::GFxMovieView& movie, int systemState)
	{
		RE::GFxValue stateArg;
		stateArg.SetNumber(static_cast<double>(systemState));
		return movie.Invoke(
		           "_root.QuestJournalFader.Menu_mc.SystemFader.Page_mc.UpdateStateFocus",
		           nullptr, &stateArg, 1) ||
		    movie.Invoke("_root.Menu_mc.SystemFader.Page_mc.UpdateStateFocus",
		        nullptr, &stateArg, 1);
	}

	// StatsPage's right-hand scrollbar deliberately assigns Scaleform focus to
	// StatsList. That is correct for a mouse drag, but it leaves the left category
	// list unable to receive the next native stick/A input. startPage() is SkyUI's
	// own focus reset and is cheap after initial population because bUpdated exits
	// before requesting the data again.
	bool RepairJournalStatsFocus(RE::GFxMovieView& movie)
	{
		return movie.Invoke(
		           "_root.QuestJournalFader.Menu_mc.StatsFader.Page_mc.startPage",
		           nullptr, nullptr, 0) ||
		    movie.Invoke("_root.Menu_mc.StatsFader.Page_mc.startPage",
		        nullptr, nullptr, 0);
	}

	// Alternate Perspective's opening selector is hosted in CustomMenu, but its
	// SkyUI BasicLists are plain AS2 clips rather than GFx button-event targets.
	// Feature-detect only that movie so other CustomMenu users keep their existing
	// behavior. NotifyMouseState is required for BasicList rollover and itemPress.
	bool IsAlternatePerspectiveMenu(RE::GFxMovieView& movie)
	{
		RE::GFxValue main;
		RE::GFxValue menu;
		RE::GFxValue mainOptions;
		RE::GFxValue subOptions;
		return movie.GetVariable(&main, "_root.main") &&
		    (main.IsObject() || main.IsDisplayObject()) &&
		    main.HasMember("openMenu") && main.HasMember("onCloseMenu") &&
		    movie.GetVariable(&menu, "_root.main.menu") &&
		    (menu.IsObject() || menu.IsDisplayObject()) &&
		    menu.HasMember("setActiveLists") && menu.HasMember("onItemPressSub") &&
		    movie.GetVariable(&mainOptions, "_root.main.menu.mainOptions") &&
		    (mainOptions.IsObject() || mainOptions.IsDisplayObject()) &&
		    mainOptions.HasMember("getClipByIndex") &&
		    mainOptions.HasMember("onItemRollOver") &&
		    movie.GetVariable(&subOptions, "_root.main.menu.subOptions") &&
		    (subOptions.IsObject() || subOptions.IsDisplayObject()) &&
		    subOptions.HasMember("getClipByIndex") &&
		    subOptions.HasMember("onItemRollOver");
	}

	bool ResolveAlternatePerspectiveLaserTarget(RE::GFxMovieView& movie,
	    float viewportX, float viewportY, RaceMenuLaserTarget& target)
	{
		target = {};
		float rootX = 0.0f;
		float rootY = 0.0f;
		if (!ViewportToMovieRootPoint(movie, viewportX, viewportY, rootX, rootY))
			return false;

		constexpr std::array<const char*, 2> optionLists = {
		    "_root.main.menu.mainOptions",
		    "_root.main.menu.subOptions"
		};
		for (const char* path : optionLists) {
			const bool isSubList =
			    std::strcmp(path, "_root.main.menu.subOptions") == 0;
			RE::GFxValue list;
			if (!movie.GetVariable(&list, path) ||
			    (!list.IsObject() && !list.IsDisplayObject()) ||
			    !DisplayObjectIsUsable(list)) {
				continue;
			}

			// OptionList enables only the pane that currently owns focus. Reject the
			// greyed pane so its overlapping bounds cannot steal the laser.
			RE::GFxValue disabled;
			if ((list.GetMember("disableInput", &disabled) && disabled.IsBool() &&
			        disabled.GetBool()) ||
			    (list.GetMember("disableSelection", &disabled) && disabled.IsBool() &&
			        disabled.GetBool()) ||
			    (list.GetMember("_selectDisable", &disabled) && disabled.IsBool() &&
			        disabled.GetBool())) {
				continue;
			}

			RE::GFxValue scrollBar;
			if (GetListScrollBar(list, scrollBar) && DisplayObjectIsUsable(scrollBar) &&
			    DisplayObjectGeometryHitAtRootPoint(
			        movie, scrollBar, rootX, rootY)) {
				target.kind = RaceMenuLaserTargetKind::kScrollBar;
				target.owner = list;
				target.clip = scrollBar;
				target.slider = scrollBar;
				return true;
			}

			// Both MainList and SubList publish the number of attached, visible entry
			// clips as _listIndex. Fall back to a bounded probe for interface variants.
			int clipCount = 48;
			RE::GFxValue listIndex;
			if (list.GetMember("_listIndex", &listIndex) && listIndex.IsNumber() &&
			    listIndex.GetNumber() >= 0.0 && listIndex.GetNumber() <= 128.0) {
				clipCount = static_cast<int>(listIndex.GetNumber());
			}

			// SubListEntry has no full-width row background. Resolve its rows from the
			// exact local layout used by ScrollingList.UpdateList: background origin,
			// borders, and entryHeight. This avoids depending on text or gradient bounds.
			// If an interface variant does not expose those members, retain the proven
			// per-clip fallback below instead of disabling the pane.
			if (isSubList) {
				RE::GFxValue background;
				auto getNumber = [](RE::GFxValue& object, const char* member,
				                     double& result) {
					RE::GFxValue value;
					if (!object.GetMember(member, &value) || !value.IsNumber())
						return false;
					result = value.GetNumber();
					return std::isfinite(result);
				};
				double localX = 0.0;
				double localY = 0.0;
				double backgroundX = 0.0;
				double backgroundY = 0.0;
				double backgroundWidth = 0.0;
				double leftBorder = 0.0;
				double rightBorder = 0.0;
				double topBorder = 0.0;
				double entryHeight = 0.0;
				const bool hasLayout = list.GetMember("background", &background) &&
				    (background.IsObject() || background.IsDisplayObject()) &&
				    RootPointToDisplayObjectLocal(
				        movie, list, rootX, rootY, localX, localY) &&
				    getNumber(background, "_x", backgroundX) &&
				    getNumber(background, "_y", backgroundY) &&
				    getNumber(background, "_width", backgroundWidth) &&
				    getNumber(list, "leftBorder", leftBorder) &&
				    getNumber(list, "rightBorder", rightBorder) &&
				    getNumber(list, "topBorder", topBorder) &&
				    getNumber(list, "entryHeight", entryHeight) &&
				    backgroundWidth > leftBorder + rightBorder && entryHeight > 0.0;
				if (hasLayout) {
					const double rowXMin = backgroundX + leftBorder;
					const double rowXMax = backgroundX + backgroundWidth - rightBorder;
					const double rowYMin = backgroundY + topBorder;
					const int clipIndex = static_cast<int>(
					    std::floor((localY - rowYMin) / entryHeight));
					if (localX >= rowXMin && localX <= rowXMax && clipIndex >= 0 &&
					    clipIndex < clipCount) {
						RE::GFxValue arg;
						arg.SetNumber(static_cast<double>(clipIndex));
						RE::GFxValue clip;
						RE::GFxValue itemIndex;
						if (list.Invoke("getClipByIndex", &clip, &arg, 1) &&
						    (clip.IsObject() || clip.IsDisplayObject()) &&
						    DisplayObjectIsUsable(clip) &&
						    clip.GetMember("itemIndex", &itemIndex) &&
						    itemIndex.IsNumber() && itemIndex.GetNumber() >= 0.0) {
							target.kind = RaceMenuLaserTargetKind::kItem;
							target.index = static_cast<int>(itemIndex.GetNumber());
							target.owner = list;
							target.clip = clip;
							return true;
						}
					}
				}
			}

			for (int clipIndex = 0; clipIndex < clipCount; ++clipIndex) {
				RE::GFxValue arg;
				arg.SetNumber(static_cast<double>(clipIndex));
				RE::GFxValue clip;
				if (!list.Invoke("getClipByIndex", &clip, &arg, 1) ||
				    (!clip.IsObject() && !clip.IsDisplayObject()) ||
				    !DisplayObjectIsUsable(clip)) {
					continue;
				}

				RE::GFxValue itemIndex;
				if (!clip.GetMember("itemIndex", &itemIndex) ||
				    !itemIndex.IsNumber() || itemIndex.GetNumber() < 0.0) {
					continue;
				}
				RE::GFxValue enabled;
				if ((clip.GetMember("enabled", &enabled) && enabled.IsBool() &&
				        !enabled.GetBool()) ||
				    (clip.GetMember("isEnabled", &enabled) && enabled.IsBool() &&
				        !enabled.GetBool())) {
					continue;
				}

				bool hit = DisplayObjectGeometryHitAtRootPoint(
				    movie, clip, rootX, rootY);
				constexpr std::array<const char*, 6> entryMembers = {
				    "background", "name", "modname", "index", "selectIndicator",
				    "hasSuboptions"
				};
				for (const char* memberName : entryMembers) {
					RE::GFxValue member;
					if (!hit && clip.GetMember(memberName, &member) &&
					    (member.IsObject() || member.IsDisplayObject())) {
						hit = DisplayObjectGeometryHitAtRootPoint(
						    movie, member, rootX, rootY);
					}
				}
				if (!hit)
					continue;

				target.kind = RaceMenuLaserTargetKind::kItem;
				target.index = static_cast<int>(itemIndex.GetNumber());
				target.owner = list;
				target.clip = clip;
				return true;
			}
		}
		return false;
	}

	// Skyrim VR's confirmation overlay is its own MessageBoxMenu. Mouse hover and
	// controller focus are separate, which permits OK and Cancel to look selected
	// simultaneously. Resolve the actual dynamic ButtonN clip under the laser.
	bool GetMessageBoxButtonAtViewportPoint(RE::GFxMovieView& movie,
	    float targetX, float targetY, int& buttonIndex, RE::GFxValue* buttonOut)
	{
		for (int i = 0; i < 32; ++i) {
			std::array<char, 4 * 160> pathStorage{};
			std::array<const char*, 4> paths{};
			for (int p = 0; p < 4; ++p)
				paths[p] = pathStorage.data() + p * 160;
			std::snprintf(pathStorage.data() + 0 * 160, 160,
			    "_root.MessageMenu.Buttons.Button%d", i);
			std::snprintf(pathStorage.data() + 1 * 160, 160,
			    "_root.MessageMenu.ButtonContainer.Button%d", i);
			std::snprintf(pathStorage.data() + 2 * 160, 160,
			    "_root.Menu_mc.Buttons.Button%d", i);
			std::snprintf(pathStorage.data() + 3 * 160, 160,
			    "_root.Menu_mc.ButtonContainer.Button%d", i);

			if (!MovieClipHitAtViewportPoint(movie, paths, targetX, targetY))
				continue;

			buttonIndex = i;
			if (buttonOut) {
				for (const char* path : paths) {
					if (movie.GetVariable(buttonOut, path) &&
					    (buttonOut->IsObject() || buttonOut->IsDisplayObject())) {
						break;
					}
				}
			}
			return true;
		}
		buttonIndex = -1;
		return false;
	}

	bool FocusMessageBoxButton(RE::GFxMovieView& movie, int buttonIndex,
	    bool activate)
	{
		if (buttonIndex < 0)
			return false;

		RE::GFxValue button;
		bool found = false;
		// Match all four hierarchies supported by hit testing.
		for (const char* parent : { "_root.MessageMenu.Buttons", "_root.MessageMenu.ButtonContainer",
		         "_root.Menu_mc.Buttons", "_root.Menu_mc.ButtonContainer" }) {
			char path[160]{};
			std::snprintf(path, sizeof(path), "%s.Button%d", parent, buttonIndex);
			if (movie.GetVariable(&button, path) &&
			    (button.IsObject() || button.IsDisplayObject())) {
				found = true;
				break;
			}
		}
		if (!found) {
			return false;
		}

		std::array<RE::GFxValue, 2> focusArgs;
		focusArgs[0] = button;
		focusArgs[1].SetNumber(0.0);
		bool focused = movie.Invoke(
		    "_global.gfx.managers.FocusHandler.instance.setFocus", nullptr,
		    focusArgs.data(), static_cast<std::uint32_t>(focusArgs.size()));
		if (!focused) {
			focused = movie.Invoke("_global.Selection.setFocus", nullptr,
			    focusArgs.data(), 1) ||
			    movie.Invoke("Selection.setFocus", nullptr, focusArgs.data(), 1);
		}
		if (!activate) {
			SKSE::log::debug("LASER MessageBox FOCUS button={} result={}",
			    buttonIndex, focused);
			return focused;
		}

		// VRMessageBox.ClickCallback only parses ButtonN._name then forwards N to
		// buttonPress. Supply the already resolved index directly; an AS Invoke
		// success alone does not establish that the native callback was reached.
		auto* ui = RE::UI::GetSingleton();
		auto menu = ui ? ui->GetMenu("MessageBoxMenu") : nullptr;
		RE::GFxValue index;
		index.SetNumber(static_cast<double>(buttonIndex));
		const bool previousLaserDispatch = g_laserMessageBoxDispatch;
		g_laserMessageBoxDispatch = true;
		const bool invoked = menu && InvokeNativeMenuCallback(
		    *menu, movie, "buttonPress", &index, 1);
		g_laserMessageBoxDispatch = previousLaserDispatch;
		SKSE::log::info("LASER MessageBox ACTIVATE button={} focus={} nativeCallback={}",
		    buttonIndex, focused, invoked);
		return invoked;
	}

	// A whole item-list panel is an intentional laser target even when Scaleform's
	// kButtonEvents hit-test does not expose its individual AS2 rows. This is a
	// geometry-only probe: unlike TryActivateHoveredVRItem it does not move mouse
	// state, change the menu platform, or activate anything.
	bool PointerOverVRItemList(RE::GFxMovieView& movie, const char* menuName,
	    float targetX, float targetY)
	{
		const bool supported = strcmp(menuName, "InventoryMenu") == 0 ||
		    strcmp(menuName, "MagicMenu") == 0 ||
		    strcmp(menuName, "ContainerMenu") == 0;
		if (!supported)
			return false;

		constexpr std::array<const char*, 4> itemLists = {
		    "_root.Menu_mc.inventoryLists.itemList",
		    "_root.Menu_mc.inventoryLists.itemList.List_mc",
		    "_root.Menu_mc.InventoryLists_mc.ItemsList",
		    "_root.Menu_mc.InventoryLists_mc.ItemsList.List_mc"
		};
		return MovieClipHitAtViewportPoint(movie, itemLists, targetX, targetY);
	}

	// When the laser is on empty menu space, the controller focus tree owns the
	// menu. Trigger is consumed by the runtime while its ray still intersects the
	// quad, so complete the focused row's native hand-aware action here. This path
	// is deliberately rejected while the movie still reports mouse platform 0;
	// that prevents a blank-space trigger from equipping a stale mouse selection.
	bool TryActivateFocusedVRItem(RE::GFxMovieView& movie, const char* menuName,
	    std::uint8_t laserHand)
	{
		const bool inventory = strcmp(menuName, "InventoryMenu") == 0;
		const bool magic = strcmp(menuName, "MagicMenu") == 0;
		const bool container = strcmp(menuName, "ContainerMenu") == 0;
		if (!inventory && !magic && !container)
			return false;

		RE::GFxValue itemList;
		bool skyUiLayout = movie.GetVariable(&itemList,
		    "_root.Menu_mc.inventoryLists.itemList") &&
		    (itemList.IsObject() || itemList.IsDisplayObject());
		const char* platformPath = "_root.Menu_mc._platform";
		const char* processMethod = "_root.Menu_mc.shouldProcessItemsListInput";
		if (!skyUiLayout) {
			if (!movie.GetVariable(&itemList,
			        "_root.Menu_mc.InventoryLists_mc.ItemsList") ||
			    (!itemList.IsObject() && !itemList.IsDisplayObject())) {
				return false;
			}
			platformPath = "_root.Menu_mc.iPlatform";
			processMethod = "_root.Menu_mc.ShouldProcessItemsListInput";
		}

		RE::GFxValue platform;
		if (!movie.GetVariable(&platform, platformPath) || !platform.IsNumber() ||
		    platform.GetNumber() == 0.0) {
			return false;
		}

		RE::GFxValue checkOverList;
		checkOverList.SetBoolean(false);
		RE::GFxValue canProcess;
		if (!movie.Invoke(processMethod, &canProcess, &checkOverList, 1) ||
		    !canProcess.IsBool() || !canProcess.GetBool()) {
			return false;
		}

		RE::GFxValue selectedIndex;
		RE::GFxValue selectedEntry;
		if (!itemList.GetMember("selectedIndex", &selectedIndex) ||
		    !selectedIndex.IsNumber() || selectedIndex.GetNumber() < 0.0 ||
		    !itemList.GetMember("selectedEntry", &selectedEntry) ||
		    selectedEntry.IsUndefined() || selectedEntry.IsNull()) {
			return false;
		}

		const double equipSlot = laserHand == 0 ? 1.0 : 0.0;
		RE::GFxValue slotArg;
		slotArg.SetNumber(equipSlot);
		bool invoked = false;
		if (container) {
			std::array<RE::GFxValue, 2> args;
			args[0].SetNumber(equipSlot);
			args[1].SetBoolean(false);
			invoked = movie.Invoke("_root.Menu_mc.AttemptTakeAndEquip", nullptr,
			    args.data(), static_cast<std::uint32_t>(args.size()));
			if (!invoked) {
				invoked = movie.Invoke("_root.Menu_mc.AttemptEquip", nullptr,
				    args.data(), static_cast<std::uint32_t>(args.size()));
			}
		} else {
			invoked = movie.Invoke("_root.Menu_mc.AttemptEquip", nullptr, &slotArg, 1);
		}
		if (!invoked)
			return false;

		SKSE::log::info(
		    "CONTROLLER focused-row ACTIVATE menu='{}' layout={} index={} hand={} slot={}",
		    menuName, skyUiLayout ? "SkyUI" : "vanilla",
		    static_cast<int>(selectedIndex.GetNumber()),
		    laserHand == 0 ? "LEFT" : (laserHand == 1 ? "RIGHT" : "UNKNOWN"),
		    static_cast<int>(equipSlot));
		return true;
	}

	// Skyrim VR's Inventory/Magic menus deliberately ignore mouse-originated
	// itemPress events.  A physical mouse click is completed by Skyrim's native
	// input layer calling AttemptEquip(slot); a GFx-only laser click never reaches
	// that layer.  Keep the pointer as a mouse, but complete an item-row trigger
	// through the same hand-aware AttemptEquip API Skyrim VR uses.
	//
	// This is deliberately limited to InventoryMenu/MagicMenu/ContainerMenu.
	// Dialogue keeps its proven NotifyMouseState + Return route, and every other
	// flat menu keeps the ordinary paired GFx mouse gesture.
	bool TryActivateHoveredVRItem(RE::GFxMovieView& movie, const char* menuName,
	    std::uint8_t laserHand, float targetX, float targetY)
	{
		const bool inventory = strcmp(menuName, "InventoryMenu") == 0;
		const bool magic = strcmp(menuName, "MagicMenu") == 0;
		// ContainerMenu shares the InventoryLists/ItemsList components and the
		// native AttemptEquip contract. Routing the laser trigger here restores
		// the VR trigger semantics (equip/read the hovered item, per hand)
		// instead of the Scaleform mouse click's "take". If a replacer movie
		// lacks any part of the contract, every probe below fails and the
		// caller falls back to the mouse click, i.e. today's behaviour.
		const bool container = strcmp(menuName, "ContainerMenu") == 0;
		if (!inventory && !magic && !container)
			return false;

		// SkyUI/Dear Diary and Bethesda's vanilla VR movies expose the same native
		// AttemptEquip contract through different AS2 member names. Feature-detect
		// the loaded movie instead of depending on a particular interface replacer.
		RE::GFxValue itemList;
		bool skyUiLayout = movie.GetVariable(&itemList,
		    "_root.Menu_mc.inventoryLists.itemList") &&
		    (itemList.IsObject() || itemList.IsDisplayObject());
		const char* platformPath = "_root.Menu_mc._platform";
		const char* processMethod = "_root.Menu_mc.shouldProcessItemsListInput";
		if (!skyUiLayout) {
			if (!movie.GetVariable(&itemList,
			        "_root.Menu_mc.InventoryLists_mc.ItemsList") ||
			    (!itemList.IsObject() && !itemList.IsDisplayObject())) {
				return false;
			}
			platformPath = "_root.Menu_mc.iPlatform";
			processMethod = "_root.Menu_mc.ShouldProcessItemsListInput";
		}

		// HandleEvent(kMouseMove) is enough to dispatch SkyUI's rollover/highlight,
		// but it does not reliably update the AS2 Mouse singleton used by
		// Mouse.getTopMostEntity(). Synchronize that state at the exact trigger
		// coordinate before asking the movie to prove which row is under the laser.
		// Keep this local to Inventory/Magic; Dialogue and Tween use separate proven
		// activation paths and must not inherit flat-menu mouse-state behavior.
		movie.NotifyMouseState(targetX, targetY, 0, 0);

		// Let the loaded SWF resolve its own Mouse.getTopMostEntity(). Calling
		// _global.Mouse from C++ produced a wrapper object whose ancestry did not
		// compare equal to itemList, so the old helper never reached AttemptEquip.
		//
		// The stock checks bypass mouse ancestry while the movie says a controller
		// is active. That can change simply because the player touched a
		// stick, even though this particular interaction is the laser mouse. Pin the
		// checks to mouse platform 0, restore the menu immediately, and only then
		// invoke AttemptEquip. This prevents a trigger over a category/tab from
		// equipping whichever row happened to remain selected.
		RE::GFxValue originalPlatform;
		RE::GFxValue mousePlatform;
		mousePlatform.SetNumber(0.0);
		if (!movie.GetVariable(&originalPlatform, platformPath) ||
		    !originalPlatform.IsNumber() ||
		    !movie.SetVariable(platformPath, mousePlatform,
		        RE::GFxMovie::SetVarType::kNormal)) {
			return false;
		}

		RE::GFxValue checkOverList;
		checkOverList.SetBoolean(true);
		RE::GFxValue canProcess;
		bool overSelectedRow = movie.Invoke(processMethod, &canProcess,
		    &checkOverList, 1) && canProcess.IsBool() && canProcess.GetBool();
		if (overSelectedRow && skyUiLayout) {
			// SkyUI adds a second guard which rejects its scrollbar and blank row
			// space. Vanilla's own ShouldProcess method is its complete stock guard.
			RE::GFxValue confirmsHover;
			overSelectedRow = movie.Invoke("_root.Menu_mc.confirmSelectedEntry",
			    &confirmsHover, nullptr, 0) && confirmsHover.IsBool() &&
			    confirmsHover.GetBool();
		}
		const bool platformRestored = movie.SetVariable(platformPath, originalPlatform,
		    RE::GFxMovie::SetVarType::kNormal);
		if (!platformRestored)
			SKSE::log::warn("LASER could not restore {} platform after row hit-test", menuName);
		if (!overSelectedRow) {
			return false;
		}

		RE::GFxValue selectedIndex;
		RE::GFxValue selectedEntry;
		if (!itemList.GetMember("selectedIndex", &selectedIndex) ||
		    !selectedIndex.IsNumber() || selectedIndex.GetNumber() < 0.0 ||
		    !itemList.GetMember("selectedEntry", &selectedEntry) ||
		    selectedEntry.IsUndefined() || selectedEntry.IsNull()) {
			return false;
		}

		// SkyUI's confirmSelectedEntry() above already performs the authoritative
		// topmost-entity walk and requires that entity's itemIndex to equal the
		// selected row. Do not repeat that proof with MovieClip.hitTest(): Scaleform
		// exposes that clip in local/list coordinates on these VR movies, while the
		// laser cursor is in stage coordinates, so the redundant test rejected a
		// visibly highlighted row and forced activation into the ignored mouse path.
		//
		// Vanilla has no confirmSelectedEntry() helper, so retain its rendered-row
		// proof until the stock menu gives us an equivalent semantic predicate.
		if (!skyUiLayout) {
			RE::GFxValue selectedClip;
			RE::GFxValue clipIndex;
			if (!selectedEntry.GetMember("clipIndex", &clipIndex) ||
			    !clipIndex.IsNumber() || clipIndex.GetNumber() < 0.0 ||
			    !itemList.Invoke("GetClipByIndex", &selectedClip, &clipIndex, 1)) {
				return false;
			}
			if (!selectedClip.IsObject() && !selectedClip.IsDisplayObject())
				return false;

			RE::GFxValue clipVisible;
			RE::GFxValue clipItemIndex;
			if (!selectedClip.GetMember("_visible", &clipVisible) ||
			    !clipVisible.IsBool() || !clipVisible.GetBool() ||
			    !selectedClip.GetMember("itemIndex", &clipItemIndex) ||
			    !clipItemIndex.IsNumber() ||
			    static_cast<int>(clipItemIndex.GetNumber()) !=
			        static_cast<int>(selectedIndex.GetNumber())) {
				return false;
			}

			RE::GViewport viewport{};
			movie.GetViewport(&viewport);
			const RE::GRectF visibleFrame = movie.GetVisibleFrameRect();
			const float frameWidth = visibleFrame.right - visibleFrame.left;
			const float frameHeight = visibleFrame.bottom - visibleFrame.top;
			if (viewport.width <= 0 || viewport.height <= 0 ||
			    !std::isfinite(frameWidth) || !std::isfinite(frameHeight) ||
			    frameWidth <= 0.0f || frameHeight <= 0.0f) {
				return false;
			}
			const float rootX = visibleFrame.left +
			    (targetX - static_cast<float>(viewport.left)) * frameWidth /
			        static_cast<float>(viewport.width);
			const float rootY = visibleFrame.top +
			    (targetY - static_cast<float>(viewport.top)) * frameHeight /
			        static_cast<float>(viewport.height);

			std::array<RE::GFxValue, 3> hitArgs;
			hitArgs[0].SetNumber(rootX);
			hitArgs[1].SetNumber(rootY);
			hitArgs[2].SetBoolean(false); // full row bounds, not glyph-only shape
			RE::GFxValue rowHit;
			if (!selectedClip.Invoke("hitTest", &rowHit, hitArgs) ||
			    !rowHit.IsBool() || !rowHit.GetBool()) {
				return false;
			}
		}

		// SkyUI/Skyrim use slot 0 for the right hand and slot 1 for the left.
		// Protocol v5 publishes the physical controller that generated this edge.
		// Legacy/unknown senders retain the game's traditional right-hand default.
		const double equipSlot = laserHand == 0 ? 1.0 : 0.0;
		bool equipInvoked = false;
		if (container) {
			// SkyUI-VR deliberately leaves ContainerMenu.AttemptEquip() empty. VR's
			// real per-hand contract is AttemptTakeAndEquip(slot, checkOverList): it
			// transfers the selected chest item into the player's inventory, then
			// equips it in the supplied hand (armor equips normally). This is distinct
			// from Inventory/Magic and from desktop SkyUI's platform/equip-mode route.
			// The exact laser row was already proven above, so do not make the VR movie
			// repeat its mouse ancestry test.
			std::array<RE::GFxValue, 2> equipArgs;
			equipArgs[0].SetNumber(equipSlot);
			equipArgs[1].SetBoolean(false); // exact row already proven above
			equipInvoked = movie.Invoke("_root.Menu_mc.AttemptTakeAndEquip", nullptr,
			    equipArgs.data(), static_cast<std::uint32_t>(equipArgs.size()));
		} else {
			RE::GFxValue equipArg;
			equipArg.SetNumber(equipSlot);
			// One argument preserves InventoryMenu's default over-list validation and
			// exactly matches MagicMenu's API in vanilla VR, SkyUI VR, and Dear Diary.
			equipInvoked = movie.Invoke("_root.Menu_mc.AttemptEquip", nullptr,
			    &equipArg, 1);
		}
		if (!equipInvoked)
			return false;

		SKSE::log::debug("LASER selected-row ACTIVATE menu='{}' layout={} index={} hand={} slot={}",
		    menuName, skyUiLayout ? "SkyUI" : "vanilla",
		    static_cast<int>(selectedIndex.GetNumber()),
		    laserHand == 0 ? "LEFT" : (laserHand == 1 ? "RIGHT" : "UNKNOWN"),
		    static_cast<int>(equipSlot));
		return true;
	}

	// Game-thread pump body. Scheduled by the scheduler thread below.
	void LaserCursorPumpOnce()
	{
		if (!g_pTransform)
			return;

		// State that persists across pump ticks (game thread only)
		static uint32_t s_lastFrameSeq = 0;
		static uint32_t s_lastPumpedMenuGeneration = 0xFFFFFFFFu;
		static uint32_t s_lastPressSeq = 0;
		static uint32_t s_lastReleaseSeq = 0;
		static bool     s_mouseHeld = false;
		static bool     s_clickArmed = false;
		static ULONGLONG s_clickRearmNotBefore = 0;
		static bool     s_cursorShown = false;
		static ULONGLONG s_pressTick = 0;
		static bool     s_wasActive = false;
		static int      s_diagLogsLeft = 0;
		static float    s_gain = 0.5f;       // closed-loop gain, adapted below
		static float    s_lastSentDx = 0.0f, s_lastSentDy = 0.0f;
		static float    s_lastCurX = -1.0f, s_lastCurY = -1.0f;
		static std::uint32_t s_planeGeneration = 0;
		static char     s_planeMenuName[64] = {};
		static ULONGLONG s_planeNotBefore = 0;
		static int      s_planeStableFrames = 0;
		static bool     s_planePublished = false;
		static bool     s_gfxMousePrimed = false;
		static bool     s_bookGestureActive = false;
		static bool     s_bookGestureMoved = false;
		static float    s_bookAnchorV = 0.0f;
		static float    s_bookPressU = 0.5f;
		static ULONGLONG s_bookActionNotBefore = 0;
		static RE::GPtr<RE::GFxMovieView> s_pressedMovie;
		static float    s_pressedMovieX = 0.0f;
		static float    s_pressedMovieY = 0.0f;
		static bool     s_pressedMovieUsesNotifyMouse = false;
		static bool     s_pressedMovieUsesSculpt = false;
		static bool     s_pressedMoviePendingStats = false;
		static int      s_pressedJournalTab = -1;
		static int      s_pressedJournalSystemState = -1;
		static bool     s_journalReturnToSystemCategories = false;
		static int      s_journalReturnLastPulsedState = -1;
		static bool     s_journalReplayCategoryPending = false;
		static bool     s_journalReplayCategoryClick = false;
		static float    s_journalReplayX = 0.0f;
		static float    s_journalReplayY = 0.0f;
		static ULONGLONG s_journalReplayNotBefore = 0;
		static int      s_journalObservedSystemState = -1;
		static int      s_messageBoxHoveredButton = -1;
		static bool     s_laserOwnsFocus = false;
		static std::uint64_t s_seenControllerIntentSerial = 0;
		static ULONGLONG s_controllerLaserLockUntil = 0;
		static bool     s_laserMotionAnchorValid = false;
		static float    s_laserMotionAnchorX = 0.0f;
		static float    s_laserMotionAnchorY = 0.0f;
		static ULONGLONG s_laserMotionAnchorTick = 0;
		static RE::GFxValue s_raceDragSlider;
		static bool     s_raceSliderDragging = false;
		static RE::GFxValue s_verticalDragScrollBar;
		static bool     s_verticalScrollBarDragging = false;
		static RE::GFxValue s_raceSculptDisplay;
		static RE::GFxValue s_raceSculptForeground;
		static double   s_raceSculptLastX = 0.0;
		static double   s_raceSculptLastY = 0.0;
		static RE::GFxValue s_raceHoveredButton;
		static float    s_candidatePos[3] = {};
		static float    s_candidateQuat[4] = { 0, 0, 0, 1 };
		static float    s_candidateWidth = 0.0f, s_candidateHeight = 0.0f;
		struct SemanticProbeCache
		{
			bool valid = false;
			uint32_t menuGeneration = 0;
			RE::GFxMovieView* movie = nullptr;
			ULONGLONG tick = 0;
			float x = 0.0f;
			float y = 0.0f;
			bool alternatePerspectiveMenu = false;
			bool buttonHit = false;
			JournalLeftPaneAction journalTarget = JournalLeftPaneAction::kNone;
			bool itemListHit = false;
			RaceMenuLaserTarget alternatePerspectiveTarget;
			bool alternatePerspectiveTargetHit = false;
			int messageBoxHoverButton = -1;
			bool messageBoxButtonHit = false;
			RaceMenuLaserTarget raceMenuTarget;
			bool raceMenuTargetHit = false;
			bool mcmListHit = false;
			RE::GFxValue mcmScrollBar;
			bool mcmScrollBarHit = false;
		};
		static SemanticProbeCache s_semanticProbe;
		static ULONGLONG s_lastSlowSemanticLog = 0;
		static ULONGLONG s_lastGfxDriveTick = 0;
		static float s_lastGfxDriveX = 0.0f;
		static float s_lastGfxDriveY = 0.0f;

		// A scheduler wake is not a rendered frame. Do the expensive scene/menu
		// work at most once per submitted frame, plus once for each menu-lifecycle
		// generation so close/transition cleanup cannot be skipped. This is the
		// second half of the backlog fix: even after the pending task completes, a
		// stalled renderer cannot repeatedly run Scaleform probes for the same frame.
		const uint32_t pumpFrameSeq = g_pTransform->laserFrameSeq;
		const uint32_t pumpMenuGeneration = g_menuPlaneGeneration;
		if (pumpFrameSeq == s_lastFrameSeq &&
		    pumpMenuGeneration == s_lastPumpedMenuGeneration) {
			return;
		}
		s_lastFrameSeq = pumpFrameSeq;
		s_lastPumpedMenuGeneration = pumpMenuGeneration;

		auto releasePressedMovie = [&](const char* reason) {
			// A gesture owns one exact Scaleform movie for its entire lifetime.
			// Never inject a coordinate-less/global mouse-up after the menu stack
			// changes: that was selecting a control in the newly opened movie.
			if (s_mouseHeld && s_pressedMovie) {
				if (s_pressedMovieUsesSculpt) {
					EndRaceMenuSculptStroke(*s_pressedMovie,
					    s_raceSculptDisplay, s_raceSculptForeground);
					SKSE::log::debug(
					    "LASER RaceMenu sculpt END at local({:.1f},{:.1f}) reason={}",
					    s_raceSculptLastX, s_raceSculptLastY, reason);
				} else if (s_pressedMovieUsesNotifyMouse) {
					// Journal controls (lists, sliders, steppers, scroll arrows) are
					// wired to AS2 Mouse state. End that exact movie's held bit even if
					// the menu stack changed before the physical trigger was released.
					s_pressedMovie->NotifyMouseState(
					    s_pressedMovieX, s_pressedMovieY, 0u, 0);
					SKSE::log::debug("LASER notify-click UP at ({:.1f},{:.1f}) reason={}",
					    s_pressedMovieX, s_pressedMovieY, reason);
				} else {
					RE::GFxMouseEvent up(RE::GFxEvent::EventType::kMouseUp, 0,
					    s_pressedMovieX, s_pressedMovieY);
					s_pressedMovie->HandleEvent(up);
					SKSE::log::debug("LASER gfx-click UP at ({:.1f},{:.1f}) reason={}",
					    s_pressedMovieX, s_pressedMovieY, reason);
				}
			}
			s_pressedMovie = nullptr;
			s_mouseHeld = false;
			s_pressedMovieUsesNotifyMouse = false;
			s_pressedMovieUsesSculpt = false;
			s_pressedMoviePendingStats = false;
			s_pressedJournalTab = -1;
			s_pressedJournalSystemState = -1;
			s_raceDragSlider.SetUndefined();
			s_raceSliderDragging = false;
			s_verticalDragScrollBar.SetUndefined();
			s_verticalScrollBarDragging = false;
			s_raceSculptDisplay.SetUndefined();
			s_raceSculptForeground.SetUndefined();
		};

		bool menuActive = !g_activeTrackedMenus.empty();

		// Hard Sovngarde guard: while StatsMenu (level-up constellation) is up,
		// do nothing at all. We never touch Scaleform anyway, but stay out of
		// the input path too while its special scene owns rendering.
		auto ui = RE::UI::GetSingleton();
		bool statsOpen = ui && ui->IsMenuOpen("StatsMenu");
		// Geometry, hover, and activation must all key off the same advertised
		// top movie. An underlying open menu must not override the active plane.
		bool mapOpen = g_mapMenuOpen;
		bool dialogueOpen = strcmp(g_pTransform->menuName, "Dialogue Menu") == 0;
		bool journalOpen = strcmp(g_pTransform->menuName, "Journal Menu") == 0;
		bool raceMenuOpen = strcmp(g_pTransform->menuName, "RaceSex Menu") == 0;
		if (ui)
			RecoverRaceMenuKeyboard(*ui, raceMenuOpen && !statsOpen && !mapOpen);
		// MapMenu is entirely Skyrim-owned. OCU does not inspect its native pointer,
		// scene graph, input handlers, or Scaleform movie, and publishes no custom
		// map laser. This preserves the original Skyrim VR map laser unchanged.
		static bool s_loggedNativeMapBypass = false;
		if (mapOpen && !s_loggedNativeMapBypass) {
			s_loggedNativeMapBypass = true;
			SKSE::log::debug(
			    "LASER MapMenu native bypass: OCU map geometry, Scaleform, input, depth bridge, and compositor laser disabled");
		}
		if (mapOpen) {
			if (s_semanticProbe.valid)
				s_semanticProbe = SemanticProbeCache{};
			s_lastGfxDriveTick = 0;
			g_pTransform->updateCounter++;
			g_pTransform->uiPlaneValid = 0;
			g_pTransform->mapPointerValid = 0;
			g_pTransform->mapPointerDistanceMeters = 0.0f;
			g_pTransform->updateCounter++;

			// Drop only OCU-owned state. Never call a movie during MapMenu entry or
			// teardown; Skyrim owns every map click and controller edge.
			s_pressedMovie = nullptr;
			s_mouseHeld = false;
			s_pressedMovieUsesNotifyMouse = false;
			s_pressedMovieUsesSculpt = false;
			s_pressedMoviePendingStats = false;
			s_raceDragSlider.SetUndefined();
			s_raceSliderDragging = false;
			s_verticalDragScrollBar.SetUndefined();
			s_verticalScrollBarDragging = false;
			s_raceSculptDisplay.SetUndefined();
			s_raceSculptForeground.SetUndefined();
			s_raceHoveredButton.SetUndefined();
			s_clickArmed = false;
			s_lastPressSeq = g_pTransform->laserPressSeq;
			s_lastReleaseSeq = g_pTransform->laserReleaseSeq;
			s_wasActive = false;
			s_planePublished = false;
			s_planeStableFrames = 0;
			s_gfxMousePrimed = false;
			s_laserOwnsFocus = false;
			s_cursorShown = false; // Forget OCU ownership; do not hide Skyrim's cursor later.
			return;
		}
		if (!raceMenuOpen)
			s_raceHoveredButton.SetUndefined();
		// Special geometry and special input must key off the same advertised top
		// menu. BookMenu can remain open underneath another tracked overlay.
		bool bookOpen = ui && ui->IsMenuOpen("Book Menu") &&
		    strcmp(g_pTransform->menuName, "Book Menu") == 0;

		// Stats/Sovngarde forbids Scaleform calls. Retain the exact old movie
		// through that interval, then clear its held state on the first safe tick.
		if (!statsOpen && s_pressedMoviePendingStats && s_pressedMovie)
			releasePressedMovie("post-Stats cleanup");

		if (!menuActive || statsOpen) {
			s_semanticProbe = SemanticProbeCache{};
			s_lastGfxDriveTick = 0;
			s_journalReturnToSystemCategories = false;
			s_journalReturnLastPulsedState = -1;
			s_journalReplayCategoryPending = false;
			s_journalReplayCategoryClick = false;
			s_journalReplayNotBefore = 0;
			s_journalObservedSystemState = -1;
			s_messageBoxHoveredButton = -1;
			s_laserOwnsFocus = false;
			s_seenControllerIntentSerial = g_controllerMenuIntentSerial.load(std::memory_order_acquire);
			s_controllerLaserLockUntil = 0;
			s_laserMotionAnchorValid = false;
			if (s_wasActive) {
				g_pTransform->updateCounter++;
				g_pTransform->uiPlaneValid = 0;
				g_pTransform->mapPointerValid = 0;
				g_pTransform->updateCounter++;
				if (s_mouseHeld || s_pressedMovie) {
					if (statsOpen) {
						// Absolute Sovngarde guard: do not call any Scaleform movie while
						// the constellation renderer owns the shared UI machinery.
						// Keep the native held state until a safe post-Stats tick can
						// deliver its release. Clearing it here wedges Skyrim's mouse.
						s_pressedMoviePendingStats = s_mouseHeld || s_pressedMovie.get() != nullptr;
					} else {
						releasePressedMovie("menu closed");
					}
				}
				if (s_cursorShown) {
					if (auto mc = RE::MenuCursor::GetSingleton())
						mc->SetCursorVisibility(false);
					s_cursorShown = false;
				}
				s_wasActive = false;
				s_planePublished = false;
				s_planeStableFrames = 0;
				s_bookGestureActive = false;
				s_bookGestureMoved = false;
			}
			return;
		}

		const bool generationChanged = s_planeGeneration != g_menuPlaneGeneration;
		const bool menuNameChanged = strncmp(s_planeMenuName, g_pTransform->menuName, sizeof(s_planeMenuName)) != 0;
		if (!s_wasActive || generationChanged || menuNameChanged) {
			s_semanticProbe = SemanticProbeCache{};
			s_lastGfxDriveTick = 0;
			s_journalReturnToSystemCategories = false;
			s_journalReturnLastPulsedState = -1;
			s_journalReplayCategoryPending = false;
			s_journalReplayCategoryClick = false;
			s_journalReplayNotBefore = 0;
			s_journalObservedSystemState = -1;
			s_messageBoxHoveredButton = -1;
			s_laserOwnsFocus = false;
			s_seenControllerIntentSerial = g_controllerMenuIntentSerial.load(std::memory_order_acquire);
			s_controllerLaserLockUntil = 0;
			s_laserMotionAnchorValid = false;
			// A Scaleform MouseDown can open a different movie before the
			// controller is released (Settings -> Mod Configuration is the
			// common case). Never deliver the old page's MouseUp to the new
			// movie at the same screen coordinate. Consume every edge during
			// a short page-settle quarantine, then arm from a clean snapshot.
			if (s_mouseHeld || s_pressedMovie)
				releasePressedMovie("menu transition");
			s_clickArmed = false;
			s_clickRearmNotBefore = GetTickCount64() + 180;
			s_lastPressSeq = g_pTransform->laserPressSeq;
			s_lastReleaseSeq = g_pTransform->laserReleaseSeq;
			s_wasActive = true;
			s_planeGeneration = g_menuPlaneGeneration;
			strncpy_s(s_planeMenuName, g_pTransform->menuName, sizeof(s_planeMenuName) - 1);
			s_planeMenuName[sizeof(s_planeMenuName) - 1] = '\0';
			s_planeNotBefore = GetTickCount64() + 75;
			s_planeStableFrames = 0;
			s_planePublished = false;
			s_gfxMousePrimed = false;
			s_bookGestureActive = false;
			s_bookGestureMoved = false;
			s_bookActionNotBefore = 0;
			s_diagLogsLeft = 3; // log diagnostics for the first few ticks per menu
			s_gain = 0.5f;
			 s_lastCurX = s_lastCurY = -1.0f;
		}

		// Resolve the exact movie advertised by the ordered open/close-event stack.
		// Do not gate this on UI::GetTopMostMenu(): in Skyrim VR that resolver can
		// return HUD/internal overlay entries (often with an empty VR menu name)
		// while Tween/Inventory/Magic is visibly on top. Requiring pointer equality
		// therefore suppressed every laser frame, and reading that unrelated
		// object's VRRuntimeData menuName for diagnostics could dereference invalid
		// data. The generation change + 180 ms edge quarantine above already makes
		// movie transitions fail closed without consulting the engine resolver.
		RE::GPtr<RE::IMenu> advertisedTopMenu;
		if (ui)
			advertisedTopMenu = ui->GetMenu(RE::BSFixedString(s_planeMenuName));
		const bool advertisedMenuReady = advertisedTopMenu && advertisedTopMenu->OnStack() &&
		    advertisedTopMenu->uiMovie;
		if (!advertisedMenuReady) {
			if (s_semanticProbe.valid)
				s_semanticProbe = SemanticProbeCache{};
			s_lastGfxDriveTick = 0;
			s_journalReturnToSystemCategories = false;
			s_journalReturnLastPulsedState = -1;
			s_journalReplayCategoryPending = false;
			s_journalReplayCategoryClick = false;
			s_journalReplayNotBefore = 0;
			s_laserOwnsFocus = false;
			s_seenControllerIntentSerial = g_controllerMenuIntentSerial.load(std::memory_order_acquire);
			s_controllerLaserLockUntil = 0;
			s_laserMotionAnchorValid = false;
			if (s_mouseHeld || s_pressedMovie)
				releasePressedMovie("advertised menu unavailable");
			g_pTransform->updateCounter++;
			g_pTransform->uiPlaneValid = 0;
			g_pTransform->mapPointerValid = 0;
			g_pTransform->updateCounter++;
			s_lastPressSeq = g_pTransform->laserPressSeq;
			s_lastReleaseSeq = g_pTransform->laserReleaseSeq;
			s_clickArmed = false;
			s_clickRearmNotBefore = GetTickCount64() + 180;
			s_planePublished = false;
			s_planeStableFrames = 0;
			s_bookGestureActive = false;
			s_bookGestureMoved = false;

			static ULONGLONG s_lastMenuUnavailableLog = 0;
			const ULONGLONG now = GetTickCount64();
			if (now - s_lastMenuUnavailableLog >= 1000) {
				s_lastMenuUnavailableLog = now;
				SKSE::log::warn("LASER advertised menu '{}' is not ready; plane/input suppressed",
				    s_planeMenuName);
			}
			return;
		}

		// ---- Export UI plane + cursor feedback (SKSE-owned fields, seqlock) ----
		g_pTransform->updateCounter++;
		// Keep exporting after the initial coherence gate. s_planePublished now
		// means "live tracking armed", not "freeze the first accepted pose".
		bool planeOk = ExportUiPlane(s_diagLogsLeft > 0, dialogueOpen, bookOpen);
		if (!s_planePublished) {
			if (planeOk) {
				const float dx = g_pTransform->uiPlanePos[0] - s_candidatePos[0];
				const float dy = g_pTransform->uiPlanePos[1] - s_candidatePos[1];
				const float dz = g_pTransform->uiPlanePos[2] - s_candidatePos[2];
				const float posDelta = sqrtf(dx * dx + dy * dy + dz * dz);
				const float quatDot = fabsf(
				    g_pTransform->uiPlaneQuat[0] * s_candidateQuat[0] +
				    g_pTransform->uiPlaneQuat[1] * s_candidateQuat[1] +
				    g_pTransform->uiPlaneQuat[2] * s_candidateQuat[2] +
				    g_pTransform->uiPlaneQuat[3] * s_candidateQuat[3]);
				const bool sameCandidate = s_planeStableFrames > 0 && posDelta < 0.002f && quatDot > 0.99995f &&
				    fabsf(g_pTransform->uiPlaneWidth - s_candidateWidth) < 0.002f &&
				    fabsf(g_pTransform->uiPlaneHeight - s_candidateHeight) < 0.002f;
				if (sameCandidate) {
					++s_planeStableFrames;
				} else {
					for (int i = 0; i < 3; ++i) s_candidatePos[i] = g_pTransform->uiPlanePos[i];
					for (int i = 0; i < 4; ++i) s_candidateQuat[i] = g_pTransform->uiPlaneQuat[i];
					s_candidateWidth = g_pTransform->uiPlaneWidth;
					s_candidateHeight = g_pTransform->uiPlaneHeight;
					s_planeStableFrames = 1;
				}

				if (GetTickCount64() >= s_planeNotBefore && s_planeStableFrames >= 3) {
					s_planePublished = true;
					SKSE::log::debug("LASER live tracking ARMED menu='{}' generation={} stableFrames={} pos({:.3f},{:.3f},{:.3f})",
					    s_planeMenuName, s_planeGeneration, s_planeStableFrames,
					    g_pTransform->uiPlanePos[0], g_pTransform->uiPlanePos[1], g_pTransform->uiPlanePos[2]);
				}
			} else {
				s_planeStableFrames = 0;
			}
		}
		g_pTransform->uiPlaneValid = s_planePublished ? 1 : 0;

		auto mc = RE::MenuCursor::GetSingleton();
		if (mc) {
			auto& cd = mc->GetRuntimeData();
			g_pTransform->cursorPosX = cd.cursorPosX;
			g_pTransform->cursorPosY = cd.cursorPosY;
			g_pTransform->cursorRangeX = cd.screenWidthX;
			g_pTransform->cursorRangeY = cd.screenWidthY;
			if (s_diagLogsLeft > 0)
				SKSE::log::debug("LASER cursor pos({:.1f},{:.1f}) range({:.1f},{:.1f}) sens={:.3f} showCount={}",
				    cd.cursorPosX, cd.cursorPosY, cd.screenWidthX, cd.screenWidthY,
				    cd.cursorSensitivity, cd.showCursorCount);
		}
		g_pTransform->updateCounter++;
		if (s_diagLogsLeft > 0)
			s_diagLogsLeft--;

		// ---- Cursor drive (already gated to one pump per rendered frame) ----

		const bool bookMode = bookOpen;
		if (bookMode) {
			// BookMenu's bottom Scaleform bar is display-only in VR. Hide the
			// ordinary 2D cursor and convert laser gestures into native page actions.
			if (s_cursorShown && mc) {
				mc->SetCursorVisibility(false);
				s_cursorShown = false;
			}

			const uint32_t pressSeq = g_pTransform->laserPressSeq;
			const uint32_t releaseSeq = g_pTransform->laserReleaseSeq;
			if (!s_clickArmed) {
				s_lastPressSeq = pressSeq;
				s_lastReleaseSeq = releaseSeq;
				if (GetTickCount64() >= s_clickRearmNotBefore) {
					s_clickArmed = true;
					SKSE::log::debug("LASER BookMenu gesture input armed");
				}
			}

			if (!g_pTransform->laserActive) {
				// Leaving the physical page cancels the gesture. Never leak a book
				// press into the generic mouse path when the ray comes back.
				s_lastPressSeq = pressSeq;
				s_lastReleaseSeq = releaseSeq;
				s_mouseHeld = false;
				s_bookGestureActive = false;
				s_bookGestureMoved = false;
				return;
			}

			bool bookReady = false;
			bool isNote = false;
			if (ui) {
				if (auto menu = ui->GetMenu<RE::BookMenu>()) {
					auto& bookData = menu->GetRuntimeData();
					bookReady = planeOk && s_planePublished && bookData.bookInitialized && !bookData.closeMenu &&
					    bookData.bookModel && bookData.startAnimating == 0;
					isNote = bookData.isNote;
				}
			}

			if (s_clickArmed && pressSeq != s_lastPressSeq) {
				s_lastPressSeq = pressSeq;
				s_pressTick = GetTickCount64();
				s_mouseHeld = true;
				s_bookGestureActive = bookReady;
				s_bookGestureMoved = false;
				s_bookAnchorV = g_pTransform->laserV;
				s_bookPressU = g_pTransform->laserU;
				SKSE::log::debug("LASER BookMenu gesture DOWN type={} uv({:.3f},{:.3f})",
				    isNote ? "note" : "book", s_bookPressU, s_bookAnchorV);
			}

			// Stock notes are paginated rather than pixel-scrollable. While the
			// trigger is held, each deliberate 10%-of-sheet vertical stroke becomes
			// one native page action. Upward laser motion advances (scrolls down).
			const ULONGLONG now = GetTickCount64();
			if (isNote && bookReady && s_mouseHeld && s_bookGestureActive &&
			    now >= s_bookActionNotBefore) {
				const float delta = s_bookAnchorV - g_pTransform->laserV;
				if (delta >= 0.10f || delta <= -0.10f) {
					const bool nextPage = delta > 0.0f;
					if (QueueBookPageAction(nextPage)) {
						s_bookGestureMoved = true;
						s_bookAnchorV = g_pTransform->laserV;
						s_bookActionNotBefore = now + 400;
					}
				}
			}

			if (s_clickArmed && releaseSeq != s_lastReleaseSeq) {
				s_lastReleaseSeq = releaseSeq;
				if (s_mouseHeld && s_bookGestureActive && bookReady && !s_bookGestureMoved &&
				    now >= s_bookActionNotBefore) {
					// Ignore the narrow spine/gutter so a shaky click cannot choose the
					// wrong side. This tap fallback works for notes as well as books.
					const float releaseU = 0.5f * (s_bookPressU + g_pTransform->laserU);
					if (releaseU < 0.47f || releaseU > 0.53f) {
						if (QueueBookPageAction(releaseU > 0.5f))
							s_bookActionNotBefore = now + 400;
					}
				}
				s_mouseHeld = false;
				s_bookGestureActive = false;
				s_bookGestureMoved = false;
			}
			return;
		}

		// A System-category click made while a right-side Journal submenu owns
		// focus means "return to the left list." Settings can be two states deep,
		// and its 10-frame transitions (plus an optional settings save) discard
		// input. Advance through the movie's official Tab/Cancel route one settled
		// state at a time until MAIN_STATE restores FocusHandler to CategoryList.
		if (s_journalReturnToSystemCategories) {
			int currentTab = -1;
			int systemState = -1;
			const bool journalStateReadable = journalOpen &&
			    JournalMainFaderIsInteractive(*advertisedTopMenu->uiMovie) &&
			    GetNumberVariable(*advertisedTopMenu->uiMovie,
			        "_root.QuestJournalFader.Menu_mc.iCurrentTab",
			        "_root.Menu_mc.iCurrentTab", currentTab) &&
			    GetNumberVariable(*advertisedTopMenu->uiMovie,
			        "_root.QuestJournalFader.Menu_mc.SystemFader.Page_mc.iCurrentState",
			        "_root.Menu_mc.SystemFader.Page_mc.iCurrentState", systemState);
			if (!journalStateReadable || currentTab != 2) {
				s_journalReturnToSystemCategories = false;
				s_journalReturnLastPulsedState = -1;
				s_journalReplayCategoryPending = false;
				s_journalReplayCategoryClick = false;
				s_journalReplayNotBefore = 0;
			} else if (systemState == 0) {
				const ULONGLONG now = GetTickCount64();
				if (s_journalReplayCategoryPending && s_journalReplayNotBefore == 0) {
					// MAIN_STATE can be observable one frame before its clips finish their
					// transition. Give the restored CategoryList a short settle window.
					s_journalReplayNotBefore = now + 60;
				} else if (!s_journalReplayCategoryPending || now >= s_journalReplayNotBefore) {
					if (s_journalReplayCategoryPending) {
						auto& movie = *advertisedTopMenu->uiMovie;
						movie.NotifyMouseState(s_journalReplayX, s_journalReplayY, 0u, 0);
						if (s_journalReplayCategoryClick) {
							movie.NotifyMouseState(s_journalReplayX, s_journalReplayY, 1u, 0);
							movie.NotifyMouseState(s_journalReplayX, s_journalReplayY, 0u, 0);
						}
						SKSE::log::debug(
						    "LASER Journal System restored left CategoryList and replayed {} at ({:.1f},{:.1f})",
						    s_journalReplayCategoryClick ? "click" : "hover",
						    s_journalReplayX, s_journalReplayY);
					} else {
						SKSE::log::debug("LASER Journal System focus restored to left CategoryList");
					}
					s_journalReturnToSystemCategories = false;
					s_journalReturnLastPulsedState = -1;
					s_journalReplayCategoryPending = false;
					s_journalReplayCategoryClick = false;
					s_journalReplayNotBefore = 0;
				}
			} else if (systemState != 13 &&
			    systemState != s_journalReturnLastPulsedState) {
				s_journalReplayNotBefore = 0;
				SendGFxKeyPulse(*advertisedTopMenu->uiMovie, RE::GFxKey::kTab);
				s_journalReturnLastPulsedState = systemState;
				SKSE::log::debug("LASER Journal System return-left step from state={}", systemState);
			}
		}

		// Keep native A attached to the pane that is actually visible. This is a
		// one-shot state repair, not a per-frame focus override, so controller and
		// laser ownership can remain fluid after the state settles.
		if (journalOpen && JournalMainFaderIsInteractive(*advertisedTopMenu->uiMovie)) {
			int currentTab = -1;
			int systemState = -1;
			const bool readable = GetNumberVariable(*advertisedTopMenu->uiMovie,
			        "_root.QuestJournalFader.Menu_mc.iCurrentTab",
			        "_root.Menu_mc.iCurrentTab", currentTab) &&
			    GetNumberVariable(*advertisedTopMenu->uiMovie,
			        "_root.QuestJournalFader.Menu_mc.SystemFader.Page_mc.iCurrentState",
			        "_root.Menu_mc.SystemFader.Page_mc.iCurrentState", systemState);
			// Journal is one persistent movie containing several internal pages. A
			// mouse-down that opens a different page must not remain held over the new
			// controls; otherwise the new page receives the same press and appears as a
			// second stacked menu. Release against the exact old movie and wait for a
			// fresh physical trigger press.
			if (readable && s_mouseHeld && s_pressedMovieUsesNotifyMouse &&
			    s_pressedMovie && s_pressedMovie.get() == advertisedTopMenu->uiMovie.get() &&
			    s_pressedJournalTab >= 0 &&
			    (currentTab != s_pressedJournalTab ||
			        (currentTab == 2 && systemState != s_pressedJournalSystemState))) {
				releasePressedMovie("Journal internal page transition");
				s_clickArmed = false;
				s_clickRearmNotBefore = GetTickCount64() + 180;
				s_lastPressSeq = g_pTransform->laserPressSeq;
				s_lastReleaseSeq = g_pTransform->laserReleaseSeq;
			}

			if (readable && currentTab == 2 && systemState != 13) {
				if (systemState != s_journalObservedSystemState) {
					const bool repaired = RepairJournalSystemFocus(
					    *advertisedTopMenu->uiMovie, systemState);
					SKSE::log::info(
					    "MENU Journal System state={} native focus repair={}",
					    systemState, repaired);
					s_journalObservedSystemState = systemState;
				}
			} else if (!readable || currentTab != 2) {
				s_journalObservedSystemState = -1;
			}
		} else {
			s_journalObservedSystemState = -1;
		}

		if (!mc)
			return;

		if (g_pTransform->laserActive) {
			auto& cd = mc->GetRuntimeData();
			// The game's cursor MUST be active while the laser drives it: the
			// engine only feeds MenuCursor position into Scaleform hover when
			// the cursor is shown (confirmed live: hidden cursor = no hover
			// highlight, and clicks "Accept" the stale focused item instead of
			// the pointed-at one). The arrow renders exactly under the laser
			// dot on the menu plane.
			// Re-assert visibility whenever the engine knocks it back down —
			// 2026-07-25 session data: a single latched SetCursorVisibility(true)
			// left showCursorCount at 0 for the entire session (something in the
			// VR menu path re-hides it), so latch-once is not enough.
			float rangeX = (cd.screenWidthX > 0.0f) ? cd.screenWidthX : 1280.0f;
			float rangeY = (cd.screenWidthY > 0.0f) ? cd.screenWidthY : 720.0f;
			// Safe-zone inset: the game insets UI content by safeZoneX/Y, so
			// plane UV maps into [safeZone, range - safeZone], not [0, range].
			float szX = cd.safeZoneX, szY = cd.safeZoneY;
			if (!std::isfinite(szX) || szX < 0.0f || szX > rangeX * 0.4f) szX = 0.0f;
			if (!std::isfinite(szY) || szY < 0.0f || szY > rangeY * 0.4f) szY = 0.0f;
			// The exported UV already follows Scaleform's left-to-right axis. Keep
			// hover and activation on that same coordinate; mirroring here reverses
			// Tween's visible Items and Magic targets.
			float targetX = szX + g_pTransform->laserU * (rangeX - 2.0f * szX);
			float targetY = szY + g_pTransform->laserV * (rangeY - 2.0f * szY);
			RE::GPtr<RE::GFxMovieView> laserMovie = advertisedTopMenu->uiMovie;
			const ULONGLONG semanticNow = GetTickCount64();
			// A menu can be on the UI stack before its ActionScript object tree is
			// finished constructing. Keep all semantic Scaleform probes inside the
			// same transition quarantine that already blocks click edges.
			const bool menuSemanticInputReady = semanticNow >= s_clickRearmNotBefore;
			const bool messageBoxOpen = strcmp(s_planeMenuName, "MessageBoxMenu") == 0;
			const uint32_t pressSeq = g_pTransform->laserPressSeq;
			const uint32_t releaseSeq = g_pTransform->laserReleaseSeq;
			const ULONGLONG intentNow = semanticNow;
			if (!s_clickArmed) {
				s_lastPressSeq = pressSeq;
				s_lastReleaseSeq = releaseSeq;
				if (intentNow >= s_clickRearmNotBefore)
					s_clickArmed = true;
			}
			const bool newLaserPress = s_clickArmed && pressSeq != s_lastPressSeq;
			const bool newLaserRelease = s_clickArmed && releaseSeq != s_lastReleaseSeq;

			// Keep the OpenXR ray and plane pose at the headset's full refresh rate, but
			// make Scaleform input strictly change-driven. The old 20 Hz stationary
			// keepalive repeatedly walked complex menu trees even though neither the ray
			// nor the hovered control had changed. Moving hover is capped at ~30 Hz;
			// menu transitions and every physical button edge bypass the limiter. A
			// click therefore probes the current point immediately, while a resting ray
			// performs no Scaleform work at all.
			const float semanticDx = targetX - s_semanticProbe.x;
			const float semanticDy = targetY - s_semanticProbe.y;
			const bool semanticMoved = !s_semanticProbe.valid ||
			    semanticDx * semanticDx + semanticDy * semanticDy >= 2.25f;
			constexpr ULONGLONG kMovingScaleformIntervalMs = 33;
			const bool semanticProbeDue = menuSemanticInputReady && laserMovie &&
			    (!s_semanticProbe.valid ||
			        s_semanticProbe.menuGeneration != s_planeGeneration ||
			        s_semanticProbe.movie != laserMovie.get() ||
			        newLaserPress || newLaserRelease ||
			        (semanticMoved &&
			            semanticNow - s_semanticProbe.tick >= kMovingScaleformIntervalMs));
			if (semanticProbeDue) {
				const auto semanticStarted = std::chrono::steady_clock::now();
				SemanticProbeCache next;
				next.valid = true;
				next.menuGeneration = s_planeGeneration;
				next.movie = laserMovie.get();
				next.tick = semanticNow;
				next.x = targetX;
				next.y = targetY;
				next.alternatePerspectiveMenu = strcmp(s_planeMenuName, "CustomMenu") == 0 &&
				    IsAlternatePerspectiveMenu(*laserMovie);
				if (!dialogueOpen) {
					next.buttonHit = laserMovie->HitTest(
					    targetX, targetY, RE::GFxMovieView::HitTestType::kButtonEvents, 0);
					if (journalOpen) {
						int ignoredSystemState = -1;
						next.journalTarget = ResolveJournalLeftPaneAction(
						    *laserMovie, targetX, targetY, ignoredSystemState);
					}
				}
				next.itemListHit = !next.buttonHit &&
				    PointerOverVRItemList(*laserMovie, s_planeMenuName, targetX, targetY);
				next.alternatePerspectiveTargetHit = next.alternatePerspectiveMenu &&
				    ResolveAlternatePerspectiveLaserTarget(
				        *laserMovie, targetX, targetY, next.alternatePerspectiveTarget);
				next.messageBoxButtonHit = messageBoxOpen &&
				    GetMessageBoxButtonAtViewportPoint(*laserMovie, targetX, targetY,
				        next.messageBoxHoverButton, nullptr);
				next.raceMenuTargetHit = raceMenuOpen &&
				    ResolveRaceMenuLaserTarget(
				        *laserMovie, targetX, targetY, next.raceMenuTarget);
				next.mcmScrollBarHit = journalOpen &&
				    ResolveMCMScrollTarget(
				        *laserMovie, targetX, targetY, next.mcmListHit, next.mcmScrollBar);
				s_semanticProbe = std::move(next);

				const auto semanticMicros = std::chrono::duration_cast<std::chrono::microseconds>(
				    std::chrono::steady_clock::now() - semanticStarted).count();
				if (semanticMicros >= 4000 && semanticNow - s_lastSlowSemanticLog >= 2000) {
					s_lastSlowSemanticLog = semanticNow;
					SKSE::log::warn(
					    "LASER slow semantic probe menu='{}' duration={:.2f}ms",
					    s_planeMenuName, static_cast<double>(semanticMicros) / 1000.0);
				}
			}

			const bool alternatePerspectiveMenu = s_semanticProbe.valid &&
			    s_semanticProbe.alternatePerspectiveMenu;
			const bool buttonHit = s_semanticProbe.valid && s_semanticProbe.buttonHit;
			const JournalLeftPaneAction journalTarget = s_semanticProbe.valid ?
			    s_semanticProbe.journalTarget : JournalLeftPaneAction::kNone;
			const bool itemListHit = s_semanticProbe.valid && s_semanticProbe.itemListHit;
			RaceMenuLaserTarget& alternatePerspectiveTarget =
			    s_semanticProbe.alternatePerspectiveTarget;
			const bool alternatePerspectiveTargetHit = s_semanticProbe.valid &&
			    s_semanticProbe.alternatePerspectiveTargetHit;
			const int messageBoxHoverButton = s_semanticProbe.valid ?
			    s_semanticProbe.messageBoxHoverButton : -1;
			const bool messageBoxButtonHit = s_semanticProbe.valid &&
			    s_semanticProbe.messageBoxButtonHit;
			RaceMenuLaserTarget& raceMenuTarget = s_semanticProbe.raceMenuTarget;
			const bool raceMenuTargetHit = s_semanticProbe.valid &&
			    s_semanticProbe.raceMenuTargetHit;
			const bool mcmListHit = s_semanticProbe.valid && s_semanticProbe.mcmListHit;
			RE::GFxValue& mcmScrollBar = s_semanticProbe.mcmScrollBar;
			const bool mcmScrollBarHit = s_semanticProbe.valid &&
			    s_semanticProbe.mcmScrollBarHit;
			// RaceMenu sliders use track clicks and drags whose empty track regions do
			// not always advertise kButtonEvents. Treat its whole proven quad as an
			// input surface; the SWF still decides whether the pointed control reacts.
			const bool laserTargetInteractive = dialogueOpen ||
			    (menuSemanticInputReady && raceMenuOpen) || buttonHit ||
			    itemListHit || alternatePerspectiveTargetHit || messageBoxButtonHit || mcmListHit ||
			    s_verticalScrollBarDragging ||
			    journalTarget != JournalLeftPaneAction::kNone;

			const auto controllerIntentSerial =
			    g_controllerMenuIntentSerial.load(std::memory_order_acquire);
			if (controllerIntentSerial != s_seenControllerIntentSerial) {
				s_seenControllerIntentSerial = controllerIntentSerial;
				if (!dialogueOpen) {
					// A click/drag on StatsList's right scrollbar moves Scaleform focus
					// away from the left CategoryList. As soon as a native controller is
					// used again, hand focus back through SkyUI's own page routine. The
					// right stick still scrolls StatsList through onRightStickInput.
					if (journalOpen &&
					    JournalMainFaderIsInteractive(*advertisedTopMenu->uiMovie)) {
						int currentTab = -1;
						if (GetNumberVariable(*advertisedTopMenu->uiMovie,
						        "_root.QuestJournalFader.Menu_mc.iCurrentTab",
						        "_root.Menu_mc.iCurrentTab", currentTab) &&
						    currentTab == 1) {
							const bool repaired = RepairJournalStatsFocus(
							    *advertisedTopMenu->uiMovie);
							SKSE::log::info(
							    "MENU Journal Stats controller focus restored={} ", repaired);
						}
					}
					if (s_laserOwnsFocus) {
						SKSE::log::debug("MENU INPUT owner=CONTROLLER menu='{}' (native input)",
						    s_planeMenuName);
					}
					s_laserOwnsFocus = false;
					s_controllerLaserLockUntil = intentNow + 180;
					s_laserMotionAnchorValid = true;
					s_laserMotionAnchorX = targetX;
					s_laserMotionAnchorY = targetY;
					s_laserMotionAnchorTick = intentNow;
					if (s_mouseHeld || s_pressedMovie)
						releasePressedMovie("native controller intent");
					s_gfxMousePrimed = false;
				}
			}

			bool meaningfulLaserMotion = false;
			if (!s_laserMotionAnchorValid) {
				s_laserMotionAnchorValid = true;
				s_laserMotionAnchorX = targetX;
				s_laserMotionAnchorY = targetY;
				s_laserMotionAnchorTick = intentNow;
			} else {
				const float motionX = targetX - s_laserMotionAnchorX;
				const float motionY = targetY - s_laserMotionAnchorY;
				constexpr float kLaserIntentPixels = 40.0f;
				if (motionX * motionX + motionY * motionY >=
				    kLaserIntentPixels * kLaserIntentPixels) {
					if (intentNow >= s_controllerLaserLockUntil)
						meaningfulLaserMotion = true;
					// Do not discard movement accumulated during the short controller
					// lockout; it becomes laser intent as soon as that lock expires.
					if (intentNow >= s_controllerLaserLockUntil) {
						s_laserMotionAnchorX = targetX;
						s_laserMotionAnchorY = targetY;
						s_laserMotionAnchorTick = intentNow;
					}
				} else if (intentNow - s_laserMotionAnchorTick >= 500) {
					// A slow resting-hand drift must not accumulate forever into intent.
					s_laserMotionAnchorX = targetX;
					s_laserMotionAnchorY = targetY;
					s_laserMotionAnchorTick = intentNow;
				}
			}

			const bool explicitLaserIntent = laserTargetInteractive &&
			    (newLaserPress || meaningfulLaserMotion);
			if (dialogueOpen) {
				s_laserOwnsFocus = true;
			} else if (explicitLaserIntent && !s_laserOwnsFocus) {
				s_laserOwnsFocus = true;
				SKSE::log::debug(
				    "MENU INPUT owner=LASER menu='{}' intent={} target(button={}, itemList={}, altStart={}, messageBox={}, journal={})",
				    s_planeMenuName, newLaserPress ? "trigger" : "motion", buttonHit,
				    itemListHit, alternatePerspectiveTargetHit, messageBoxHoverButton,
				    static_cast<int>(journalTarget));
			}

			const bool laserShouldDrive = dialogueOpen ||
			    (s_laserOwnsFocus && laserTargetInteractive);
			if (!laserShouldDrive) {
				// Leave SkyUI's native focus untouched while the controller owns it.
				// In particular, never publish a mouse move from a merely resting ray.
				if (s_mouseHeld || s_pressedMovie)
					releasePressedMovie("laser input dormant");
				if (s_cursorShown) {
					mc->SetCursorVisibility(false);
					s_cursorShown = false;
				}
				s_gfxMousePrimed = false;
				s_messageBoxHoveredButton = -1;

				if (newLaserPress) {
					s_lastPressSeq = pressSeq;
					bool activated = false;
					if (strcmp(s_planeMenuName, "TweenMenu") == 0) {
						activated = ActivateHighlightedTweenSelection(*laserMovie);
					} else {
						activated = TryActivateFocusedVRItem(
						    *laserMovie, s_planeMenuName, g_pTransform->laserHand);
					}
					if (!activated) {
						// Trigger is masked by the runtime while its ray intersects the
						// quad. Return restores the same focused Accept path that A uses.
						SendGFxKeyPulse(*laserMovie, RE::GFxKey::kReturn);
						SKSE::log::info(
						    "CONTROLLER focused ACCEPT menu='{}' (trigger over empty laser space)",
						    s_planeMenuName);
					}
				}
				if (s_clickArmed && releaseSeq != s_lastReleaseSeq)
					s_lastReleaseSeq = releaseSeq;
				s_lastCurX = s_lastCurY = -1.0f;
				return;
			}

			if (messageBoxOpen) {
				if (messageBoxHoverButton >= 0 &&
				    messageBoxHoverButton != s_messageBoxHoveredButton) {
					FocusMessageBoxButton(*laserMovie, messageBoxHoverButton, false);
					s_messageBoxHoveredButton = messageBoxHoverButton;
				}
			} else {
				s_messageBoxHoveredButton = -1;
			}
			if (alternatePerspectiveTargetHit &&
			    alternatePerspectiveTarget.kind == RaceMenuLaserTargetKind::kItem) {
				HoverRaceMenuLaserTarget(alternatePerspectiveTarget);
			}
			if (s_pressedMovieUsesSculpt && s_mouseHeld && s_pressedMovie) {
				double localX = 0.0;
				double localY = 0.0;
				const bool sameCanvas = raceMenuOpen && laserMovie &&
				    s_pressedMovie.get() == laserMovie.get() &&
				    g_pTransform->laserTriggerHeld != 0 &&
				    GetRaceMenuSculptPoint(*laserMovie, s_raceSculptForeground,
				        targetX, targetY, localX, localY);
				if (sameCanvas) {
					const double dx = localX - s_raceSculptLastX;
					const double dy = localY - s_raceSculptLastY;
					if (dx * dx + dy * dy >= 0.0625 &&
					    ContinueRaceMenuSculptStroke(*laserMovie,
					        s_raceSculptForeground, targetX, targetY,
					        localX, localY)) {
						s_raceSculptLastX = localX;
						s_raceSculptLastY = localY;
					}
				} else {
					releasePressedMovie("left RaceMenu sculpt canvas");
				}
			}
			if (raceMenuTargetHit &&
			    raceMenuTarget.kind == RaceMenuLaserTargetKind::kButton) {
				if (!s_raceHoveredButton.IsUndefined() &&
				    !(s_raceHoveredButton == raceMenuTarget.clip)) {
					RE::GFxValue controller;
					controller.SetNumber(0.0);
					s_raceHoveredButton.Invoke(
					    "handleMouseRollOut", nullptr, &controller, 1);
					s_raceHoveredButton.SetUndefined();
				}
				if (s_raceHoveredButton.IsUndefined()) {
					HoverRaceMenuLaserTarget(raceMenuTarget);
					s_raceHoveredButton = raceMenuTarget.clip;
				}
			} else {
				if (!s_raceHoveredButton.IsUndefined()) {
					RE::GFxValue controller;
					controller.SetNumber(0.0);
					s_raceHoveredButton.Invoke(
					    "handleMouseRollOut", nullptr, &controller, 1);
					s_raceHoveredButton.SetUndefined();
				}
				if (raceMenuTargetHit) {
					if (raceMenuTarget.kind == RaceMenuLaserTargetKind::kSculptCanvas &&
					    !s_pressedMovieUsesSculpt) {
						HoverRaceMenuSculptCanvas(*laserMovie, raceMenuTarget.clip,
						    targetX, targetY);
					} else {
						HoverRaceMenuLaserTarget(raceMenuTarget);
					}
				}
			}
			if (s_raceSliderDragging) {
				if (raceMenuOpen && laserMovie && g_pTransform->laserTriggerHeld != 0) {
					SetRaceMenuSliderAtViewportPoint(
					    *laserMovie, s_raceDragSlider, targetX, targetY);
				} else {
					s_raceDragSlider.SetUndefined();
					s_raceSliderDragging = false;
				}
			}
			if (s_verticalScrollBarDragging) {
				if (laserMovie && g_pTransform->laserTriggerHeld != 0) {
					SetVerticalScrollBarAtViewportPoint(
					    *laserMovie, s_verticalDragScrollBar, targetX, targetY);
				} else {
					s_verticalDragScrollBar.SetUndefined();
					s_verticalScrollBarDragging = false;
				}
			}

			// RaceMenu now uses direct AS2 hit testing/callbacks for every supported
			// control, so its ordinary mouse arrow is redundant. Suppress both the
			// Skyrim cursor and Scaleform's logical cursor only for this movie. Other
			// menus retain the cursor because their hover paths still consume it.
			if (raceMenuOpen) {
				if (mc->GetRuntimeData().showCursorCount >= 0)
					mc->SetCursorVisibility(false);
				s_cursorShown = false;
				if (laserMovie && laserMovie->GetMouseCursorCount() != 0)
					laserMovie->SetMouseCursorCount(0);
			} else if (mc->GetRuntimeData().showCursorCount <= 0) {
				// The cursor is only made active after the arbiter grants laser ownership.
				// SetCursorVisibility itself changes Skyrim's mouse platform, so doing it
				// before this point recreated the exact stick/laser fight fixed above.
				mc->SetCursorVisibility(true);
				s_cursorShown = true;
			}
			// DIRECT CURSOR DRIVE (2026-07-25). The closed-loop mouse-delta
			// approach is dead: session data showed the game discarding the
			// synthetic AddMouseMoveEvent stream entirely (cursorPosX pinned at
			// 0 for minutes, err never converging, gain saturated). Write the
			// cursor position directly instead — this is the value the engine
			// feeds into Scaleform hover each frame, and it also can't "jut off
			// crazy": the cursor IS the laser target every tick, no feedback
			// loop to go unstable.
			float errX = targetX - cd.cursorPosX;
			float errY = targetY - cd.cursorPosY;
			float errMag = sqrtf(errX * errX + errY * errY); // pre-write, for diagnostics
			cd.cursorPosX = targetX;
			cd.cursorPosY = targetY;

			// SCALEFORM MOUSE EVENTS (2026-07-25): writing MenuCursor position
			// proved insufficient — our writes held the pen (they blocked the
			// right-hand pointer's highlighting), but hover never followed the
			// written positions: the engine's hover pipeline consumes real GFx
			// mouse EVENTS, not the cursor struct. Dispatch kMouseMove (and
			// kMouseDown/kMouseUp on trigger, below) into the top tracked
			// menu's movie — the same channel the VR keyboard's GFxCharEvent
			// injection has used safely for months. Game thread, tracked menus
			// only, never StatsMenu, no MovieDef access = no Sovngarde risk.
			const float gfxDriveDx = targetX - s_lastGfxDriveX;
			const float gfxDriveDy = targetY - s_lastGfxDriveY;
			const bool gfxPointerMoved = !s_gfxMousePrimed ||
			    gfxDriveDx * gfxDriveDx + gfxDriveDy * gfxDriveDy >= 2.25f;
			const bool activeScaleformDrag = s_raceSliderDragging ||
			    s_verticalScrollBarDragging || s_pressedMovieUsesSculpt;
			const bool gfxDriveDue = !s_gfxMousePrimed || newLaserPress || newLaserRelease ||
			    activeScaleformDrag ||
			    (gfxPointerMoved &&
			        semanticNow - s_lastGfxDriveTick >= kMovingScaleformIntervalMs);
			if (laserMovie && gfxDriveDue) {
				// Hover is authoritative. Do not synthesize a Down-arrow to prime
				// list focus; it can move selection away from the pointed-at row.
				const bool firstGfxMouse = !s_gfxMousePrimed;
				if (firstGfxMouse) {
					RE::GViewport viewport{};
					laserMovie->GetViewport(&viewport);
					const auto oldCursorCount = laserMovie->GetMouseCursorCount();
					if (!raceMenuOpen && oldCursorCount == 0)
						laserMovie->SetMouseCursorCount(1);
					SKSE::log::debug(
					    "LASER GFx mouse menu='{}' cursorCount {}->{} viewport buf={}x{} rect=({},{} {}x{})",
					    s_planeMenuName, oldCursorCount, laserMovie->GetMouseCursorCount(), viewport.bufferWidth,
					    viewport.bufferHeight, viewport.left, viewport.top, viewport.width, viewport.height);
				}
				// Journal's AS2 controls are driven by Mouse.getTopMostEntity() and
				// _xmouse/_ymouse: Settings sliders, list entries, and scroll arrows
				// all require the canonical mouse state and its held-button mask.
				// NotifyMouseState also generates Journal's internal move/edge events,
				// so never duplicate it with HandleEvent there. Other flat menus retain
				// the proven GFx event path; Dialogue only synchronizes position before
				// activating its focused choice with Return.
				if (journalOpen) {
					const bool notifyMouseHeld = s_mouseHeld && s_pressedMovieUsesNotifyMouse &&
					    s_pressedMovie && s_pressedMovie.get() == laserMovie.get();
					laserMovie->NotifyMouseState(targetX, targetY, notifyMouseHeld ? 1u : 0u, 0);
				} else if (alternatePerspectiveMenu) {
					// Alternate Perspective is driven through its exact BasicList entry and
					// scrollbar callbacks above and below. Skyrim VR's generic mouse state
					// does not reliably resolve these dynamically attached AS2 clips.
				} else if (raceMenuOpen) {
					// The installed RaceMenu VR movie maps AS2 Mouse coordinates incorrectly
					// through Skyrim's larger viewport. Its real controls are driven above by
					// their own semantic hover/press/slider callbacks instead.
				} else {
					if (dialogueOpen && !s_mouseHeld)
						laserMovie->NotifyMouseState(targetX, targetY, 0u, 0);
					RE::GFxMouseEvent mv(RE::GFxEvent::EventType::kMouseMove, 0,
					    targetX, targetY);
					laserMovie->HandleEvent(mv);
				}
				if (firstGfxMouse) {
					float mouseX = 0.0f, mouseY = 0.0f;
					std::uint32_t mouseButtons = 0;
					laserMovie->GetMouseState(0, &mouseX, &mouseY, &mouseButtons);
					const bool diagnosticButtonHit = laserMovie->HitTest(
					    targetX, targetY, RE::GFxMovieView::HitTestType::kButtonEvents, 0);
					SKSE::log::debug(
					    "LASER GFx state menu='{}' target({:.1f},{:.1f}) mouse({:.1f},{:.1f}) buttons={} buttonHit={}",
					    s_planeMenuName, targetX, targetY, mouseX, mouseY, mouseButtons, diagnosticButtonHit);
					s_gfxMousePrimed = true;
				}
				s_lastGfxDriveTick = semanticNow;
				s_lastGfxDriveX = targetX;
				s_lastGfxDriveY = targetY;
			}
			if (s_pressedMovie && s_pressedMovie.get() == laserMovie.get()) {
				s_pressedMovieX = targetX;
				s_pressedMovieY = targetY;
			}

			// Throttled drive diagnostics (every 2s while pointing).
			// err here = how far the cursor had drifted from target since the
			// last write; anything beyond a few px means the engine is moving
			// the cursor behind our back.
			static ULONGLONG s_lastDriveDiag = 0;
			ULONGLONG nowDiag = GetTickCount64();
			if (nowDiag - s_lastDriveDiag > 2000) {
				s_lastDriveDiag = nowDiag;
				SKSE::log::debug("LASER drive-direct uv({:.3f},{:.3f}) target({:.1f},{:.1f}) drift={:.1f} sz({:.1f},{:.1f}) showCount={}",
				    g_pTransform->laserU, g_pTransform->laserV, targetX, targetY,
				    errMag, cd.safeZoneX, cd.safeZoneY, cd.showCursorCount);
			}

			// Keep hover and activation in one exact movie. Dialogue uses its
			// focused-choice route; every flat menu receives one paired GFx mouse
			// gesture, matching the proven pre-Relos behavior.
			if (!s_clickArmed) {
				// Synchronize without injecting either edge throughout the settle
				// window. Do not require lifetime press/release counters to match:
				// a release can legitimately be lost while the old movie disappears,
				// and equality would then wedge every future menu open permanently.
				s_lastPressSeq = pressSeq;
				s_lastReleaseSeq = releaseSeq;
				if (GetTickCount64() >= s_clickRearmNotBefore) {
					s_clickArmed = true;
					SKSE::log::debug("LASER click input armed for '{}'", s_planeMenuName);
				}
			}
			if (s_clickArmed && pressSeq != s_lastPressSeq) {
				s_lastPressSeq = pressSeq;
				s_pressTick = GetTickCount64();
				if (laserMovie) {
					if (s_pressedMovie)
						releasePressedMovie("superseded press");

					if (dialogueOpen) {
						RE::GFxKeyEvent down(RE::GFxEvent::EventType::kKeyDown,
						    RE::GFxKey::kReturn, 0, 0, {}, 0);
						RE::GFxKeyEvent up(RE::GFxEvent::EventType::kKeyUp,
						    RE::GFxKey::kReturn, 0, 0, {}, 0);
						laserMovie->HandleEvent(down);
						laserMovie->HandleEvent(up);
						s_mouseHeld = false;
						s_pressedMovie = nullptr;
						s_pressedMovieUsesNotifyMouse = false;
						SKSE::log::debug("LASER Dialogue choice ACTIVATE at ({:.1f},{:.1f})", targetX, targetY);
					} else if (strcmp(s_planeMenuName, "TweenMenu") == 0) {
						ActivateHighlightedTweenSelection(*laserMovie);
						// Semantic activation is atomic. Never send a mouse-up into the
						// newly opened Inventory/Magic movie at the old Tween coordinate.
						s_mouseHeld = false;
						s_pressedMovie = nullptr;
						s_pressedMovieUsesNotifyMouse = false;
					} else if (TryActivateHoveredVRItem(*laserMovie, s_planeMenuName,
					               g_pTransform->laserHand, targetX, targetY)) {
						// AttemptEquip is an atomic press action.  Do not leave a synthetic
						// mouse button held or send a second activation on trigger release.
						s_mouseHeld = false;
						s_pressedMovie = nullptr;
						s_pressedMovieUsesNotifyMouse = false;
					} else if (messageBoxOpen && messageBoxHoverButton >= 0 &&
					    FocusMessageBoxButton(*laserMovie, messageBoxHoverButton, true)) {
						// Direct callback is atomic and can close this movie immediately.
						s_mouseHeld = false;
						s_pressedMovie = nullptr;
						s_pressedMovieUsesNotifyMouse = false;
					} else if (alternatePerspectiveTargetHit &&
					    alternatePerspectiveTarget.kind == RaceMenuLaserTargetKind::kScrollBar) {
						if (SetVerticalScrollBarAtViewportPoint(*laserMovie,
						        alternatePerspectiveTarget.slider, targetX, targetY)) {
							s_verticalDragScrollBar = alternatePerspectiveTarget.slider;
							s_verticalScrollBarDragging = true;
							s_mouseHeld = false;
							s_pressedMovie = nullptr;
							s_pressedMovieUsesNotifyMouse = false;
							SKSE::log::debug(
							    "LASER Alternate Perspective scrollbar drag START at ({:.1f},{:.1f})",
							    targetX, targetY);
						}
					} else if (alternatePerspectiveTargetHit &&
					    alternatePerspectiveTarget.kind == RaceMenuLaserTargetKind::kItem) {
						const int activatedIndex = alternatePerspectiveTarget.index;
						const bool activated = ActivateRaceMenuLaserTarget(*laserMovie,
						    alternatePerspectiveTarget, targetX, targetY, nullptr);
						s_mouseHeld = false;
						s_pressedMovie = nullptr;
						s_pressedMovieUsesNotifyMouse = false;
						SKSE::log::debug(
						    "LASER Alternate Perspective item ACTIVATE index={} result={} at ({:.1f},{:.1f})",
						    activatedIndex, activated, targetX, targetY);
					} else if (mcmScrollBarHit) {
						// MCM's dynamic CLIK scrollbar is not reported consistently by
						// GFx's button-event hit test. Drive its public position setter
						// directly so trigger-hold works across the mod/sub/options lists.
						if (SetVerticalScrollBarAtViewportPoint(
						        *laserMovie, mcmScrollBar, targetX, targetY)) {
							s_verticalDragScrollBar = mcmScrollBar;
							s_verticalScrollBarDragging = true;
							s_mouseHeld = false;
							s_pressedMovie = nullptr;
							s_pressedMovieUsesNotifyMouse = false;
							SKSE::log::debug(
							    "LASER MCM vertical scrollbar drag START at ({:.1f},{:.1f})",
							    targetX, targetY);
						}
					} else if (journalOpen) {
						int confirmState = -1;
						const auto confirmAction = ResolveJournalSystemConfirmAction(
						    *laserMovie, targetX, targetY, confirmState);
						if (ActivateJournalSystemConfirmation(*laserMovie, confirmAction)) {
							// Confirmation is atomic. Do not leave a synthetic mouse button
							// held over the Journal movie or replay it after the load begins.
							s_journalReturnToSystemCategories = false;
							s_journalReturnLastPulsedState = -1;
							s_journalReplayCategoryPending = false;
							s_journalReplayCategoryClick = false;
							s_journalReplayNotBefore = 0;
							s_mouseHeld = false;
							s_pressedMovie = nullptr;
							s_pressedMovieUsesNotifyMouse = false;
							SKSE::log::debug(
							    "LASER Journal System confirmation consumed at state={}", confirmState);
							return;
						}
						int systemState = -1;
						const auto leftPaneAction = ResolveJournalLeftPaneAction(
						    *laserMovie, targetX, targetY, systemState);
						if (leftPaneAction == JournalLeftPaneAction::kReturnSystemCategories) {
							// Consume this mouse gesture: the visible CategoryList is disabled in a
							// submenu. One trigger walks its official Back route until MAIN_STATE,
							// then restores hover on this row without clicking it. The user can
							// deliberately pull the trigger again to open that category.
							s_journalReturnToSystemCategories = true;
							s_journalReplayCategoryPending = true;
							s_journalReplayCategoryClick = false;
							s_journalReplayX = targetX;
							s_journalReplayY = targetY;
							s_journalReplayNotBefore = 0;
							SendGFxKeyPulse(*laserMovie, RE::GFxKey::kTab);
							s_journalReturnLastPulsedState = systemState;
							s_mouseHeld = false;
							s_pressedMovie = nullptr;
							s_pressedMovieUsesNotifyMouse = false;
							SKSE::log::debug(
							    "LASER Journal System return-left requested from state={}", systemState);
							return;
						}
						// Bit 0 is GFx's first/left mouse button. The 0->1 transition is
						// Journal's sole press event; continuing to publish 1 while held
						// gives slider thumbs and scroll-arrow repeat logic a real drag/hold.
						// Snapshot the page before MouseDown; the handler may synchronously
						// switch SystemPage state inside NotifyMouseState.
						s_pressedJournalTab = -1;
						s_pressedJournalSystemState = -1;
						GetNumberVariable(*laserMovie,
						    "_root.QuestJournalFader.Menu_mc.iCurrentTab",
						    "_root.Menu_mc.iCurrentTab", s_pressedJournalTab);
						if (s_pressedJournalTab == 2) {
							GetNumberVariable(*laserMovie,
							    "_root.QuestJournalFader.Menu_mc.SystemFader.Page_mc.iCurrentState",
							    "_root.Menu_mc.SystemFader.Page_mc.iCurrentState",
							    s_pressedJournalSystemState);
						}
						laserMovie->NotifyMouseState(targetX, targetY, 1u, 0);
						s_pressedMovie = laserMovie;
						s_pressedMovieX = targetX;
						s_pressedMovieY = targetY;
						s_mouseHeld = true;
						s_pressedMovieUsesNotifyMouse = true;
						SKSE::log::debug("LASER notify-click DOWN menu='{}' at ({:.1f},{:.1f})",
						    s_planeMenuName, targetX, targetY);
					} else if (raceMenuOpen && raceMenuTargetHit) {
						if (raceMenuTarget.kind == RaceMenuLaserTargetKind::kSculptCanvas) {
							double localX = 0.0;
							double localY = 0.0;
							if (BeginRaceMenuSculptStroke(*laserMovie, raceMenuTarget,
							        targetX, targetY, localX, localY)) {
								s_raceSculptDisplay = raceMenuTarget.owner;
								s_raceSculptForeground = raceMenuTarget.clip;
								s_raceSculptLastX = localX;
								s_raceSculptLastY = localY;
								s_pressedMovie = laserMovie;
								s_pressedMovieX = targetX;
								s_pressedMovieY = targetY;
								s_mouseHeld = true;
								s_pressedMovieUsesNotifyMouse = false;
								s_pressedMovieUsesSculpt = true;
								SKSE::log::debug(
								    "LASER RaceMenu sculpt BEGIN local({:.1f},{:.1f}) viewport({:.1f},{:.1f})",
								    localX, localY, targetX, targetY);
							} else {
								SKSE::log::info(
								    "LASER RaceMenu sculpt BEGIN rejected viewport({:.1f},{:.1f})",
								    targetX, targetY);
							}
						} else {
							RE::GFxValue dragSlider;
							if (ActivateRaceMenuLaserTarget(*laserMovie, raceMenuTarget,
							        targetX, targetY, &dragSlider)) {
								s_raceSliderDragging =
								    raceMenuTarget.kind == RaceMenuLaserTargetKind::kSlider;
								if (s_raceSliderDragging)
									s_raceDragSlider = dragSlider;
								s_verticalScrollBarDragging =
								    raceMenuTarget.kind == RaceMenuLaserTargetKind::kScrollBar;
								if (s_verticalScrollBarDragging)
									s_verticalDragScrollBar = dragSlider;
								s_mouseHeld = false;
								s_pressedMovie = nullptr;
								s_pressedMovieUsesNotifyMouse = false;
								SKSE::log::debug(
								    "LASER RaceMenu semantic ACTIVATE kind={} index={} at ({:.1f},{:.1f})",
								    static_cast<int>(raceMenuTarget.kind), raceMenuTarget.index,
								    targetX, targetY);
							}
						}
					} else if (raceMenuOpen) {
						laserMovie->NotifyMouseState(targetX, targetY, 1u, 0);
						s_pressedMovie = laserMovie;
						s_pressedMovieX = targetX;
						s_pressedMovieY = targetY;
						s_mouseHeld = true;
						s_pressedMovieUsesNotifyMouse = true;
						SKSE::log::debug(
						    "LASER {} notify-click DOWN at ({:.1f},{:.1f})",
						    raceMenuOpen ? "RaceMenu" : "Alternate Perspective",
						    targetX, targetY);
					} else {
						RE::GFxMouseEvent down(RE::GFxEvent::EventType::kMouseDown, 0,
						    targetX, targetY);
						laserMovie->HandleEvent(down);
						s_pressedMovie = laserMovie;
						s_pressedMovieX = targetX;
						s_pressedMovieY = targetY;
						s_mouseHeld = true;
						s_pressedMovieUsesNotifyMouse = false;
						SKSE::log::debug("LASER gfx-click DOWN menu='{}' at ({:.1f},{:.1f})",
						    s_planeMenuName, targetX, targetY);
					}
				}
			}
			if (s_clickArmed && releaseSeq != s_lastReleaseSeq) {
				s_lastReleaseSeq = releaseSeq;
				s_raceDragSlider.SetUndefined();
				s_raceSliderDragging = false;
				s_verticalDragScrollBar.SetUndefined();
				s_verticalScrollBarDragging = false;
				if (s_mouseHeld || s_pressedMovie)
					releasePressedMovie("trigger release");
			}
		} else {
			if (s_semanticProbe.valid)
				s_semanticProbe = SemanticProbeCache{};
			s_lastGfxDriveTick = 0;
			s_lastPressSeq = g_pTransform->laserPressSeq;
			s_lastReleaseSeq = g_pTransform->laserReleaseSeq;
			if (s_mouseHeld || s_pressedMovie)
				releasePressedMovie("left menu surface");
			if (s_cursorShown && mc) {
				mc->SetCursorVisibility(false);
				s_cursorShown = false;
			}
			s_laserOwnsFocus = false;
			s_gfxMousePrimed = false;
			s_messageBoxHoveredButton = -1;
			s_lastCurX = s_lastCurY = -1.0f;
			s_lastSentDx = s_lastSentDy = 0.0f;
		}
	}

	// =========================================================================
	// Console world-ref selection: while the console is open, a physics ray
	// from the UI pointer node (the game's own menu beam; wand fallback) picks
	// whatever TESObjectREFR you point at — NPCs, items, doors, clutter, at
	// range, anywhere around you — and makes it the console's selected ref,
	// printing 'Name' (FormID) once per new target. No quad, no Scaleform,
	// no mapping: accuracy is havok-exact. Runs on the game thread at ~10Hz.
	// =========================================================================
	void ConsoleRefPickOnce()
	{
		static RE::FormID s_lastPicked = 0;
		static int s_diagLogsLeft = 2;

		auto ui = RE::UI::GetSingleton();
		if (!ui || !ui->IsMenuOpen(RE::Console::MENU_NAME)) {
			s_lastPicked = 0; // Fresh console session = fresh pick announcements
			return;
		}

		auto pc = RE::PlayerCharacter::GetSingleton();
		if (!pc || !pc->Is3DLoaded())
			return;
		auto vrData = pc->GetVRNodeData();
		if (!vrData)
			return;

		// The game's own UI pointer node carries the exact beam the player
		// sees in menus; the raw wand node is the fallback if it's absent.
		RE::NiNode* aimNode = vrData->UIPointerNode.get();
		if (!aimNode)
			aimNode = vrData->RightWandNode.get();
		if (!aimNode)
			return;

		const RE::NiPoint3 from = aimNode->world.translate;
		RE::NiPoint3 dir = MatColumn(aimNode->world.rotate, 1); // local +Y = beam forward
		float dm = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
		if (dm < 1e-4f)
			return;
		dir /= dm;

		auto cell = pc->GetParentCell();
		auto world = cell ? cell->GetbhkWorld() : nullptr;
		if (!world)
			return;

		constexpr float kRange = 8192.0f; // ~115m; console picks should reach across a scene
		const float hkScale = RE::bhkWorld::GetWorldScale();
		RE::bhkPickData pick;
		pick.rayInput.from = RE::hkVector4(RE::NiPoint3(from.x * hkScale, from.y * hkScale, from.z * hkScale));
		pick.rayInput.to = RE::hkVector4(RE::NiPoint3(
		    (from.x + dir.x * kRange) * hkScale,
		    (from.y + dir.y * kRange) * hkScale,
		    (from.z + dir.z * kRange) * hkScale));
		pick.rayInput.enableShapeCollectionFilter = false;
		pick.rayInput.filterInfo.filter = static_cast<uint32_t>(RE::COL_LAYER::kLOS);

		RE::TESObjectREFR* hitRef = nullptr;
		{
			RE::BSReadLockGuard lock(world->worldLock);
			if (world->PickObject(pick) && pick.rayOutput.HasHit() && pick.rayOutput.rootCollidable)
				hitRef = RE::TESHavokUtilities::FindCollidableRef(*pick.rayOutput.rootCollidable);
		}

		if (s_diagLogsLeft > 0) {
			s_diagLogsLeft--;
			SKSE::log::debug("Console pick: node={} from({:.0f},{:.0f},{:.0f}) dir({:.2f},{:.2f},{:.2f}) hit={:08X}",
			    vrData->UIPointerNode ? "UIPointer" : "RightWand",
			    from.x, from.y, from.z, dir.x, dir.y, dir.z,
			    hitRef ? hitRef->GetFormID() : 0);
		}

		// Terrain, sky, or own body: keep the current selection rather than
		// clearing it — mid-command retargeting on a stray sweep is worse.
		if (!hitRef || hitRef == pc)
			return;
		if (hitRef->GetFormID() == s_lastPicked)
			return;
		s_lastPicked = hitRef->GetFormID();

		if (auto console = ui->GetMenu<RE::Console>())
			console->SetSelectedRef(hitRef);
		if (auto clog = RE::ConsoleLog::GetSingleton()) {
			const char* name = hitRef->GetDisplayFullName();
			clog->Print("'%s' (%08X)", (name && name[0]) ? name : hitRef->GetName(), hitRef->GetFormID());
		}
	}

	// Full 3D console selection driven by the exact OpenXR controller rays.
	// Unlike ConsoleRefPickOnce above, this never derives aim from UIPointerNode
	// and never changes selection merely because the pointer moved.
	void ConsoleWorldPickOnce()
	{
		static bool s_sessionActive = false;
		static uint32_t s_lastFrameSequence = 0;
		static uint32_t s_lastTriggerSequence[2] = {};
		static uint32_t s_selectedFormId = 0;
		static int s_diagLogsLeft = 0;

		auto publishNoHits = []() {
			if (!g_pConsoleLaser)
				return;
			InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_pConsoleLaser->gameSequence));
			MemoryBarrier();
			for (int side = 0; side < 2; ++side) {
				g_pConsoleLaser->hitValid[side] = 0;
				g_pConsoleLaser->hitFormId[side] = 0;
				g_pConsoleLaser->hitDistanceMeters[side] = 0.0f;
			}
			MemoryBarrier();
			InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_pConsoleLaser->gameSequence));
		};

		auto pc = RE::PlayerCharacter::GetSingleton();
		auto vrData = pc ? pc->GetVRNodeData() : nullptr;
		if (g_mapMenuOpen || !g_consoleOpen.load(std::memory_order_acquire)) {
			if (s_sessionActive) {
				publishNoHits();
				SKSE::log::debug("Console world laser inactive; OCU hit state cleared");
			}
			s_sessionActive = false;
			s_lastFrameSequence = 0;
			s_selectedFormId = 0;
			return;
		}

		if (!g_pConsoleLaser || !pc || !pc->Is3DLoaded() || !vrData)
			return;

		// Skyrim owns native cursor and UIPointerGeo visibility. Hiding either
		// here can leak into MapMenu after console close. Only manage OCU's ray.

		uint8_t rayValid[2] = {};
		float rayOriginFromHmd[2][3] = {};
		float rayDirection[2][3] = {};
		uint32_t triggerSequence[2] = {};
		uint32_t frameSequence = 0;
		bool snapshotOk = false;
		for (int attempt = 0; attempt < 3; ++attempt) {
			uint32_t seq1 = g_pConsoleLaser->runtimeSequence;
			if (seq1 & 1)
				continue;
			MemoryBarrier();
			frameSequence = g_pConsoleLaser->frameSequence;
			for (int side = 0; side < 2; ++side) {
				rayValid[side] = g_pConsoleLaser->rayValid[side];
				triggerSequence[side] = g_pConsoleLaser->triggerPressSequence[side];
				for (int axis = 0; axis < 3; ++axis) {
					rayOriginFromHmd[side][axis] = g_pConsoleLaser->rayOriginFromHmd[side][axis];
					rayDirection[side][axis] = g_pConsoleLaser->rayDirection[side][axis];
				}
			}
			MemoryBarrier();
			uint32_t seq2 = g_pConsoleLaser->runtimeSequence;
			if (seq1 == seq2) {
				snapshotOk = true;
				break;
			}
		}
		if (!snapshotOk)
			return;

		if (!s_sessionActive) {
			s_sessionActive = true;
			s_diagLogsLeft = 4;
			s_lastFrameSequence = 0;
			for (int side = 0; side < 2; ++side)
				s_lastTriggerSequence[side] = triggerSequence[side];
			SKSE::log::debug("Console world laser armed: OpenXR -> RoomNode -> Havok");
		}
		if (frameSequence == s_lastFrameSequence)
			return;
		s_lastFrameSequence = frameSequence;

		RE::NiNode* roomNode = vrData->RoomNode.get();
		RE::NiNode* hmdNode = vrData->UprightHmdNode.get();
		if (!roomNode || !hmdNode || !std::isfinite(roomNode->world.scale) ||
		    fabsf(roomNode->world.scale) < 1e-5f) {
			publishNoHits();
			return;
		}

		RE::NiTransform hmdToRoom;
		RE::NiPoint3 hmdRoomPos;
		if (BuildLocalToAncestor(hmdNode, roomNode, hmdToRoom)) {
			hmdRoomPos = hmdToRoom.translate;
		} else {
			RE::NiPoint3 hmdWorldDelta = hmdNode->world.translate - roomNode->world.translate;
			hmdRoomPos = TransposeMul(roomNode->world.rotate, hmdWorldDelta);
			hmdRoomPos /= roomNode->world.scale;
		}

		auto cell = pc->GetParentCell();
		auto world = cell ? cell->GetbhkWorld() : nullptr;
		if (!world) {
			publishNoHits();
			return;
		}

		constexpr float kRangeUnits = 8192.0f;
		constexpr float kStartOffsetUnits = 4.0f;
		const float hkScale = RE::bhkWorld::GetWorldScale();
		const float roomScaleAbs = fabsf(roomNode->world.scale);
		bool hitValid[2] = {};
		float hitDistanceMeters[2] = {};
		uint32_t hitFormId[2] = {};
		RE::TESObjectREFR* hitRefs[2] = {};

		for (int side = 0; side < 2; ++side) {
			if (!rayValid[side])
				continue;
			bool finiteRay = true;
			for (int axis = 0; axis < 3; ++axis) {
				finiteRay = finiteRay && std::isfinite(rayOriginFromHmd[side][axis]) &&
				    std::isfinite(rayDirection[side][axis]);
			}
			if (!finiteRay)
				continue;

			// Inverse of MapSkyrimToXr: XR (X right, Y up, Z back)
			// becomes RoomNode-local (X right, Y forward, Z up).
			RE::NiPoint3 originDeltaRoom = {
				rayOriginFromHmd[side][0] * kSkyrimUnitsPerMeter,
				-rayOriginFromHmd[side][2] * kSkyrimUnitsPerMeter,
				rayOriginFromHmd[side][1] * kSkyrimUnitsPerMeter
			};
			RE::NiPoint3 directionRoom = {
				rayDirection[side][0],
				-rayDirection[side][2],
				rayDirection[side][1]
			};
			RE::NiPoint3 from = roomNode->world * (hmdRoomPos + originDeltaRoom);
			RE::NiPoint3 dir = roomNode->world.rotate * directionRoom;
			float dm = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
			if (!std::isfinite(dm) || dm < 1e-4f)
				continue;
			dir /= dm;
			RE::NiPoint3 rayStart = from + dir * kStartOffsetUnits;

			RE::bhkPickData pick;
			pick.rayInput.from = RE::hkVector4(RE::NiPoint3(
				rayStart.x * hkScale, rayStart.y * hkScale, rayStart.z * hkScale));
			pick.rayInput.to = RE::hkVector4(RE::NiPoint3(
				(rayStart.x + dir.x * kRangeUnits) * hkScale,
				(rayStart.y + dir.y * kRangeUnits) * hkScale,
				(rayStart.z + dir.z * kRangeUnits) * hkScale));
			pick.rayInput.enableShapeCollectionFilter = false;
			pick.rayInput.filterInfo.filter = static_cast<uint32_t>(RE::COL_LAYER::kLOS);

			{
				RE::BSReadLockGuard lock(world->worldLock);
				if (world->PickObject(pick) && pick.rayOutput.HasHit()) {
					hitValid[side] = true;
					hitDistanceMeters[side] =
					    (kStartOffsetUnits + pick.rayOutput.hitFraction * kRangeUnits) /
					    (kSkyrimUnitsPerMeter * roomScaleAbs);
					if (pick.rayOutput.rootCollidable)
						hitRefs[side] = RE::TESHavokUtilities::FindCollidableRef(
						    *pick.rayOutput.rootCollidable);
				}
			}
			if (hitRefs[side] == pc) {
				hitRefs[side] = nullptr;
				hitValid[side] = false;
				hitDistanceMeters[side] = 0.0f;
			}
			if (hitRefs[side])
				hitFormId[side] = hitRefs[side]->GetFormID();
		}

		for (int side = 0; side < 2; ++side) {
			if (triggerSequence[side] == s_lastTriggerSequence[side])
				continue;
			s_lastTriggerSequence[side] = triggerSequence[side];
			RE::TESObjectREFR* selected = hitRefs[side];
			if (!selected) {
				SKSE::log::debug("Console {} trigger: no selectable reference under laser",
					side == 0 ? "LEFT" : "RIGHT");
				continue;
			}

			s_selectedFormId = selected->GetFormID();
			if (auto ui = RE::UI::GetSingleton()) {
				if (auto console = ui->GetMenu<RE::Console>())
					console->SetSelectedRef(selected);
			}
			if (auto clog = RE::ConsoleLog::GetSingleton()) {
				const char* displayName = selected->GetDisplayFullName();
				const char* name = (displayName && displayName[0]) ? displayName : selected->GetName();
				auto base = selected->GetBaseObject();
				clog->Print("'%s'  RefID: %08X  BaseID: %08X",
					(name && name[0]) ? name : "<unnamed>",
					selected->GetFormID(), base ? base->GetFormID() : 0);
			}
			SKSE::log::debug("Console {} trigger selected ref {:08X}",
				side == 0 ? "LEFT" : "RIGHT", s_selectedFormId);
		}

		InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_pConsoleLaser->gameSequence));
		MemoryBarrier();
		for (int side = 0; side < 2; ++side) {
			g_pConsoleLaser->hitValid[side] = hitValid[side] ? 1 : 0;
			g_pConsoleLaser->hitDistanceMeters[side] = hitDistanceMeters[side];
			g_pConsoleLaser->hitFormId[side] = hitFormId[side];
		}
		g_pConsoleLaser->selectedFormId = s_selectedFormId;
		MemoryBarrier();
		InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_pConsoleLaser->gameSequence));

		if (s_diagLogsLeft > 0) {
			--s_diagLogsLeft;
			SKSE::log::info(
				"Console rays: L valid={} hit={:08X} t={:.2f}m | R valid={} hit={:08X} t={:.2f}m",
				rayValid[0], hitFormId[0], hitDistanceMeters[0],
				rayValid[1], hitFormId[1], hitDistanceMeters[1]);
		}
	}

	// Scheduler: posts the pump onto the game thread while menus are active.
	// Single AddTask per tick (never self-requeueing, so no same-frame loops).
	void StartLaserPumpScheduler()
	{
		if (g_laserPumpRunning.exchange(true))
			return;
		std::thread([]() {
			int rtRefreshTick = 0;
			int consolePickTick = 0;
			while (g_laserPumpRunning.load()) {
				if (g_pTransform && g_pTransform->active &&
				    !g_laserPumpTaskPending.exchange(true, std::memory_order_acq_rel)) {
					SKSE::GetTaskInterface()->AddTask([]() {
						LaserCursorPumpOnce();
						g_laserPumpTaskPending.store(false, std::memory_order_release);
					});
				}
				// Match the live controller ray while console is open. When closed,
				// retain a cheap ~10Hz cleanup tick to clear OCU's console hit state.
				if (g_consoleOpen.load(std::memory_order_acquire)) {
					consolePickTick = 0;
					if (!g_consolePickTaskPending.exchange(true, std::memory_order_acq_rel)) {
						SKSE::GetTaskInterface()->AddTask([]() {
							ConsoleWorldPickOnce();
							g_consolePickTaskPending.store(false, std::memory_order_release);
						});
					}
				} else if (++consolePickTick >= 12) {
					consolePickTick = 0;
					if (!g_consolePickTaskPending.exchange(true, std::memory_order_acq_rel)) {
						SKSE::GetTaskInterface()->AddTask([]() {
							ConsoleWorldPickOnce();
							g_consolePickTaskPending.store(false, std::memory_order_release);
						});
					}
				}
				// ~1/sec: re-capture game render targets in case a render-scale
				// mod (Community Shaders VR etc.) recreated them
				if (++rtRefreshTick >= 125) {
					rtRefreshTick = 0;
					if (!g_renderTargetRefreshTaskPending.exchange(true, std::memory_order_acq_rel)) {
						SKSE::GetTaskInterface()->AddTask([]() {
							RefreshBridgeRenderTargets();
							g_renderTargetRefreshTaskPending.store(false, std::memory_order_release);
						});
					}
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(8));
			}
		}).detach();
		SKSE::log::info("Laser cursor pump scheduler started (Scaleform-free path + RT refresh)");
	}

	// =========================================================================
	// Virtual keyboard hook — intercepts Start() to show VR keyboard
	// =========================================================================

	void HookedStart(RE::BSVirtualKeyboardDevice* /*a_self*/, const RE::BSVirtualKeyboardDevice::kbInfo* a_info)
	{
		if (!a_info) {
			SKSE::log::warn("HookedStart called with null kbInfo");
			return;
		}

		// Store callbacks for when the keyboard completes
		// (Fix 2.1: Lock to synchronize with WndProc thread)
		{
			std::lock_guard<std::mutex> lock(g_callbackMutex);
			g_doneCallback = a_info->doneCallback;
			g_cancelCallback = a_info->cancelCallback;
			g_userParam = a_info->userParam;
			g_waitingForKeyboard = true;
		}

		SKSE::log::debug("BSVirtualKeyboardDevice::Start() intercepted");
		SKSE::log::debug("  startingText: omitted");
		SKSE::log::debug("  maxChars: {}", a_info->maxChars);
		SKSE::log::debug("  doneCallback: {:p}", reinterpret_cast<void*>(a_info->doneCallback));
		SKSE::log::debug("  cancelCallback: {:p}", reinterpret_cast<void*>(a_info->cancelCallback));

		// Call ShowKeyboard through the OpenVR overlay interface (Open Composite intercepts this)
		auto overlay = RE::BSOpenVR::GetCleanIVROverlay();
		if (overlay) {
			// Hard limit: 31 characters max for enchanting/naming in Skyrim VR.
			// Bethesda's enchanting table buffer is 32 bytes (31 chars + null).
			// When the game passes maxChars=0 (no limit), enforce 31 anyway to
			// prevent buffer overflows when the text is copied back to the game.
			constexpr uint32_t SKYRIM_HARD_LIMIT = 31;
			uint32_t gameLimit = (a_info->maxChars > 1) ? (a_info->maxChars - 1) : SKYRIM_HARD_LIMIT;
			uint32_t charLimit = (gameLimit < SKYRIM_HARD_LIMIT) ? gameLimit : SKYRIM_HARD_LIMIT;
			SKSE::log::debug("  charLimit: {} (game maxChars: {})", charLimit, a_info->maxChars);

			auto err = overlay->ShowKeyboard(
				vr::k_EGamepadTextInputModeNormal,
				vr::k_EGamepadTextInputLineModeSingleLine,
				"Enter text",                                              // description
				charLimit,                                                 // max chars (hard-capped at 31)
				a_info->startingText ? a_info->startingText : "",          // existing text
				false,                                                     // bUseMinimalMode
				0);                                                        // uUserValue

			if (err != vr::VROverlayError_None) {
				SKSE::log::error("ShowKeyboard failed with error {}", static_cast<int>(err));
				std::lock_guard<std::mutex> lock(g_callbackMutex);
				g_waitingForKeyboard = false;
			} else {
				SKSE::log::debug("VR keyboard shown successfully");
			}
		} else {
			SKSE::log::error("Failed to get IVROverlay interface");
			std::lock_guard<std::mutex> lock(g_callbackMutex);
			g_waitingForKeyboard = false;
		}
	}

	// =========================================================================
	// WndProc hook — keyboard input forwarding + keyboard bridge messages
	// =========================================================================

	// Cast request passed by pointer from the OCU compositor DLL (same process).
	// Layout must match the struct in BaseOverlay.cpp's gestures namespace.
	struct OCGestureCastRequest
	{
		char plugin[128];   // source plugin file name (load-order independent)
		uint32_t formId;    // LOCAL form id within that plugin
		int mode;           // 0 = cast instantly, 1 = equip left hand, 2 = equip right hand, 3 = start held stream, 4 = stop held stream
		int hand;           // casting hand: 0 = left, 1 = right (the hand that drew the gesture)
	};

	LRESULT CALLBACK HookedWndProc(HWND a_hwnd, UINT a_msg, WPARAM a_wParam, LPARAM a_lParam)
	{
		// Suppress ALL WM_CHAR while the VR keyboard is active. Every character
		// the VR keyboard types already reaches Scaleform via PostCharToGame's
		// GFxCharEvent (and Prisma via PrismaVR_DeliverChar), so any WM_CHAR
		// produced from our injected scancodes by TranslateMessage (or by IME/
		// message-loop fixer mods like Dekana's) is a duplicate. This was
		// previously gated on g_consoleOpen, which left PC-mode typing into
		// SkyUI/MCM/Prisma fields double-entering on setups where WM_CHAR flows.
		if (a_msg == WM_CHAR) {
			if ((intptr_t)GetPropW(a_hwnd, L"OC_KB_ACTIVE") != 0) {
				SKSE::log::trace("WM_CHAR suppressed (VR keyboard active): '{}'", (char)a_wParam);
				return 0;
			}
		}

		switch (a_msg) {

		// --- Open Composite keyboard completion signal ---
		// (Fix 2.1: Thread-safe callback handling)
		case WM_OC_KEYBOARD: {
			// wParam 2/3: VR keyboard asks us to SHOW/HIDE the game console
			// directly through the UI queue. Replaces injected tilde keystrokes,
			// which double-toggled on some setups (DirectInput + message-loop
			// mods both acting on the same keystroke). Show/hide is idempotent,
			// so even a duplicated request cannot invert the console state.
			// wParam 6: gesture-fired spell cast/equip. lParam = pointer to an
			// OCGestureCastRequest in the OCU DLL (same process, so the pointer
			// is valid). Copy the fields here, then act on the main thread.
			if (a_wParam == 6 && a_lParam) {
				const auto* req = reinterpret_cast<const OCGestureCastRequest*>(a_lParam);
				std::string plugin(req->plugin, strnlen(req->plugin, sizeof(req->plugin)));
				const uint32_t formId = req->formId;
				const int mode = req->mode;
				const int hand = req->hand;
				SKSE::GetTaskInterface()->AddTask([plugin, formId, mode, hand]() {
					auto* dataHandler = RE::TESDataHandler::GetSingleton();
					auto* player = RE::PlayerCharacter::GetSingleton();
					if (!dataHandler || !player)
						return;
					auto* spell = dataHandler->LookupForm<RE::SpellItem>(formId, plugin);
					if (!spell) {
						SKSE::log::warn("Gesture cast: spell 0x{:X} in '{}' not found", formId, plugin);
						return;
					}
					// Gesture casts come out of the hand that drew the gesture —
					// each hand's built-in caster works with nothing equipped.
					// Discovery from live testing: CastSpellImmediate on a
					// concentration spell doesn't apply one tick, it STARTS a
					// persistent stream the engine keeps flowing (and keeps
					// draining magicka for) until someone stops the caster.
					// So streams are started once and explicitly stopped.
					const auto castSource = hand == 0
					    ? RE::MagicSystem::CastingSource::kLeftHand
					    : RE::MagicSystem::CastingSource::kRightHand;
					auto* handCaster = player->GetMagicCaster(castSource);
					if (!handCaster)
						handCaster = player->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant);

					if (mode == 4) {
						// Stop a held stream (hold button released)
						if (handCaster)
							handCaster->InterruptCast(false);
						SKSE::log::debug("Gesture cast: '{}' stream stopped ({} hand)", spell->GetName(), hand == 0 ? "left" : "right");
						return;
					}
					if (mode == 3) {
						// Start a held stream: fires once; the engine channels
						// and drains magicka until our mode-4 stop arrives.
						auto* avOwner = player->AsActorValueOwner();
						float cost = spell->CalculateMagickaCost(player);
						if (avOwner && cost > 0.0f && avOwner->GetActorValue(RE::ActorValue::kMagicka) < cost * 0.5f) {
							SKSE::log::info("Gesture cast: '{}' fizzled — not enough magicka to start the stream", spell->GetName());
							return;
						}
						if (handCaster) {
							handCaster->CastSpellImmediate(spell, false, nullptr, 1.0f, false, 0.0f, player);
							SKSE::log::debug("Gesture cast: '{}' streaming from {} hand until release", spell->GetName(), hand == 0 ? "left" : "right");
						}
						return;
					}
					if (mode == 0) {
						// Honest magic: instant casts check and drain magicka
						// like a real cast (powers cost 0 and pass through).
						auto* avOwner = player->AsActorValueOwner();
						float cost = spell->CalculateMagickaCost(player);
						const bool concentration = spell->data.castingType == RE::MagicSystem::CastingType::kConcentration;
						if (avOwner && cost > 0.0f && avOwner->GetActorValue(RE::ActorValue::kMagicka) < cost) {
							SKSE::log::info("Gesture cast: '{}' fizzled — not enough magicka ({:.0f} needed)",
								spell->GetName(), cost);
							return;
						}
						// Concentration streams drain per-second on their own;
						// only fire-and-forget casts pay a one-shot cost here.
						if (!concentration && avOwner && cost > 0.0f)
							avOwner->RestoreActorValue(RE::ActorValue::kMagicka, -cost);
						if (handCaster) {
							handCaster->CastSpellImmediate(spell, false, nullptr, 1.0f, false, 0.0f, player);
							if (concentration) {
								// Quick-release burst: let the stream run 2s,
								// then stop the caster (nothing else ever will).
								SKSE::log::debug("Gesture cast: '{}' streaming a 2s burst from {} hand", spell->GetName(), hand == 0 ? "left" : "right");
								std::thread([hand]() {
									std::this_thread::sleep_for(std::chrono::seconds(2));
									SKSE::GetTaskInterface()->AddTask([hand]() {
										auto* pl = RE::PlayerCharacter::GetSingleton();
										if (!pl)
											return;
										auto src = hand == 0
										    ? RE::MagicSystem::CastingSource::kLeftHand
										    : RE::MagicSystem::CastingSource::kRightHand;
										if (auto* cst = pl->GetMagicCaster(src))
											cst->InterruptCast(false);
									});
								}).detach();
							} else {
								SKSE::log::debug("Gesture cast: '{}' fired from {} hand ({:.0f} magicka)", spell->GetName(), hand == 0 ? "left" : "right", cost);
							}
						}
					} else {
						// CK equip slots: right hand 0x13F42, left hand 0x13F43.
						// Plain lookup + static_cast: these engine FormIDs are
						// guaranteed BGSEquipSlot, and As<> has template-linkage
						// issues in this CommonLib build.
						auto* slot = static_cast<RE::BGSEquipSlot*>(
							RE::TESForm::LookupByID(mode == 1 ? 0x00013F43 : 0x00013F42));
						RE::ActorEquipManager::GetSingleton()->EquipSpell(player, spell, slot);
						SKSE::log::debug("Gesture cast: '{}' equipped to {} hand", spell->GetName(), mode == 1 ? "left" : "right");
					}
				});
				return 0;
			}

			// wParam 4/5: gesture-fired key DOWN/UP (lParam = DIK scancode).
			// Injected into Skyrim's own input event queue on the main thread,
			// so SKSE hotkey listeners (Prisma, MCM, AIAgent) receive a real
			// ButtonEvent regardless of window focus or SendInput filtering.
			if (a_wParam == 4 || a_wParam == 5) {
				const int scancode = static_cast<int>(a_lParam);
				const bool down = (a_wParam == 4);
				SKSE::GetTaskInterface()->AddTask([scancode, down]() {
					auto* queue = RE::BSInputEventQueue::GetSingleton();
					if (queue)
						queue->AddButtonEvent(RE::INPUT_DEVICE::kKeyboard, 0, scancode,
							down ? 1.0f : 0.0f, down ? 0.0f : 0.06f);
				});
				SKSE::log::debug("Gesture key {} scancode 0x{:X} queued into game input", down ? "DOWN" : "UP", scancode);
				return 0;
			}

			if (a_wParam == 2 || a_wParam == 3) {
				const bool show = (a_wParam == 2);
				SKSE::GetTaskInterface()->AddUITask([show]() {
					auto* queue = RE::UIMessageQueue::GetSingleton();
					auto* strings = RE::InterfaceStrings::GetSingleton();
					if (queue && strings) {
						queue->AddMessage(strings->console,
							show ? RE::UI_MESSAGE_TYPE::kShow : RE::UI_MESSAGE_TYPE::kHide,
							nullptr);
					}
				});
				SKSE::log::debug("Console {} requested by VR keyboard", show ? "SHOW" : "HIDE");
				return 0;
			}

			// Copy callback state under lock, then invoke outside lock to avoid deadlock
			DoneCallback_t* doneCb = nullptr;
			CancelCallback_t* cancelCb = nullptr;
			void* userParam = nullptr;
			bool wasWaiting = false;

			{
				std::lock_guard<std::mutex> lock(g_callbackMutex);
				wasWaiting = g_waitingForKeyboard;
				if (wasWaiting) {
					doneCb = g_doneCallback;
					cancelCb = g_cancelCallback;
					userParam = g_userParam;
					// Clear state
					g_waitingForKeyboard = false;
					g_doneCallback = nullptr;
					g_cancelCallback = nullptr;
					g_userParam = nullptr;
				}
			}

			// Invoke callbacks outside the lock
			if (wasWaiting) {
				if (a_wParam == 1 && doneCb) {
					// Keyboard Done — retrieve text and invoke callback
					char text[512] = {};
					auto overlay = RE::BSOpenVR::GetCleanIVROverlay();
					if (overlay) {
						overlay->GetKeyboardText(text, sizeof(text));
					}

					SKSE::log::debug("Keyboard done (entered text omitted)");
					doneCb(userParam, text);
				} else if (a_wParam == 0 && cancelCb) {
					// Keyboard Cancelled
					SKSE::log::debug("Keyboard cancelled");
					cancelCb();
				}
			}
			return 0;
		}

		// ========================================================================
		// [EXPERIMENTAL — DISABLED] VR Laser -> Scaleform mouse injection
		// Disabled because injecting GFxMouseEvent / NotifyMouseState / GetMovieDef
		// into Scaleform from SKSE causes the Sovngarde bug: accessing the wrong
		// menu's Scaleform movie (especially StatsMenu) permanently corrupts VR
		// rendering. The menu stack is unpredictable and one wrong access ruins it.
		//
		// Future approach: DLL-side SetCursorPos (maps laser UV to Windows cursor
		// position) avoids Scaleform entirely. No SKSE involvement for mouse.
		// ========================================================================
		// case WM_OC_LASER: {
		// 	float u = static_cast<float>(LOWORD(a_wParam)) / 10000.0f;
		// 	float v = static_cast<float>(HIWORD(a_wParam)) / 10000.0f;
		// 	int action = static_cast<int>(a_lParam & 0xFF);
		// 	bool showCursor = (a_lParam & 0x100) != 0;
		// 	auto ui = RE::UI::GetSingleton();
		// 	if (!ui) break;
		// 	if (g_activeTrackedMenus.empty()) break;
		// 	const char* targetMenuName;
		// 	if (g_activeTrackedMenus.count("CustomMenu") > 0)
		// 		targetMenuName = "CustomMenu";
		// 	else {
		// 		targetMenuName = nullptr;
		// 		for (auto& m : g_activeTrackedMenus) {
		// 			if (m != "TweenMenu") { targetMenuName = m.c_str(); break; }
		// 		}
		// 		if (!targetMenuName)
		// 			targetMenuName = g_activeTrackedMenus.begin()->c_str();
		// 	}
		// 	auto menuPtr = ui->GetMenu(RE::BSFixedString(targetMenuName));
		// 	RE::IMenu* topMenu = menuPtr.get();
		// 	if (!topMenu || !topMenu->uiMovie) break;
		// 	float stageW = 1280.0f, stageH = 720.0f;
		// 	if (g_pTransform && g_pTransform->stageWidth > 0) stageW = g_pTransform->stageWidth;
		// 	if (g_pTransform && g_pTransform->stageHeight > 0) stageH = g_pTransform->stageHeight;
		// 	float x = u * stageW;
		// 	float y = v * stageH;
		// 	if (showCursor) {
		// 		uint32_t buttons = 0;
		// 		if (action == 1) buttons = 0x02;
		// 		topMenu->uiMovie->NotifyMouseState(x, y, buttons, 0);
		// 		topMenu->uiMovie->SetMouseCursorCount(1);
		// 		auto* mc = RE::MenuCursor::GetSingleton();
		// 		if (mc) {
		// 			mc->SetCursorVisibility(true);
		// 			mc->cursorPosX = x;
		// 			mc->cursorPosY = y;
		// 		}
		// 	} else {
		// 		if (action == 0) {
		// 			RE::GFxMouseEvent evt(RE::GFxEvent::EventType::kMouseMove, 0, x, y, 0.f, 0);
		// 			topMenu->uiMovie->HandleEvent(evt);
		// 		} else if (action == 1) {
		// 			RE::GFxMouseEvent evt(RE::GFxEvent::EventType::kMouseDown, 0, x, y, 0.f, 0);
		// 			topMenu->uiMovie->HandleEvent(evt);
		// 		} else if (action == 2) {
		// 			RE::GFxMouseEvent evt(RE::GFxEvent::EventType::kMouseUp, 0, x, y, 0.f, 0);
		// 			topMenu->uiMovie->HandleEvent(evt);
		// 		}
		// 	}
		// 	return 0;
		// }

		// --- Open Composite character injection (direct Scaleform bypass) ---
		// lParam == 0: printable character → GFxCharEvent
		// lParam == 1: control key (backspace, enter) → GFxKeyEvent kKeyDown
		case WM_OC_CHAR: {
			auto ui = RE::UI::GetSingleton();
			if (!ui)
				break;

			if (a_lParam == 0) {
				// Printable character — inject as GFxCharEvent
				GFxCharEvent evt(static_cast<std::uint32_t>(a_wParam));
				for (auto& menu : ui->menuStack) {
					if (menu && menu->uiMovie)
						menu->uiMovie->HandleEvent(evt);
				}
			} else {
				// Control key — map VK to GFxKey and inject as GFxKeyEvent
				RE::GFxKey::Code gfxKey = RE::GFxKey::kVoidSymbol;
				switch (static_cast<WORD>(a_wParam)) {
				case VK_BACK:   gfxKey = RE::GFxKey::kBackspace; break;
				case VK_RETURN: gfxKey = RE::GFxKey::kReturn;    break;
				case VK_TAB:    gfxKey = RE::GFxKey::kTab;       break;
				case VK_DELETE: gfxKey = RE::GFxKey::kDelete;    break;
				case VK_LEFT:   gfxKey = RE::GFxKey::kLeft;      break;
				case VK_RIGHT:  gfxKey = RE::GFxKey::kRight;     break;
				case VK_UP:     gfxKey = RE::GFxKey::kUp;        break;
				case VK_DOWN:   gfxKey = RE::GFxKey::kDown;      break;
				case VK_ESCAPE: gfxKey = RE::GFxKey::kEscape;    break;
				}
				if (gfxKey != RE::GFxKey::kVoidSymbol) {
					// Send KeyDown followed by KeyUp (SkyUI needs both)
					RE::GFxKeyEvent evtDown(RE::GFxEvent::EventType::kKeyDown,
					    gfxKey, 0, 0, {}, 0);
					RE::GFxKeyEvent evtUp(RE::GFxEvent::EventType::kKeyUp,
					    gfxKey, 0, 0, {}, 0);
					for (auto& menu : ui->menuStack) {
						if (menu && menu->uiMovie) {
							menu->uiMovie->HandleEvent(evtDown);
							menu->uiMovie->HandleEvent(evtUp);
						}
					}
				}
			}
			return 0;
		}

		// [EXPERIMENTAL — future development] VR button forwarding via BSInputEventQueue.
		// Disabled: injecting Oculus device button events while Scaleform is in mouse
		// mode causes Skyrim to crash (mixed input device conflict).
		// case WM_OC_BUTTON: {
		// 	int buttonId = LOWORD(a_wParam);
		// 	int side = HIWORD(a_wParam);
		// 	float value = (a_lParam != 0) ? 1.0f : 0.0f;
		// 	float duration = (a_lParam != 0) ? 0.0f : 0.1f;
		// 	auto queue = RE::BSInputEventQueue::GetSingleton();
		// 	if (!queue) break;
		// 	RE::INPUT_DEVICE device = (side == 0)
		// 	    ? static_cast<RE::INPUT_DEVICE>(6)
		// 	    : static_cast<RE::INPUT_DEVICE>(5);
		// 	if (buttonId == 0) {
		// 		SKSE::log::info("OC_BUTTON: side={} btn=AppMenu val={} dev={}",
		// 		    side, value, static_cast<int>(device));
		// 		queue->AddButtonEvent(device, 0, 0x01, value, duration);
		// 	} else if (buttonId == 1) {
		// 		SKSE::log::info("OC_BUTTON: side={} btn=A val={} dev={}",
		// 		    side, value, static_cast<int>(device));
		// 		queue->AddButtonEvent(device, 0, 0x02, value, duration);
		// 	}
		// 	return 0;
		// }
		}

		return CallWindowProcW(g_originalWndProc, a_hwnd, a_msg, a_wParam, a_lParam);
	}

	// =========================================================================
	// Hook installation
	// =========================================================================

	void InstallWndProcHook()
	{
		auto renderer = RE::BSGraphics::Renderer::GetSingleton();
		if (!renderer) {
			SKSE::log::error("Failed to get Renderer singleton");
			return;
		}

		auto& runtimeData = renderer->GetRuntimeData();
		HWND  hwnd = reinterpret_cast<HWND>(runtimeData.renderWindows[0].hWnd);

		if (!hwnd) {
			SKSE::log::error("Failed to get game window handle");
			return;
		}

		g_gameHwnd = hwnd; // Cache for SetProp usage

		g_originalWndProc = reinterpret_cast<WNDPROC>(
			SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HookedWndProc)));

		if (g_originalWndProc) {
			g_hooked = true;
			SKSE::log::info("WndProc hooked successfully (original: {:p})",
				reinterpret_cast<void*>(g_originalWndProc));
		} else {
			SKSE::log::error("Failed to hook WndProc (error: {})", GetLastError());
		}
	}

	void InstallVirtualKeyboardHook()
	{
		// Observe only the game's naming confirmation, rather than guessing that
		// any OK button over RaceMenu is permission to open a keyboard.
		REL::Relocation<std::uintptr_t> namingVtable{ RE::ConfirmAndNameCallback::VTABLE[0] };
		auto* namingSlot = reinterpret_cast<std::uintptr_t*>(namingVtable.address()) + 1;
		DWORD namingProtect = 0;
		if (VirtualProtect(namingSlot, sizeof(*namingSlot), PAGE_EXECUTE_READWRITE, &namingProtect)) {
			g_originalConfirmAndNameRun = reinterpret_cast<ConfirmAndNameRun>(*namingSlot);
			*namingSlot = reinterpret_cast<std::uintptr_t>(&HookedConfirmAndName);
			VirtualProtect(namingSlot, sizeof(*namingSlot), namingProtect, &namingProtect);
			SKSE::log::info("RaceMenu ConfirmAndName callback observed for laser keyboard recovery");
		} else {
			SKSE::log::error("Could not observe RaceMenu ConfirmAndName callback (error: {})", GetLastError());
		}

		auto inputMgr = RE::BSInputDeviceManager::GetSingleton();
		if (!inputMgr) {
			SKSE::log::error("Failed to get BSInputDeviceManager singleton");
			return;
		}

		auto vkbd = inputMgr->GetVirtualKeyboard();
		if (!vkbd) {
			SKSE::log::error("Failed to get virtual keyboard device");
			return;
		}

		SKSE::log::debug("Virtual keyboard device found at {:p}", reinterpret_cast<void*>(vkbd));

		// Get vtable pointer
		auto vtable = *reinterpret_cast<std::uintptr_t**>(vkbd);
		SKSE::log::debug("VTable at {:p}", reinterpret_cast<void*>(vtable));

		// Slot 0x0B = Start() virtual function
		constexpr std::size_t kStartSlot = 0x0B;

		// Save original (should be a no-op, but save it anyway)
		g_originalStart = reinterpret_cast<Start_t>(vtable[kStartSlot]);
		SKSE::log::debug("Original Start() at {:p}", reinterpret_cast<void*>(g_originalStart));

		// Patch vtable to point to our hook
		DWORD oldProtect = 0;
		if (VirtualProtect(&vtable[kStartSlot], sizeof(std::uintptr_t), PAGE_EXECUTE_READWRITE, &oldProtect)) {
			vtable[kStartSlot] = reinterpret_cast<std::uintptr_t>(&HookedStart);
			VirtualProtect(&vtable[kStartSlot], sizeof(std::uintptr_t), oldProtect, &oldProtect);
			SKSE::log::debug("BSVirtualKeyboardDevice::Start() hooked successfully");
		} else {
			SKSE::log::error("Failed to VirtualProtect vtable for Start() hook (error: {})", GetLastError());
		}
	}

	// =========================================================================
#include "DapaPlayerMask.inl"

	// =========================================================================
	// [EXPERIMENTAL — DISABLED] WASD+E Keyboard Remap
	// =========================================================================
	// PROBLEM: Pressing W, A, S, D, or E while using the VR keyboard causes
	// CTDs because these keys trigger game actions (movement, activate)
	// simultaneously with text input, corrupting game state.
	//
	// ORIGINAL SOLUTION (below): Remap WASD+E to obscure keys (Home/End/PgUp/
	// PgDn/Delete) in-memory so they can be typed safely on the VR keyboard.
	// VR players use joysticks for movement anyway.
	//
	// WHY DISABLED: The proper solution is the Bindings tab in the
	// OpenComposite Configurator app, which lets the user edit controlmapvr.txt
	// directly (20-field VR format). That gives full control over keyboard AND
	// controller bindings per input context, rather than this hacky in-memory
	// override that only covers 5 keys in one context.
	//
	// NOTE FOR FUTURE DEVELOPERS: This CTD is real and will affect any VR
	// keyboard implementation. The Configurator's Bindings tab is the correct
	// fix — it writes a proper controlmapvr.txt with WASD+E rebound to unused
	// scancodes across all relevant input contexts.
	// =========================================================================

	void RemapMovementKeys()
	{
		auto* controlMap = RE::ControlMap::GetSingleton();
		if (!controlMap) {
			SKSE::log::warn("RemapMovementKeys: ControlMap singleton not available");
			return;
		}

		// Get the Gameplay input context
		auto* gameplayContext = controlMap->controlMap[RE::UserEvents::INPUT_CONTEXT_ID::kGameplay];
		if (!gameplayContext) {
			SKSE::log::warn("RemapMovementKeys: Gameplay context not found");
			return;
		}

		// Keyboard device mappings
		auto& keyboardMappings = gameplayContext->deviceMappings[RE::INPUT_DEVICE::kKeyboard];

		// DirectInput scancodes for new keys
		constexpr std::uint16_t kHome     = 0xC7;  // Home
		constexpr std::uint16_t kEnd      = 0xCF;  // End
		constexpr std::uint16_t kPageUp   = 0xC9;  // Page Up
		constexpr std::uint16_t kPageDown = 0xD1;  // Page Down
		constexpr std::uint16_t kDelete   = 0xD3;  // Delete

		// Remap table: action name -> new scancode
		struct RemapEntry {
			const char* eventName;
			std::uint16_t newKey;
		};
		constexpr RemapEntry remaps[] = {
			{ "Forward",      kHome },
			{ "Back",         kEnd },
			{ "Strafe Left",  kPageUp },
			{ "Strafe Right", kPageDown },
			{ "Activate",     kDelete },
		};

		int remapped = 0;
		for (auto& mapping : keyboardMappings) {
			for (const auto& remap : remaps) {
				if (mapping.eventID == remap.eventName) {
					SKSE::log::debug("RemapMovementKeys: {} (0x{:02X} -> 0x{:02X})",
						remap.eventName, mapping.inputKey, remap.newKey);
					mapping.inputKey = remap.newKey;
					remapped++;
					break;
				}
			}
		}

		SKSE::log::debug("RemapMovementKeys: Remapped {} keyboard bindings (WASD+E freed for typing)", remapped);
	}

	// =========================================================================
	// SKSE message handler
	// =========================================================================

	// =========================================================================
	// Gesture spell support
	// =========================================================================

	// Dump every castable spell and power in the load order so the Configurator
	// can offer a searchable picker. Written to the SKSE folder in My Games
	// (survives Root Builder cleanup; refreshed every game launch).
	void DumpGestureSpellList()
	{
		auto* dataHandler = RE::TESDataHandler::GetSingleton();
		if (!dataHandler)
			return;
		auto dir = SKSE::log::log_directory();
		if (!dir)
			return;

		// Spells the player character actually knows (race/base + learned).
		// Empty at kDataLoaded (no save yet); filled on the post-load re-dump.
		std::set<RE::FormID> known;
		if (auto* player = RE::PlayerCharacter::GetSingleton()) {
			if (auto* base = player->GetActorBase()) {
				if (auto* effects = base->actorEffects) {
					for (uint32_t i = 0; i < effects->numSpells; i++)
						if (effects->spells[i])
							known.insert(effects->spells[i]->GetFormID());
				}
			}
			for (auto* sp : player->GetActorRuntimeData().addedSpells)
				if (sp)
					known.insert(sp->GetFormID());
		}

		auto path = *dir / "OCUGestureSpellList.json";
		std::ofstream out(path);
		if (!out.is_open()) {
			SKSE::log::warn("Gesture spell list: cannot write {}", path.string());
			return;
		}

		auto esc = [](std::string_view s) {
			std::string r;
			for (char c : s) {
				if (c == '"' || c == '\\')
					r += '\\';
				if ((unsigned char)c >= 0x20)
					r += c;
			}
			return r;
		};

		out << "[\n";
		bool first = true;
		int count = 0;
		for (auto* spell : dataHandler->GetFormArray<RE::SpellItem>()) {
			if (!spell)
				continue;
			auto type = spell->GetSpellType();
			if (type != RE::MagicSystem::SpellType::kSpell
				&& type != RE::MagicSystem::SpellType::kPower
				&& type != RE::MagicSystem::SpellType::kLesserPower)
				continue;
			const char* name = spell->GetName();
			if (!name || !name[0])
				continue;
			auto* file = spell->GetFile(0);
			if (!file)
				continue;

			if (!first)
				out << ",\n";
			first = false;
			const char* kind = (type == RE::MagicSystem::SpellType::kSpell) ? "spell" : "power";
			char formHex[16];
			snprintf(formHex, sizeof(formHex), "0x%X", spell->GetLocalFormID());
			const bool conc = spell->data.castingType == RE::MagicSystem::CastingType::kConcentration;
			out << "  {\"name\":\"" << esc(name)
				<< "\",\"plugin\":\"" << esc(file->GetFilename())
				<< "\",\"formId\":\"" << formHex
				<< "\",\"type\":\"" << kind
				<< "\",\"casting\":\"" << (conc ? "conc" : "ff")
				<< "\",\"known\":" << (known.count(spell->GetFormID()) ? "true" : "false") << "}";
			count++;
		}
		out << "\n]\n";
		SKSE::log::info("Gesture spell list: dumped {} spells/powers to {}", count, path.string());
	}

	// =========================================================================
	// Combat haptics: engine hit events -> controller rumble via OCU DLL
	//
	// The DLL (openvr_api.dll) exports OCU_CombatHaptic(hand, kind, micros) and
	// owns all config gating (opencomposite.ini combatHapticShield/Weapon/
	// Strength), so this side only classifies events. Physics-collision mods
	// (PLANCK-style weapon clash) fire OpenVR haptics themselves and already
	// rumble through OCU's normal path; this covers what the ENGINE reports:
	//   - a hit you BLOCKED (shield or weapon) -> thump the blocking hand
	//   - your bash connecting                 -> the bashing hand
	//   - your melee weapon connecting         -> the attacking hand
	//   - your fist connecting                 -> only while actually swinging
	// =========================================================================
	namespace CombatHaptics
	{
		using HapticFn = void(__cdecl*)(int hand, int kind, unsigned int durationMicros);
		HapticFn g_hapticFn = nullptr;

		void ResolveExport()
		{
			if (HMODULE mod = GetModuleHandleA("openvr_api.dll"))
				g_hapticFn = reinterpret_cast<HapticFn>(GetProcAddress(mod, "OCU_CombatHaptic"));
			SKSE::log::info("Combat haptics: OCU_CombatHaptic {}",
				g_hapticFn ? "resolved" : "not found (stock openvr_api.dll?) — feature inactive");
		}

		// 0=left 1=right, -1 unknown. Prefers the engine's current attack data
		// (knows which hand swung, even dual-wielding twins); falls back to
		// matching the hit's source form against equipped gear.
		int AttackingHand(RE::PlayerCharacter* player, RE::FormID source)
		{
			if (auto* proc = player->GetActorRuntimeData().currentProcess) {
				if (proc->high && proc->high->attackData)
					return proc->high->attackData->IsLeftAttack() ? 0 : 1;
			}
			auto* right = player->GetEquippedObject(false);
			auto* left = player->GetEquippedObject(true);
			if (right && right->GetFormID() == source)
				return 1;
			if (left && left->GetFormID() == source)
				return 0;
			return -1;
		}

		class HitSink : public RE::BSTEventSink<RE::TESHitEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent* ev,
				RE::BSTEventSource<RE::TESHitEvent>*) override
			{
				if (!ev || !g_hapticFn)
					return RE::BSEventNotifyControl::kContinue;
				auto* player = RE::PlayerCharacter::GetSingleton();
				if (!player)
					return RE::BSEventNotifyControl::kContinue;

				const bool targetIsPlayer = ev->target && ev->target.get() == player;
				const bool causeIsPlayer = ev->cause && ev->cause.get() == player;

				// Incoming hit you blocked (shield or weapon parry/clash)
				if (targetIsPlayer && ev->flags.any(RE::TESHitEvent::Flag::kHitBlocked)) {
					auto* left = player->GetEquippedObject(true);
					bool leftShield = left && left->IsArmor(); // shields equip to the left hand
					g_hapticFn(leftShield ? 0 : 1, 0, 120000);
					return RE::BSEventNotifyControl::kContinue;
				}

				if (!causeIsPlayer || targetIsPlayer)
					return RE::BSEventNotifyControl::kContinue;

				// Your bash landing (shield bash or weapon bash)
				if (ev->flags.any(RE::TESHitEvent::Flag::kBashAttack)) {
					auto* left = player->GetEquippedObject(true);
					bool leftShield = left && left->IsArmor();
					int hand = leftShield ? 0 : AttackingHand(player, ev->source);
					g_hapticFn(hand < 0 ? 1 : hand, 1, 100000);
					return RE::BSEventNotifyControl::kContinue;
				}

				// Your melee connecting. Projectile hits (arrows, spells) have a
				// projectile form — no impact reaches the hand, skip them.
				if (ev->projectile != 0)
					return RE::BSEventNotifyControl::kContinue;

				auto* src = RE::TESForm::LookupByID(ev->source);
				// As<> has link errors in this CommonLib — form-type check instead
				auto* weap = (src && src->GetFormType() == RE::FormType::Weapon)
					? static_cast<RE::TESObjectWEAP*>(src)
					: nullptr;
				if (!weap || weap->IsBow() || weap->IsCrossbow())
					return RE::BSEventNotifyControl::kContinue;

				if (weap->IsHandToHandMelee()) {
					// Bare fists: only while genuinely mid-swing, so an idle
					// empty hand brushing something never buzzes.
					auto* state = player->AsActorState();
					if (!state || state->GetAttackState() == RE::ATTACK_STATE_ENUM::kNone)
						return RE::BSEventNotifyControl::kContinue;
					int hand = AttackingHand(player, ev->source);
					g_hapticFn(hand < 0 ? 1 : hand, 2, 45000);
					return RE::BSEventNotifyControl::kContinue;
				}

				int hand = AttackingHand(player, ev->source);
				g_hapticFn(hand < 0 ? 1 : hand, 1, 80000);
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		// Fire-and-forget spell release (Incinerate etc.) -> pulse the casting
		// hand, both on a dual cast. Concentration spells are skipped here;
		// the stream pump below gives them a continuous low rumble instead.
		class SpellSink : public RE::BSTEventSink<RE::TESSpellCastEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::TESSpellCastEvent* ev,
				RE::BSTEventSource<RE::TESSpellCastEvent>*) override
			{
				if (!ev || !g_hapticFn)
					return RE::BSEventNotifyControl::kContinue;
				auto* player = RE::PlayerCharacter::GetSingleton();
				if (!player || !ev->object || ev->object.get() != player)
					return RE::BSEventNotifyControl::kContinue;

				auto* form = RE::TESForm::LookupByID(ev->spell);
				auto* spell = (form && form->GetFormType() == RE::FormType::Spell)
					? static_cast<RE::SpellItem*>(form)
					: nullptr;
				if (!spell || spell->data.castingType == RE::MagicSystem::CastingType::kConcentration)
					return RE::BSEventNotifyControl::kContinue;

				auto* left = player->GetEquippedObject(true);
				auto* right = player->GetEquippedObject(false);
				bool l = left && left->GetFormID() == ev->spell;
				bool r = right && right->GetFormID() == ev->spell;
				if (!l && !r)
					return RE::BSEventNotifyControl::kContinue; // shout/scroll: not a hand cast
				if (l)
					g_hapticFn(0, 4, 70000);
				if (r)
					g_hapticFn(1, 4, 70000);
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		// Arrow release: the behavior graph fires "arrowRelease" the moment
		// the string lets go. Light snap in BOTH hands (bow arm feels the
		// limbs, draw hand the string).
		class AnimSink : public RE::BSTEventSink<RE::BSAnimationGraphEvent>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(const RE::BSAnimationGraphEvent* ev,
				RE::BSTEventSource<RE::BSAnimationGraphEvent>*) override
			{
				if (!ev || !g_hapticFn)
					return RE::BSEventNotifyControl::kContinue;
				if (ev->tag == "arrowRelease") {
					g_hapticFn(0, 3, 50000);
					g_hapticFn(1, 3, 50000);
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		// The player's animation graph only exists once a save is up, so this
		// runs at kPostLoadGame/kNewGame. AddAnimationGraphEventSink dedupes,
		// so re-calling on every load is safe.
		void RegisterAnimSink()
		{
			if (!g_hapticFn)
				return;
			static AnimSink sink;
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (player && player->AddAnimationGraphEventSink(&sink))
				SKSE::log::info("Combat haptics: arrowRelease anim sink registered");
		}

		// Concentration stream rumble: ~8Hz poll of both hand casters; while a
		// concentration spell is flowing, re-trigger a low 150ms pulse. The
		// overlap (150ms pulse every 120ms) reads as one continuous hum.
		void StartMagicPump()
		{
			static std::atomic<bool> started{ false };
			if (started.exchange(true))
				return;
			std::thread([]() {
				while (true) {
					std::this_thread::sleep_for(std::chrono::milliseconds(120));
					if (!g_hapticFn)
						continue;
					SKSE::GetTaskInterface()->AddTask([]() {
						auto* player = RE::PlayerCharacter::GetSingleton();
						if (!player)
							return;
						for (int hand = 0; hand < 2; hand++) {
							auto source = hand == 0 ? RE::MagicSystem::CastingSource::kLeftHand
							                        : RE::MagicSystem::CastingSource::kRightHand;
							auto* caster = player->GetMagicCaster(source);
							if (!caster || caster->state.get() != RE::MagicCaster::State::kCasting)
								continue;
							auto* spell = caster->currentSpell;
							if (spell && spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration)
								g_hapticFn(hand, 5, 150000);
						}
					});
				}
			}).detach();
		}

		void Register()
		{
			ResolveExport();
			if (!g_hapticFn)
				return;
			if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
				holder->AddEventSink<RE::TESHitEvent>(new HitSink());
				holder->AddEventSink<RE::TESSpellCastEvent>(new SpellSink());
				SKSE::log::info("Combat haptics: TESHitEvent + TESSpellCastEvent sinks registered");
			}
			StartMagicPump();
		}
	}

	bool IsDapaEnabledInGameIni()
	{
		wchar_t executablePath[MAX_PATH]{};
		const DWORD pathLength = GetModuleFileNameW(
		    nullptr, executablePath, static_cast<DWORD>(std::size(executablePath)));
		if (pathLength == 0 || pathLength >= std::size(executablePath)) {
			SKSE::log::warn(
			    "DAPA: could not resolve SkyrimVR.exe path; first-person render hooks will stay disabled");
			return false;
		}

		wchar_t* fileName = wcsrchr(executablePath, L'\\');
		if (!fileName) {
			SKSE::log::warn(
			    "DAPA: malformed SkyrimVR.exe path; first-person render hooks will stay disabled");
			return false;
		}
		*(fileName + 1) = L'\0';
		if (wcslen(executablePath) + wcslen(L"opencomposite.ini") >=
		    std::size(executablePath)) {
			SKSE::log::warn(
			    "DAPA: opencomposite.ini path is too long; first-person render hooks will stay disabled");
			return false;
		}
		wcscat_s(executablePath, L"opencomposite.ini");

		wchar_t configuredValue[32]{};
		// OCU's global video settings live under the literal empty [] section.
		// Also accept a future/hand-written [asw] section without changing the
		// current Configurator's file format.
		GetPrivateProfileStringW(L"", L"aswEnabled", L"__missing__",
		    configuredValue, static_cast<DWORD>(std::size(configuredValue)),
		    executablePath);
		if (_wcsicmp(configuredValue, L"__missing__") == 0) {
			GetPrivateProfileStringW(L"asw", L"aswEnabled", L"false",
			    configuredValue, static_cast<DWORD>(std::size(configuredValue)),
			    executablePath);
		}
		return _wcsicmp(configuredValue, L"true") == 0 ||
		       _wcsicmp(configuredValue, L"yes") == 0 ||
		       _wcsicmp(configuredValue, L"on") == 0 ||
		       wcscmp(configuredValue, L"1") == 0;
	}

	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kPostPostLoad:
			SyncDiagnosticLogging();
			DapaHiggs::Connect();
			break;
		case SKSE::MessagingInterface::kPreLoadGame:
			DapaHiggs::SetEnabled(false);
			break;
		case SKSE::MessagingInterface::kDataLoaded:
			SyncDiagnosticLogging();
			DapaSpellWheel::Initialize();
			DapaCrossbow::Initialize();
			SKSE::log::info("Game data loaded, installing hooks");
			InstallWndProcHook();
			InstallVirtualKeyboardHook();
			CreateSharedMemory();
			CreateConsoleLaserBridge();
			CreateRenderTargetBridge();
			if (IsDapaEnabledInGameIni()) {
				SKSE::log::info(
				    "DAPA enabled: installing player-body mask hooks");
				InstallSetupGeometryHook();
			} else {
				SKSE::log::info(
				    "DAPA disabled: player-body mask hooks were not installed");
			}
			FindAndStoreNiCamera();  // Try immediately (may need retry after save/new game)

			// Register menu state watcher for MCM laser pointer system
			if (auto ui = RE::UI::GetSingleton()) {
				ui->AddEventSink<RE::MenuOpenCloseEvent>(new MenuWatcher());
				SKSE::log::info("MenuOpenCloseEvent sink registered");
			}

			// Scaleform-free laser cursor pump (plane export + closed-loop mouse)
			StartLaserPumpScheduler();

			// Spell picker source for the Configurator's gesture actions
			DumpGestureSpellList();

			// Shield-block / weapon-hit controller rumble (gated in the DLL)
			CombatHaptics::Register();

			break;

		case SKSE::MessagingInterface::kPostLoadGame:
		case SKSE::MessagingInterface::kNewGame:
			if (IsDapaEnabledInGameIni()) {
				InstallSetupGeometryHook();
				PlayerMask::NotifyUnavailableAfterLoad();
			}
			DapaHiggs::SetEnabled(PlayerMask::ready.load(std::memory_order_acquire));
			FindAndStoreNiCamera();  // Retry after scene graph is fully loaded
			if (g_diagnosticLogging.load(std::memory_order_relaxed)) TestRendererShadowState();
			DumpGestureSpellList();  // Re-dump with the character's known spells tagged
			CombatHaptics::RegisterAnimSink();  // player anim graph exists now (arrow release)
			break;

		case SKSE::MessagingInterface::kInputLoaded:
			SKSE::log::info("Input loaded");
			if (auto inputManager = RE::BSInputDeviceManager::GetSingleton()) {
				inputManager->AddEventSink(MenuInputIntentWatcher::GetSingleton());
				SKSE::log::info("Menu input-intent watcher registered");
			} else {
				SKSE::log::error("Menu input-intent watcher registration failed: input manager unavailable");
			}
			// [EXPERIMENTAL — DISABLED] RemapMovementKeys() — see comment block above.
			// Replaced by Configurator's Bindings tab (controlmapvr.txt editor).
			// RemapMovementKeys();
			break;
		}
	}

	// =========================================================================
	// Logging setup
	// =========================================================================

	void SetupLogging()
	{
		auto path = SKSE::log::log_directory();
		if (!path)
			return;

		*path /= "OpenCompositeInput.log";

		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
		auto log = std::make_shared<spdlog::logger>("OpenCompositeInput", std::move(sink));
		log->set_level(spdlog::level::info);
		log->flush_on(spdlog::level::warn);
		spdlog::set_default_logger(std::move(log));
		SyncDiagnosticLogging();
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);
	SetupLogging();

	SKSE::log::info("OpenCompositeInput v3.2.0 loaded");
	SKSE::log::info("OCU SKSE package: 4.3.7-custom-eye-test-hotfix4 / DAPA exact-mask-v1 / accepted-draw-api-v1 / held-geometry ownership");
	SKSE::log::info("  VR keyboard bridge + Scaleform char injection + menu state tracking");
		SKSE::log::info("  RaceMenu keyboard test: confirmed-naming-v4 / VR-button-slot-8");
	SKSE::log::info("  + Render target bridge (MV + depth) for FSR 2/3 integration");
	SKSE::log::info("  + Laser cursor pump v2 (Scaleform-free: uiNode plane + BSInputEventQueue)");
	SKSE::log::info("  + Event-driven Scaleform laser input (30 Hz moving, zero idle probes)");

	auto messaging = SKSE::GetMessagingInterface();
	if (!messaging || !messaging->RegisterListener(OnMessage)) {
		SKSE::log::error("Failed to register messaging listener");
		return false;
	}

	SKSE::log::info("Messaging listener registered, waiting for game data load");
	return true;
}
