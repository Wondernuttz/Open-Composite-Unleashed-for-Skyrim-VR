// CommonLibVR headers MUST come before Windows.h (REX/W32/BASE.h enforces this)
#include <RE/B/BSInputDeviceManager.h>
#include <RE/B/BSInputEventQueue.h>
#include <RE/B/BSOpenVR.h>
#include <RE/B/BSVirtualKeyboardDevice.h>
#include <RE/B/BSWin32VirtualKeyboardDevice.h>
#include <RE/B/BSTEvent.h>
#include <RE/C/ControlMap.h>
#include <RE/G/GFxEvent.h>
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
// #include <RE/M/MenuCursor.h>     // Was: SetCursorVisibility, cursorPosX/Y
#include <RE/M/MenuOpenCloseEvent.h>
#include <RE/N/NiCamera.h>
#include <RE/N/NiRTTI.h>
#include <RE/P/PlayerCamera.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/R/Renderer.h>
// BSShaderAccumulator: use raw offsets to avoid header dependency issues.
// VTable REL::VariantID(304459, 254680, 0x18fd880)
// firstPerson bool at offset 0x128
// FinishAccumulating at vtable slot 0x26
#include <RE/U/UI.h>
#include <SKSE/SKSE.h>

#include <spdlog/sinks/basic_file_sink.h>
#include <set>
#include <string>
#include <mutex>

// Win32 API for WndProc hook (REX::W32 doesn't provide CallWindowProcW)
#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <atomic>
#pragma comment(lib, "d3dcompiler.lib")
#include <MinHook.h>

// =========================================================================
// Shared memory struct — read by Open Composite for laser plane positioning
// =========================================================================
#pragma pack(push, 1)
struct OCMenuTransform {
	static constexpr uint32_t MAGIC = 0x54434D4F; // 'OCMT'
	static constexpr uint32_t VERSION = 1;

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

	uint8_t  reserved[64];    // Future use
};
#pragma pack(pop)

// =========================================================================
// Shared memory struct — render target bridge for Open Composite FSR 2/3
// Exposes ID3D11Texture2D* pointers for motion vectors and depth buffer.
// Both DLLs live in the same process, so raw pointers are valid.
// =========================================================================
#pragma pack(push, 1)
struct OCRenderTargetBridge {
	static constexpr uint32_t MAGIC = 0x56544F4D; // 'MOTV'
	static constexpr uint32_t VERSION = 1;

	uint32_t magic;
	uint32_t version;
	uint32_t status;        // 0=not ready, 1=ready, 2=error

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
};
#pragma pack(pop)

namespace
{
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

	// =========================================================================
	// Shared memory for render target bridge (read by Open Composite for FSR)
	// =========================================================================
	HANDLE                g_hBridgeMapFile = nullptr;
	OCRenderTargetBridge* g_pBridge = nullptr;

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
		SKSE::log::info("Shared memory created: Local\\OpenCompositeMenuTransform ({} bytes)", sizeof(OCMenuTransform));
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
		g_pBridge->status = 0; // Not ready yet

		// Access renderer to get render target pointers
		auto renderer = RE::BSGraphics::Renderer::GetSingleton();
		if (!renderer) {
			SKSE::log::error("RT Bridge: Renderer singleton not available");
			g_pBridge->status = 2;
			return;
		}

		auto& runtimeData = renderer->GetRuntimeData();

		// Motion vector render target (enum index 7 = kMOTION_VECTOR)
		auto& mvRT = runtimeData.renderTargets[RE::RENDER_TARGET::kMOTION_VECTOR];
		if (!mvRT.texture) {
			SKSE::log::error("RT Bridge: kMOTION_VECTOR texture is null");
			g_pBridge->status = 2;
			return;
		}

		g_pBridge->mvTexture = reinterpret_cast<uint64_t>(mvRT.texture);
		g_pBridge->mvSRV     = reinterpret_cast<uint64_t>(mvRT.SRV);
		g_pBridge->mvUAV     = reinterpret_cast<uint64_t>(mvRT.UAV);

		SKSE::log::info("RT Bridge: kMOTION_VECTOR texture={:p} SRV={:p} UAV={:p}",
			static_cast<void*>(mvRT.texture),
			static_cast<void*>(mvRT.SRV),
			static_cast<void*>(mvRT.UAV));

		// Depth buffer
		auto& depthData = renderer->GetDepthStencilData();
		auto& mainDepth = depthData.depthStencils[RE::RENDER_TARGET_DEPTHSTENCIL::kMAIN];
		if (!mainDepth.texture) {
			SKSE::log::warn("RT Bridge: kMAIN depth texture is null (MV still available)");
		} else {
			g_pBridge->depthTexture = reinterpret_cast<uint64_t>(mainDepth.texture);
			g_pBridge->depthSRV     = reinterpret_cast<uint64_t>(mainDepth.depthSRV);

			SKSE::log::info("RT Bridge: Depth texture={:p} SRV={:p}",
				static_cast<void*>(mainDepth.texture),
				static_cast<void*>(mainDepth.depthSRV));
		}

		// D3D11 device and context (for receiver to validate same device)
		g_pBridge->d3dDevice  = reinterpret_cast<uint64_t>(runtimeData.forwarder);
		g_pBridge->d3dContext = reinterpret_cast<uint64_t>(runtimeData.context);

		SKSE::log::info("RT Bridge: Device={:p} Context={:p}",
			reinterpret_cast<void*>(runtimeData.forwarder),
			reinterpret_cast<void*>(runtimeData.context));

		// Mark as ready
		g_pBridge->status = 1;
		SKSE::log::info("RT Bridge: Ready — shared memory Local\\OpenCompositeRenderTargets ({} bytes)",
			sizeof(OCRenderTargetBridge));
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
		"MapMenu",
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
		"CustomMenu", // SkyUI MCM host
		// StatsMenu excluded — Sovngarde constellation bug
	};

	// Track which of our target menus are currently open (avoids calling
	// ui->IsMenuOpen from inside the event handler, which deadlocks because
	// Bethesda's UI holds a lock during MenuOpenCloseEvent dispatch).
	std::set<std::string> g_activeTrackedMenus;
	bool g_consoleOpen = false;  // Track console separately for WM_CHAR suppression

	// Update shared memory with the active menu's 3D transform data
	void UpdateMenuTransform()
	{
		if (!g_pTransform)
			return;

		auto ui = RE::UI::GetSingleton();
		if (!ui)
			return;

		bool anyActive = !g_activeTrackedMenus.empty();
		bool gamePaused = ui->GameIsPaused();

		// Begin write (odd counter = writing)
		g_pTransform->updateCounter++;

		// Allow WASD when ANY menu is active OR game is paused (kPausesGame menu like text boxes)
		g_pTransform->active = anyActive || gamePaused;
		SKSE::log::info("  SharedMem write: anyActive={}, gamePaused={}, active={}",
		    anyActive, gamePaused, g_pTransform->active);

		if (!anyActive) {
			g_pTransform->menuName[0] = '\0';
			g_pTransform->hasPerspective = false;
			g_pTransform->updateCounter++; // End write (even = stable)
			return;
		}

		// Write the menu name so the DLL knows which menu is open (for profile
		// quad selection). This does NOT access any Scaleform data — just copies
		// the string from our tracked set.
		// Priority: CustomMenu (MCM) > any content menu > TweenMenu
		const char* menuNameStr;
		if (g_activeTrackedMenus.count("CustomMenu") > 0)
			menuNameStr = "CustomMenu";
		else {
			menuNameStr = nullptr;
			for (auto& m : g_activeTrackedMenus) {
				if (m != "TweenMenu") { menuNameStr = m.c_str(); break; }
			}
			if (!menuNameStr)
				menuNameStr = g_activeTrackedMenus.begin()->c_str();
		}

		strncpy_s(g_pTransform->menuName, menuNameStr, 63);
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

			// Track specific menus for laser pointer system
			bool isTracked = false;
			for (auto& m : kTrackedMenus) {
				if (name == m) {
					isTracked = true;
					break;
				}
			}

			if (isTracked) {
				if (a_event->opening)
					g_activeTrackedMenus.insert(std::string(name));
				else
					g_activeTrackedMenus.erase(std::string(name));

				// Update shared memory whenever tracked menus change
				UpdateMenuTransform();
			}

			// Track console open/close for WM_CHAR suppression
			if (name == "Console")
				g_consoleOpen = a_event->opening;

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
				SKSE::log::info("Menu {} {} - active:{} gamePaused:{}",
				    name, a_event->opening ? "opened" : "closed",
				    !g_activeTrackedMenus.empty(), ui->GameIsPaused());
			}

			return RE::BSEventNotifyControl::kContinue;
		}
	};

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

		SKSE::log::info("BSVirtualKeyboardDevice::Start() intercepted");
		SKSE::log::info("  startingText: \"{}\"", a_info->startingText ? a_info->startingText : "(null)");
		SKSE::log::info("  maxChars: {}", a_info->maxChars);
		SKSE::log::info("  doneCallback: {:p}", reinterpret_cast<void*>(a_info->doneCallback));
		SKSE::log::info("  cancelCallback: {:p}", reinterpret_cast<void*>(a_info->cancelCallback));

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
			SKSE::log::info("  charLimit: {} (game maxChars: {})", charLimit, a_info->maxChars);

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
				SKSE::log::info("VR keyboard shown successfully");
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

	LRESULT CALLBACK HookedWndProc(HWND a_hwnd, UINT a_msg, WPARAM a_wParam, LPARAM a_lParam)
	{
		// Suppress WM_CHAR ONLY when the console is open and VR keyboard is active.
		// The console gets double entry because scancodes produce WM_CHAR via
		// TranslateMessage AND PostCharToGame sends GFxCharEvent. Other menus
		// (SkyUI, MCM, etc.) need WM_CHAR to function, so only block for console.
		if (a_msg == WM_CHAR && g_consoleOpen) {
			if ((intptr_t)GetPropW(a_hwnd, L"OC_KB_ACTIVE") != 0) {
				SKSE::log::trace("WM_CHAR suppressed (console + VR keyboard active): '{}'", (char)a_wParam);
				return 0;
			}
		}

		switch (a_msg) {

		// --- Open Composite keyboard completion signal ---
		// (Fix 2.1: Thread-safe callback handling)
		case WM_OC_KEYBOARD: {
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

					SKSE::log::info("Keyboard done, text: \"{}\"", text);
					doneCb(userParam, text);
				} else if (a_wParam == 0 && cancelCb) {
					// Keyboard Cancelled
					SKSE::log::info("Keyboard cancelled");
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

		SKSE::log::info("Virtual keyboard device found at {:p}", reinterpret_cast<void*>(vkbd));

		// Get vtable pointer
		auto vtable = *reinterpret_cast<std::uintptr_t**>(vkbd);
		SKSE::log::info("VTable at {:p}", reinterpret_cast<void*>(vtable));

		// Slot 0x0B = Start() virtual function
		constexpr std::size_t kStartSlot = 0x0B;

		// Save original (should be a no-op, but save it anyway)
		g_originalStart = reinterpret_cast<Start_t>(vtable[kStartSlot]);
		SKSE::log::info("Original Start() at {:p}", reinterpret_cast<void*>(g_originalStart));

		// Patch vtable to point to our hook
		DWORD oldProtect = 0;
		if (VirtualProtect(&vtable[kStartSlot], sizeof(std::uintptr_t), PAGE_EXECUTE_READWRITE, &oldProtect)) {
			vtable[kStartSlot] = reinterpret_cast<std::uintptr_t>(&HookedStart);
			VirtualProtect(&vtable[kStartSlot], sizeof(std::uintptr_t), oldProtect, &oldProtect);
			SKSE::log::info("BSVirtualKeyboardDevice::Start() hooked successfully");
		} else {
			SKSE::log::error("Failed to VirtualProtect vtable for Start() hook (error: {})", GetLastError());
		}
	}

	// =========================================================================
	// Forward declarations for FP mask (defined in SetupGeometry section below)
	extern ID3D11RenderTargetView* g_fpMaskRTV;
	extern uint32_t g_diag_diCalls, g_diag_diiCalls, g_diag_diFP, g_diag_diiFP;
	extern uint32_t g_diag_setupFP, g_diag_restoreFP;
	extern int g_diag_eyeCount;
	extern int g_fpRedrawCount;
	// Forward declarations for draw hooks and counters defined later
	using DrawIndexed_fn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
	extern DrawIndexed_fn g_origDI;
	extern uint32_t g_totalRedraws;
	extern ID3D11Buffer* g_fpIBs[16];
	extern int g_fpIBCount;
	extern bool g_fpIBsCaptured;
	extern bool g_fpMaskNeedsClear;

	// ClearDepthStencilView hook — capture stencil before the game clears it
	// =========================================================================
	// Skyrim VR clears the stencil buffer during post-processing, before Submit.
	// We hook ClearDepthStencilView and copy the depth-stencil to a staging
	// texture BEFORE the first stencil clear each frame. This gives ASW access
	// to the stencil values written during the G-buffer pass.

	using ClearDSV_t = void(__stdcall*)(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
	ClearDSV_t g_originalClearDSV = nullptr;
	ID3D11Texture2D* g_stencilStagingTex = nullptr;     // R24G8_TYPELESS staging copy
	ID3D11DepthStencilView* g_mainDSV = nullptr;         // Cached: game's main DSV (for comparison)
	ID3D11Texture2D* g_mainDepthTex = nullptr;            // Cached: game's main depth texture
	bool g_stencilCapturedThisFrame = false;              // Reset each frame on first clear

	static int g_clearDSVCallCount = 0;
	static int g_clearStencilCount = 0;
	static int g_clearMainDSCount = 0;
	static int g_clearRTVCallCount = 0;

	// Diagnostic: hook ClearRenderTargetView to confirm vtable hooks work at all
	using ClearRTV_t = void(__stdcall*)(ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT[4]);
	ClearRTV_t g_originalClearRTV = nullptr;
	void __stdcall HookedClearRenderTargetView(
	    ID3D11DeviceContext* ctx, ID3D11RenderTargetView* rtv, const FLOAT color[4])
	{
		g_clearRTVCallCount++;
		if (g_clearRTVCallCount <= 3)
			SKSE::log::info("ClearRTV #{}: rtv={:p}", g_clearRTVCallCount, static_cast<void*>(rtv));
		g_originalClearRTV(ctx, rtv, color);
	}

	void __stdcall HookedClearDepthStencilView(
	    ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv,
	    UINT clearFlags, FLOAT depth, UINT8 stencil)
	{
		g_clearDSVCallCount++;

		// Log first few calls to understand clear patterns
		if (g_clearDSVCallCount <= 20) {
			ID3D11Resource* dsvResource = nullptr;
			dsv->GetResource(&dsvResource);
			bool isMainDS = (dsvResource == g_mainDepthTex);
			SKSE::log::info("ClearDSV #{}: flags=0x{:X} depth={} stencil={} resource={:p} mainDS={} mainDepthTex={:p}",
			    g_clearDSVCallCount, clearFlags, depth, (int)stencil,
			    static_cast<void*>(dsvResource), isMainDS, static_cast<void*>(g_mainDepthTex));
			if (dsvResource) dsvResource->Release();
		}

		// Capture stencil BEFORE every clear of the main depth-stencil that includes
		// D3D11_CLEAR_STENCIL. Each capture overwrites the previous one.
		if ((clearFlags & D3D11_CLEAR_STENCIL)
		    && g_stencilStagingTex && g_pBridge && g_mainDepthTex)
		{
			g_clearStencilCount++;
			ID3D11Resource* dsvResource = nullptr;
			dsv->GetResource(&dsvResource);
			bool isMainDS = (dsvResource == g_mainDepthTex);
			if (dsvResource) dsvResource->Release();

			if (isMainDS) {
				g_clearMainDSCount++;
				ctx->CopyResource(g_stencilStagingTex, g_mainDepthTex);
				g_pBridge->stencilCapturedThisFrame = 1;
				if (g_clearMainDSCount <= 5)
					SKSE::log::info("StencilCapture: Captured main DS stencil (call #{}, fpDraws={})",
						g_clearMainDSCount, g_pBridge->fpStencilDrawCount);
				// Log per-eye FP mask diagnostics (first 10 eyes)
				g_diag_eyeCount++;
				if (g_diag_eyeCount <= 10) {
					SKSE::log::info("FPMask DIAG eye#{}: DI={} DII={} DI_FP={} DII_FP={} setup={} restore={} redraws={}",
						g_diag_eyeCount, g_diag_diCalls, g_diag_diiCalls,
						g_diag_diFP, g_diag_diiFP, g_diag_setupFP, g_diag_restoreFP, g_fpRedrawCount);
				}
				// Reset per-eye FP state + diagnostics
				g_pBridge->fpStencilDrawCount = 0;
				g_diag_diCalls = 0; g_diag_diiCalls = 0;
				g_diag_diFP = 0; g_diag_diiFP = 0;
				g_diag_setupFP = 0; g_diag_restoreFP = 0;
				// Clear FP mask for the new eye
				if (g_fpMaskRTV) {
					FLOAT clearColor[4] = { 0, 0, 0, 0 };
					ctx->ClearRenderTargetView(g_fpMaskRTV, clearColor);
				}
			}
		}

		g_originalClearDSV(ctx, dsv, clearFlags, depth, stencil);
	}

	void CreateStencilStagingTexture()
	{
		if (!g_pBridge || !g_pBridge->depthTexture || !g_pBridge->d3dDevice)
			return;

		auto* device = reinterpret_cast<ID3D11Device*>(g_pBridge->d3dDevice);
		g_mainDepthTex = reinterpret_cast<ID3D11Texture2D*>(g_pBridge->depthTexture);

		D3D11_TEXTURE2D_DESC depthDesc;
		g_mainDepthTex->GetDesc(&depthDesc);

		// Create staging copy with same format + SRV access
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = depthDesc.Width;
		td.Height = depthDesc.Height;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R24G8_TYPELESS;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;

		HRESULT hr = device->CreateTexture2D(&td, nullptr, &g_stencilStagingTex);
		if (FAILED(hr)) {
			SKSE::log::error("StencilCapture: CreateTexture2D failed (hr=0x{:08X})", (unsigned)hr);
			return;
		}

		g_pBridge->stencilCaptureTexture = reinterpret_cast<uint64_t>(g_stencilStagingTex);
		SKSE::log::info("StencilCapture: Staging texture {}x{} created at {:p}",
		    td.Width, td.Height, static_cast<void*>(g_stencilStagingTex));
	}

	void InstallClearDSVHook()
	{
		if (!g_pBridge || !g_pBridge->d3dDevice)
			return;

		// Get the immediate context from the device — runtimeData.context may be
		// a different object than what the game actually renders through.
		auto* device = reinterpret_cast<ID3D11Device*>(g_pBridge->d3dDevice);
		ID3D11DeviceContext* ctx = nullptr;
		device->GetImmediateContext(&ctx);
		if (!ctx) {
			SKSE::log::error("StencilCapture: GetImmediateContext returned null");
			return;
		}

		SKSE::log::info("StencilCapture: ImmediateContext={:p} bridge.context={:p}",
		    static_cast<void*>(ctx), reinterpret_cast<void*>(g_pBridge->d3dContext));

		// ID3D11DeviceContext vtable slot 53 = ClearDepthStencilView
		auto vtable = *reinterpret_cast<uintptr_t**>(ctx);
		constexpr size_t kClearDSVSlot = 53;

		g_originalClearDSV = reinterpret_cast<ClearDSV_t>(vtable[kClearDSVSlot]);
		SKSE::log::info("StencilCapture: Original ClearDSV at {:p}",
		    reinterpret_cast<void*>(g_originalClearDSV));

		DWORD oldProtect = 0;
		if (VirtualProtect(&vtable[kClearDSVSlot], sizeof(uintptr_t), PAGE_EXECUTE_READWRITE, &oldProtect)) {
			vtable[kClearDSVSlot] = reinterpret_cast<uintptr_t>(&HookedClearDepthStencilView);
			VirtualProtect(&vtable[kClearDSVSlot], sizeof(uintptr_t), oldProtect, &oldProtect);
			SKSE::log::info("StencilCapture: ClearDepthStencilView hook installed (slot {})", kClearDSVSlot);
		} else {
			SKSE::log::error("StencilCapture: VirtualProtect failed (error: {})", GetLastError());
		}

		// Diagnostic: also hook ClearRenderTargetView (slot 50) to verify vtable hooks work
		constexpr size_t kClearRTVSlot = 50;
		g_originalClearRTV = reinterpret_cast<ClearRTV_t>(vtable[kClearRTVSlot]);
		if (VirtualProtect(&vtable[kClearRTVSlot], sizeof(uintptr_t), PAGE_EXECUTE_READWRITE, &oldProtect)) {
			vtable[kClearRTVSlot] = reinterpret_cast<uintptr_t>(&HookedClearRenderTargetView);
			VirtualProtect(&vtable[kClearRTVSlot], sizeof(uintptr_t), oldProtect, &oldProtect);
			SKSE::log::info("StencilCapture: ClearRenderTargetView hook installed (slot {}, diagnostic)", kClearRTVSlot);
		}

		ctx->Release(); // Balance GetImmediateContext AddRef
	}

	// =========================================================================
	// Per-draw-call stencil injection — mark first-person pixels with stencil=2
	// =========================================================================
	// Hook BSLightingShader::SetupGeometry (vtable slot 6) to detect when the
	// game renders first-person geometry (hands, arms, weapons). For each draw
	// call, walk the BSGeometry→NiAVObject parent chain. If any ancestor matches
	// PlayerCharacter::firstPerson3D, force-enable stencil write with ref=2.
	// This marks FP pixels in the depth-stencil buffer's G8 channel, which ASW
	// reads to apply head-tracking-only MV (no locomotion correction).

	using SetupGeometry_fn = void(__fastcall*)(void* shader, void* renderPass, uint32_t flags);
	using RestoreGeometry_fn = void(__fastcall*)(void* shader, void* renderPass, uint32_t flags);

	SetupGeometry_fn  g_origSetupGeometry = nullptr;
	RestoreGeometry_fn g_origRestoreGeometry = nullptr;

	ID3D11DeviceContext* g_setupGeomCtx = nullptr;  // Cached immediate context

	bool g_inFPDraw = false;

	// =========================================================================
	// Comprehensive render target diagnostics — runs ONCE on first FP draw
	// Tests ALL potential paths for FP pixel identification in a single run.
	// =========================================================================
	static bool g_diagDone = false;

	void DiagnoseRenderTargets()
	{
		if (g_diagDone) return;
		g_diagDone = true;

		SKSE::log::info("=== RENDER TARGET DIAGNOSTICS START ===");

		// 1. Get BSGraphics::Renderer singleton
		auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
		if (!renderer) {
			SKSE::log::error("DIAG: Renderer singleton is NULL");
			return;
		}
		SKSE::log::info("DIAG: Renderer at {:p}", (void*)renderer);

		// 2. Get depth stencil data array
		auto& dsRuntime = renderer->GetDepthStencilData();
		auto* dsArray = dsRuntime.depthStencils;

		// 3. Get color render target data
		auto& rtData = renderer->GetRuntimeData();
		auto* rtArray = rtData.renderTargets;
		auto* d3dCtx = reinterpret_cast<ID3D11DeviceContext*>(rtData.context);

		SKSE::log::info("DIAG: D3D context from renderer: {:p} (bridge: {:p})",
			(void*)d3dCtx, g_setupGeomCtx ? (void*)g_setupGeomCtx : nullptr);

		// 4. Create small STAGING texture for readback tests (4x4 R24G8)
		auto* device = reinterpret_cast<ID3D11Device*>(g_pBridge->d3dDevice);
		ID3D11Texture2D* stagingDS = nullptr;
		{
			D3D11_TEXTURE2D_DESC td = {};
			td.Width = 4; td.Height = 4;
			td.MipLevels = 1; td.ArraySize = 1;
			td.Format = DXGI_FORMAT_R24G8_TYPELESS;
			td.SampleDesc.Count = 1;
			td.Usage = D3D11_USAGE_STAGING;
			td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			device->CreateTexture2D(&td, nullptr, &stagingDS);
		}
		// Also create R32F staging for color RT readback
		ID3D11Texture2D* stagingColor = nullptr;
		{
			D3D11_TEXTURE2D_DESC td = {};
			td.Width = 4; td.Height = 4;
			td.MipLevels = 1; td.ArraySize = 1;
			td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			td.SampleDesc.Count = 1;
			td.Usage = D3D11_USAGE_STAGING;
			td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			device->CreateTexture2D(&td, nullptr, &stagingColor);
		}

		// 5. Enumerate ALL depth stencil targets
		const char* dsNames[] = {
			"kMAIN", "kMAIN_COPY", "kSHADOWMAPS_ESRAM", "kVOL_LIT_SHADOW_ESRAM",
			"kSHADOWMAPS", "kDECAL_OCCLUSION", "kCUBEMAP_REFLECTIONS",
			"kPOST_ZPREPASS_COPY", "kPOST_WATER_COPY", "kBOOK_TEXT",
			"kPRECIP_OCCL_MAP", "kFOCUS_NEO", "kTOTAL/kPROJECTEDMENU",
			"kHUDMENU", "kWORLDUI", "kMAIN_DOWNSAMPLE", "k15",
			"kFADERUI", "kSHADOWMAPS_ESRAM1", "kSHADOWMAPS_ESRAM2",
			"kSHADOWMAPS_ESRAM3"
		};

		for (int i = 0; i < 21; i++) {
			auto& ds = dsArray[i];
			if (!ds.texture) continue;

			D3D11_TEXTURE2D_DESC desc;
			ds.texture->GetDesc(&desc);

			SKSE::log::info("DIAG DS[{:2d}] {:30s}: tex={:p} {}x{} fmt={} bind=0x{:X} depthSRV={:p} stencilSRV={:p} readOnlyDSV={:p}",
				i, dsNames[i], (void*)ds.texture,
				desc.Width, desc.Height, (int)desc.Format, desc.BindFlags,
				(void*)ds.depthSRV, (void*)ds.stencilSRV, (void*)ds.readOnlyViews[0]);

			// Try CopySubresourceRegion 4x4 from center to staging, read depth+stencil
			if (stagingDS && d3dCtx) {
				D3D11_BOX srcBox = {};
				srcBox.left = desc.Width / 2;
				srcBox.top = desc.Height / 2;
				srcBox.right = srcBox.left + 4;
				srcBox.bottom = srcBox.top + 4;
				srcBox.front = 0; srcBox.back = 1;

				// Try direct CopySubresourceRegion to STAGING
				d3dCtx->CopySubresourceRegion(stagingDS, 0, 0, 0, 0, ds.texture, 0, &srcBox);

				D3D11_MAPPED_SUBRESOURCE mapped = {};
				HRESULT hr = d3dCtx->Map(stagingDS, 0, D3D11_MAP_READ, 0, &mapped);
				if (SUCCEEDED(hr) && mapped.pData) {
					auto* pixels = reinterpret_cast<uint32_t*>(mapped.pData);
					// R24G8: depth in low 24 bits, stencil in high 8 bits
					uint32_t p0 = pixels[0], p1 = pixels[1];
					uint32_t depth0 = p0 & 0x00FFFFFF;
					uint32_t stencil0 = (p0 >> 24) & 0xFF;
					uint32_t depth1 = p1 & 0x00FFFFFF;
					uint32_t stencil1 = (p1 >> 24) & 0xFF;
					float fd0 = (float)depth0 / 16777215.0f;
					float fd1 = (float)depth1 / 16777215.0f;
					d3dCtx->Unmap(stagingDS, 0);

					SKSE::log::info("  -> readback: px0=0x{:08X} depth={:.6f} stencil={} | px1=0x{:08X} depth={:.6f} stencil={}",
						p0, fd0, stencil0, p1, fd1, stencil1);
				} else {
					SKSE::log::info("  -> readback FAILED hr=0x{:08X}", (unsigned)hr);
				}
			}

			// If this target has a stencilSRV, describe it
			if (ds.stencilSRV) {
				D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
				ds.stencilSRV->GetDesc(&srvDesc);
				SKSE::log::info("  -> stencilSRV format={}", (int)srvDesc.Format);
			}
			if (ds.depthSRV) {
				D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc;
				ds.depthSRV->GetDesc(&srvDesc);
				SKSE::log::info("  -> depthSRV format={}", (int)srvDesc.Format);
			}
		}

		// 6. Check interesting color render targets
		const int colorTargets[] = {
			1,   // kMAIN
			4,   // kNORMAL_TAAMASK_SSRMASK
			7,   // kMOTION_VECTOR
			19,  // kSHADOW_MASK
			15,  // kPLAYER_FACEGEN_TINT
		};
		const char* colorNames[] = {
			"kMAIN", "kNORMAL_TAAMASK", "kMOTION_VECTOR", "kSHADOW_MASK", "kPLAYER_FACEGEN_TINT"
		};
		for (int ci = 0; ci < 5; ci++) {
			int idx = colorTargets[ci];
			auto& rt = rtArray[idx];
			if (!rt.texture) {
				SKSE::log::info("DIAG RT[{:3d}] {:25s}: null", idx, colorNames[ci]);
				continue;
			}
			D3D11_TEXTURE2D_DESC desc;
			rt.texture->GetDesc(&desc);
			SKSE::log::info("DIAG RT[{:3d}] {:25s}: tex={:p} {}x{} fmt={} bind=0x{:X} SRV={:p} UAV={:p}",
				idx, colorNames[ci], (void*)rt.texture,
				desc.Width, desc.Height, (int)desc.Format, desc.BindFlags,
				(void*)rt.SRV, (void*)rt.UAV);
		}

		// 7. Compare game's main DS texture with our bridge DS
		{
			auto& mainDS = dsArray[0]; // kMAIN
			auto* bridgeDS = reinterpret_cast<ID3D11Texture2D*>(g_pBridge->depthTexture);
			SKSE::log::info("DIAG: Game kMAIN DS tex={:p}, bridge depthTexture={:p}, SAME={}",
				(void*)mainDS.texture, (void*)bridgeDS, mainDS.texture == bridgeDS);

			// Check if game's depthSRV is same as bridge depthSRV
			auto* bridgeSRV = reinterpret_cast<ID3D11ShaderResourceView*>(g_pBridge->depthSRV);
			SKSE::log::info("DIAG: Game kMAIN depthSRV={:p}, bridge depthSRV={:p}, SAME={}",
				(void*)mainDS.depthSRV, (void*)bridgeSRV, mainDS.depthSRV == bridgeSRV);
		}

		// 8. Pass kPOST_ZPREPASS_COPY texture to OC via bridge.
		// This is world-only depth (z-prepass runs before FP rendering).
		// OC can compare it against final depth at PostSubmit to find FP pixels.
		{
			auto& zPrepass = dsArray[7]; // kPOST_ZPREPASS_COPY
			if (zPrepass.texture && g_pBridge) {
				g_pBridge->stencilCaptureTexture = reinterpret_cast<uint64_t>(zPrepass.texture);
				g_pBridge->stencilCapturedThisFrame = 1; // Signal to OC: zPrepass texture valid
				SKSE::log::info("DIAG: Passed kPOST_ZPREPASS_COPY tex={:p} to bridge (stencilCaptureTexture)",
					(void*)zPrepass.texture);
			}
		}

		if (stagingDS) stagingDS->Release();
		if (stagingColor) stagingColor->Release();

		SKSE::log::info("=== RENDER TARGET DIAGNOSTICS END ===");
	}

	// Re-draw FP geometry to mask texture: pixel-perfect FP identification
	// by re-issuing each FP draw to an R8_UNORM RTV with a constant PS.
	// Bypasses NVIDIA depth compression issues entirely.
	ID3D11Texture2D* g_fpMaskTex = nullptr;        // R8_UINT, BIND_RENDER_TARGET | BIND_SHADER_RESOURCE
	ID3D11RenderTargetView* g_fpMaskRTV = nullptr;
	ID3D11PixelShader* g_fpConstPS = nullptr;       // PS that writes 1.0 (=255 in R8_UNORM)
	ID3D11DepthStencilState* g_fpNoDepthDSS = nullptr; // Depth test/write disabled
	uint32_t g_fpMaskW = 0, g_fpMaskH = 0;
	static int g_fpSetupGeomLogCount = 0;

	// Diagnostics: per-frame draw call counters (reset in ClearDSV)
	static uint32_t g_diag_diCalls = 0;       // Total DrawIndexed calls this eye
	static uint32_t g_diag_diiCalls = 0;      // Total DrawIndexedInstanced calls this eye
	static uint32_t g_diag_diFP = 0;          // DI calls while g_inFPDraw=true
	static uint32_t g_diag_diiFP = 0;         // DII calls while g_inFPDraw=true
	static uint32_t g_diag_setupFP = 0;       // SetupGeometry FP detections this eye
	static uint32_t g_diag_restoreFP = 0;     // RestoreGeometry clears this eye
	static int g_diag_eyeCount = 0;           // Total eyes processed (for log gating)

	// Saved D3D11 draw state for FP geometry replay
	struct FPDrawState {
		ID3D11Buffer* ib = nullptr;
		DXGI_FORMAT ibFormat = DXGI_FORMAT_UNKNOWN;
		UINT ibOffset = 0;
		ID3D11Buffer* vb = nullptr;
		UINT vbStride = 0;
		UINT vbOffset = 0;
		ID3D11InputLayout* il = nullptr;
		ID3D11VertexShader* vs = nullptr;
		D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
		UINT indexCount = 0;
		// VS constant buffers (slots 0-3 typically used)
		ID3D11Buffer* vsCBs[4] = {};
	};
	static FPDrawState g_fpDrawStates[16] = {};
	static int g_fpDrawStateCount = 0;
	static bool g_fpDrawStatesCaptured = false;

	// ── FP draw replay capture (mirrors FPDrawCapture/FPReplayData in ASWProvider.h) ──
	// Must match layout exactly — OC reads these structs via bridge pointer.
	struct FPDrawCapture {
		bool valid = false;
		ID3D11Buffer* ib = nullptr; DXGI_FORMAT ibFmt = DXGI_FORMAT_UNKNOWN; UINT ibOff = 0;
		ID3D11Buffer* vb = nullptr; UINT vbStride = 0, vbOff = 0;
		ID3D11InputLayout* il = nullptr;
		D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
		UINT indexCount = 0, startIndex = 0; INT baseVertex = 0;
		UINT instanceCount = 1;
		ID3D11VertexShader* vs = nullptr;
		ID3D11PixelShader* ps = nullptr;
		ID3D11ShaderResourceView* psSRVs[8] = {};
		ID3D11SamplerState* psSamplers[4] = {};
		ID3D11BlendState* blendState = nullptr; float blendFactor[4] = {}; UINT sampleMask = 0xFFFFFFFF;
		ID3D11DepthStencilState* dss = nullptr; UINT stencilRef = 0;
		ID3D11RasterizerState* rs = nullptr;
		uint8_t cb0[80] = {};    bool hasCB0 = false;
		uint8_t cb1[48] = {};    bool hasCB1 = false;
		uint8_t cb2[400] = {};   bool hasCB2 = false;
		uint8_t cb9[3840] = {};  bool hasCB9 = false;
		uint8_t cb10[3840] = {}; bool hasCB10 = false;
		uint8_t cb13[48] = {};   bool hasCB13 = false;
		uint8_t psCB0[256] = {}; UINT psCB0Size = 0; bool hasPSCB0 = false;
		uint8_t psCB1[256] = {}; UINT psCB1Size = 0; bool hasPSCB1 = false;
		uint8_t psCB2[256] = {}; UINT psCB2Size = 0; bool hasPSCB2 = false;
	};

	struct FPReplayData {
		static constexpr int MAX_DRAWS = 16;
		static constexpr int NUM_BUFS = 2;
		struct CaptureSet {
			FPDrawCapture draws[MAX_DRAWS] = {};
			int drawCount = 0;
			uint8_t cb12[1408] = {};
			bool hasCB12 = false;
			D3D11_VIEWPORT viewport = {};
		};
		CaptureSet sets[NUM_BUFS] = {};
		std::atomic<int> readyIdx{ -1 };
		int writeIdx = 0;
	};

	static FPReplayData g_fpReplay;              // Double-buffered FP capture data
	static ID3D11Buffer* g_fpStagingCB = nullptr; // Reusable STAGING buffer for CB reads (3840 bytes)
	static bool g_fpReplayInitialized = false;
	static int g_fpCaptureDrawIdx = 0;           // Current draw index within write buffer
	static bool g_fpCB12Captured = false;         // CB[12] captured this batch

	// Helper: snapshot a VS/PS constant buffer into a byte array
	// Uses pre-allocated staging buffer (g_fpStagingCB). Returns actual size copied.
	static UINT SnapshotCB(ID3D11DeviceContext* ctx, ID3D11Buffer* cb, uint8_t* dest, UINT maxSize)
	{
		if (!cb || !g_fpStagingCB) return 0;
		D3D11_BUFFER_DESC desc = {};
		cb->GetDesc(&desc);
		UINT copySize = (desc.ByteWidth < maxSize) ? desc.ByteWidth : maxSize;
		// Staging buffer is 3840 bytes — large enough for any CB we capture
		if (desc.ByteWidth > 3840) return 0;
		ctx->CopyResource(g_fpStagingCB, cb);
		D3D11_MAPPED_SUBRESOURCE mapped = {};
		if (SUCCEEDED(ctx->Map(g_fpStagingCB, 0, D3D11_MAP_READ, 0, &mapped))) {
			memcpy(dest, mapped.pData, copySize);
			ctx->Unmap(g_fpStagingCB, 0);
			return copySize;
		}
		return 0;
	}

	// Replay all saved FP draws to mask texture
	void ReplayFPDrawsToMask()
	{
		if (g_fpDrawStateCount == 0 || !g_fpMaskRTV || !g_fpConstPS || !g_fpNoDepthDSS || !g_setupGeomCtx)
			return;

		// Save current state
		ID3D11RenderTargetView* savedRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
		ID3D11DepthStencilView* savedDSV = nullptr;
		ID3D11PixelShader* savedPS = nullptr;
		ID3D11DepthStencilState* savedDSS = nullptr;
		UINT savedStencilRef = 0;
		ID3D11Buffer* savedIB = nullptr;
		DXGI_FORMAT savedIBFmt = DXGI_FORMAT_UNKNOWN;
		UINT savedIBOff = 0;
		ID3D11Buffer* savedVB = nullptr;
		UINT savedVBStride = 0, savedVBOff = 0;
		ID3D11InputLayout* savedIL = nullptr;
		ID3D11VertexShader* savedVS = nullptr;
		D3D11_PRIMITIVE_TOPOLOGY savedTopo;
		ID3D11Buffer* savedVSCBs[4] = {};

		g_setupGeomCtx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, &savedDSV);
		g_setupGeomCtx->PSGetShader(&savedPS, nullptr, nullptr);
		g_setupGeomCtx->OMGetDepthStencilState(&savedDSS, &savedStencilRef);
		g_setupGeomCtx->IAGetIndexBuffer(&savedIB, &savedIBFmt, &savedIBOff);
		g_setupGeomCtx->IAGetVertexBuffers(0, 1, &savedVB, &savedVBStride, &savedVBOff);
		g_setupGeomCtx->IAGetInputLayout(&savedIL);
		g_setupGeomCtx->VSGetShader(&savedVS, nullptr, nullptr);
		g_setupGeomCtx->IAGetPrimitiveTopology(&savedTopo);
		g_setupGeomCtx->VSGetConstantBuffers(0, 4, savedVSCBs);

		// Bind mask state
		ID3D11RenderTargetView* maskRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
		maskRTVs[0] = g_fpMaskRTV;
		g_setupGeomCtx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, maskRTVs, nullptr);
		g_setupGeomCtx->PSSetShader(g_fpConstPS, nullptr, 0);
		g_setupGeomCtx->OMSetDepthStencilState(g_fpNoDepthDSS, 0);

		// Clear mask
		FLOAT clearColor[4] = { 0, 0, 0, 0 };
		g_setupGeomCtx->ClearRenderTargetView(g_fpMaskRTV, clearColor);

		// Replay each FP draw
		for (int i = 0; i < g_fpDrawStateCount; i++) {
			auto& s = g_fpDrawStates[i];
			if (s.indexCount == 0) continue;
			g_setupGeomCtx->IASetIndexBuffer(s.ib, s.ibFormat, s.ibOffset);
			g_setupGeomCtx->IASetVertexBuffers(0, 1, &s.vb, &s.vbStride, &s.vbOffset);
			g_setupGeomCtx->IASetInputLayout(s.il);
			g_setupGeomCtx->VSSetShader(s.vs, nullptr, 0);
			g_setupGeomCtx->IASetPrimitiveTopology(s.topology);
			g_setupGeomCtx->VSSetConstantBuffers(0, 4, s.vsCBs);
			g_origDI(g_setupGeomCtx, s.indexCount, 0, 0);
			g_totalRedraws++;
		}

		// Restore all state
		g_setupGeomCtx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, savedDSV);
		g_setupGeomCtx->PSSetShader(savedPS, nullptr, 0);
		g_setupGeomCtx->OMSetDepthStencilState(savedDSS, savedStencilRef);
		g_setupGeomCtx->IASetIndexBuffer(savedIB, savedIBFmt, savedIBOff);
		g_setupGeomCtx->IASetVertexBuffers(0, 1, &savedVB, &savedVBStride, &savedVBOff);
		g_setupGeomCtx->IASetInputLayout(savedIL);
		g_setupGeomCtx->VSSetShader(savedVS, nullptr, 0);
		g_setupGeomCtx->IASetPrimitiveTopology(savedTopo);
		g_setupGeomCtx->VSSetConstantBuffers(0, 4, savedVSCBs);
		for (auto& rtv : savedRTVs) if (rtv) rtv->Release();
		if (savedDSV) savedDSV->Release();
		if (savedPS) savedPS->Release();
		if (savedDSS) savedDSS->Release();
		if (savedIB) savedIB->Release();
		if (savedVB) savedVB->Release();
		if (savedIL) savedIL->Release();
		if (savedVS) savedVS->Release();
		for (auto& cb : savedVSCBs) if (cb) cb->Release();
	}

	// Constant PS source: outputs 255 to mark FP pixels in R8_UINT mask
	static const char g_fpConstPSSrc[] = R"(
uint main() : SV_Target { return 255; }
)";

	// Draw call hooks — re-draw FP geometry to mask RTV
	using DrawIndexedInstanced_fn = void(STDMETHODCALLTYPE*)(
		ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
	DrawIndexed_fn g_origDI = nullptr;
	DrawIndexedInstanced_fn g_origDII = nullptr;
	static bool g_drawHooksInstalled = false;
	static int g_fpRedrawCount = 0;  // Diagnostic counter

	// Shared state save/restore + re-draw helper.
	// redrawFn is called between bind and restore to re-issue the draw.
	template<typename RedrawFn>
	void FPMaskRedraw(ID3D11DeviceContext* ctx, RedrawFn redrawFn)
	{
		if (!g_inFPDraw || !g_fpMaskRTV || !g_fpConstPS || !g_fpNoDepthDSS) return;

		// Save current state
		ID3D11RenderTargetView* savedRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
		ID3D11DepthStencilView* savedDSV = nullptr;
		ID3D11PixelShader* savedPS = nullptr;
		ID3D11ClassInstance* savedPSClassInst[256] = {};
		UINT savedPSNumClassInst = 256;
		ID3D11DepthStencilState* savedDSS = nullptr;
		UINT savedStencilRef = 0;

		ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, &savedDSV);
		ctx->PSGetShader(&savedPS, savedPSClassInst, &savedPSNumClassInst);
		ctx->OMGetDepthStencilState(&savedDSS, &savedStencilRef);

		// Bind mask state: our RTV, no depth, constant PS
		ID3D11RenderTargetView* maskRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
		maskRTVs[0] = g_fpMaskRTV;
		ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, maskRTVs, nullptr);
		ctx->PSSetShader(g_fpConstPS, nullptr, 0);
		ctx->OMSetDepthStencilState(g_fpNoDepthDSS, 0);

		// Re-issue the draw
		redrawFn();

		// Restore original state
		ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, savedDSV);
		ctx->PSSetShader(savedPS, savedPSNumClassInst > 0 ? savedPSClassInst : nullptr, savedPSNumClassInst);
		ctx->OMSetDepthStencilState(savedDSS, savedStencilRef);

		for (auto& rtv : savedRTVs) if (rtv) rtv->Release();
		if (savedDSV) savedDSV->Release();
		if (savedPS) savedPS->Release();
		for (UINT i = 0; i < savedPSNumClassInst; i++) if (savedPSClassInst[i]) savedPSClassInst[i]->Release();
		if (savedDSS) savedDSS->Release();

		g_fpRedrawCount++;
	}

	static bool g_diLoggedFirstDI = false;
	static bool g_diLoggedFirstDII = false;

	static bool g_fpMaskNeedsClear = true;  // Clear mask before first FP re-draw each frame

	void STDMETHODCALLTYPE Hook_DrawIndexed_SKSE(
		ID3D11DeviceContext* ctx, UINT indexCount, UINT startIndex, INT baseVertex)
	{
		// Call original draw first — never modify the game's draw
		g_origDI(ctx, indexCount, startIndex, baseVertex);

		// FP classification: check if current IB matches a known FP IB (by size)
		bool isFPDraw = g_inFPDraw; // frame 1: SetupGeometry flag
		static bool g_ibSizeMatchLogged = false;
		if (!isFPDraw && g_fpIBsCaptured && g_fpIBCount > 0) {
			ID3D11Buffer* curIB = nullptr;
			DXGI_FORMAT fmt; UINT off;
			ctx->IAGetIndexBuffer(&curIB, &fmt, &off);
			if (curIB) {
				D3D11_BUFFER_DESC curDesc;
				curIB->GetDesc(&curDesc);
				// Match by IB SIZE instead of pointer (IBs may be recreated each frame)
				for (int i = 0; i < g_fpIBCount; i++) {
					D3D11_BUFFER_DESC fpDesc;
					g_fpIBs[i]->GetDesc(&fpDesc);
					if (curDesc.ByteWidth == fpDesc.ByteWidth) { isFPDraw = true; break; }
				}
				if (isFPDraw && !g_ibSizeMatchLogged) {
					g_ibSizeMatchLogged = true;
					SKSE::log::info("FPMask: IB SIZE match! curIB={:p} size={} (ptr differs from captured)",
						(void*)curIB, curDesc.ByteWidth);
				}
				curIB->Release();
			}
		}

		if (!isFPDraw || !g_fpMaskRTV || !g_fpConstPS || !g_fpNoDepthDSS) return;

		// Clear mask before first FP re-draw each eye.
		// OC sets preFPDepthCaptured=0 after caching → signals new eye.
		// Also clear on g_fpMaskNeedsClear (set by SetupGeometry batch transition).
		if (g_fpMaskNeedsClear || (g_pBridge && g_pBridge->preFPDepthCaptured == 0)) {
			FLOAT clearColor[4] = { 0, 0, 0, 0 };
			ctx->ClearRenderTargetView(g_fpMaskRTV, clearColor);
			g_fpMaskNeedsClear = false;
			if (g_pBridge) g_pBridge->preFPDepthCaptured = 1;
		}

		// Save state
		ID3D11RenderTargetView* savedRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
		ID3D11DepthStencilView* savedDSV = nullptr;
		ID3D11PixelShader* savedPS = nullptr;
		ID3D11DepthStencilState* savedDSS = nullptr;
		UINT savedStencilRef = 0;
		ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, &savedDSV);
		ctx->PSGetShader(&savedPS, nullptr, nullptr);
		ctx->OMGetDepthStencilState(&savedDSS, &savedStencilRef);

		// Bind mask state
		ID3D11RenderTargetView* maskRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
		maskRTVs[0] = g_fpMaskRTV;
		ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, maskRTVs, nullptr);
		ctx->PSSetShader(g_fpConstPS, nullptr, 0);
		ctx->OMSetDepthStencilState(g_fpNoDepthDSS, 0);

		// Re-draw
		g_origDI(ctx, indexCount, startIndex, baseVertex);

		// Restore
		ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, savedDSV);
		ctx->PSSetShader(savedPS, nullptr, 0);
		ctx->OMSetDepthStencilState(savedDSS, savedStencilRef);
		for (auto& rtv : savedRTVs) if (rtv) rtv->Release();
		if (savedDSV) savedDSV->Release();
		if (savedPS) savedPS->Release();
		if (savedDSS) savedDSS->Release();

		g_totalRedraws++;
		static int logCount = 0;
		if (logCount++ < 30) {
			SKSE::log::info("FPMask: MH re-draw #{} idx={} IB-match={}", logCount, indexCount, !g_inFPDraw);
		}
	}

	static bool g_diLoggedFirstDII2 = false;

	void STDMETHODCALLTYPE Hook_DrawIndexedInstanced_SKSE(
		ID3D11DeviceContext* ctx, UINT indexCountPerInstance, UINT instanceCount,
		UINT startIndex, INT baseVertex, UINT startInstance)
	{
		// Call original first
		g_origDII(ctx, indexCountPerInstance, instanceCount,
			startIndex, baseVertex, startInstance);

		// FP classification: same IB size matching as DrawIndexed hook
		bool isFPDraw = g_inFPDraw;
		if (!isFPDraw && g_fpIBsCaptured && g_fpIBCount > 0) {
			ID3D11Buffer* curIB = nullptr;
			DXGI_FORMAT fmt; UINT off;
			ctx->IAGetIndexBuffer(&curIB, &fmt, &off);
			if (curIB) {
				D3D11_BUFFER_DESC curDesc;
				curIB->GetDesc(&curDesc);
				for (int i = 0; i < g_fpIBCount; i++) {
					D3D11_BUFFER_DESC fpDesc;
					g_fpIBs[i]->GetDesc(&fpDesc);
					if (curDesc.ByteWidth == fpDesc.ByteWidth) { isFPDraw = true; break; }
				}
				curIB->Release();
			}
		}

		if (!isFPDraw || !g_fpMaskRTV || !g_fpConstPS || !g_fpNoDepthDSS) return;

		// Clear mask before first FP re-draw each eye
		if (g_fpMaskNeedsClear || (g_pBridge && g_pBridge->preFPDepthCaptured == 0)) {
			FLOAT clearColor[4] = { 0, 0, 0, 0 };
			ctx->ClearRenderTargetView(g_fpMaskRTV, clearColor);
			g_fpMaskNeedsClear = false;
			if (g_pBridge) g_pBridge->preFPDepthCaptured = 1;
		}

		// Save state
		ID3D11RenderTargetView* savedRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
		ID3D11DepthStencilView* savedDSV = nullptr;
		ID3D11PixelShader* savedPS = nullptr;
		ID3D11DepthStencilState* savedDSS = nullptr;
		UINT savedStencilRef = 0;
		ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, &savedDSV);
		ctx->PSGetShader(&savedPS, nullptr, nullptr);
		ctx->OMGetDepthStencilState(&savedDSS, &savedStencilRef);

		// Bind mask state
		ID3D11RenderTargetView* maskRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
		maskRTVs[0] = g_fpMaskRTV;
		ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, maskRTVs, nullptr);
		ctx->PSSetShader(g_fpConstPS, nullptr, 0);
		ctx->OMSetDepthStencilState(g_fpNoDepthDSS, 0);

		// Re-draw (same instanced call)
		g_origDII(ctx, indexCountPerInstance, instanceCount,
			startIndex, baseVertex, startInstance);

		// Restore
		ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, savedDSV);
		ctx->PSSetShader(savedPS, nullptr, 0);
		ctx->OMSetDepthStencilState(savedDSS, savedStencilRef);
		for (auto& rtv : savedRTVs) if (rtv) rtv->Release();
		if (savedDSV) savedDSV->Release();
		if (savedPS) savedPS->Release();
		if (savedDSS) savedDSS->Release();

		g_totalRedraws++;
		static int diiLogCount = 0;
		if (diiLogCount++ < 30) {
			SKSE::log::info("FPMask: MH-DII re-draw #{} idx={} inst={} IB-match={}",
				diiLogCount, indexCountPerInstance, instanceCount, !g_inFPDraw);
		}
	}

	static bool g_fpHadFPDraws = false;  // true after FP draws, reset on non-FP transition
	static uint32_t g_totalSetupCalls = 0;
	static uint32_t g_totalFPSetups = 0;
	static uint32_t g_totalRedraws = 0;
	static uint32_t g_totalRestoreFP = 0;
	static uintptr_t g_knownFPGeoms[16] = {};
	static int g_knownFPGeomCount = 0;
	static bool g_fpGeomReappearLogged = false;

	// Known FP index buffer pointers — captured on frame 1, matched in MinHook on frame 2+
	static ID3D11Buffer* g_fpIBs[16] = {};
	static int g_fpIBCount = 0;
	static bool g_fpIBsCaptured = false;  // true after first frame FP detection complete

	void __fastcall Hook_SetupGeometry(void* shader, void* renderPass, uint32_t flags)
	{
		if (!g_diagDone) DiagnoseRenderTargets();
		g_totalSetupCalls++;

		// Try reading BSShaderAccumulator::firstPerson via GetCurrentAccumulator
		// GetCurrentAccumulator at RELOCATION_ID(98997, 105651), firstPerson at +0x128
		static bool g_loggedAccumFP = false;
		static int g_accumFPTrueCount = 0;
		static bool g_accumResolveFailed = false;
		if (!g_accumResolveFailed) {
			using GetAccum_t = void*(*)();
			static REL::Relocation<GetAccum_t> getAccum{ RELOCATION_ID(98997, 105651) };
			auto* accum = getAccum();
			if (accum) {
				bool fp = *reinterpret_cast<bool*>(reinterpret_cast<uintptr_t>(accum) + 0x128);
				if (fp) g_accumFPTrueCount++;
				if (!g_loggedAccumFP && g_totalSetupCalls > 100) {
					g_loggedAccumFP = true;
					SKSE::log::info("FPMask: AccumDiag accum={:p} firstPerson(0x128)={} fpTrueCount={} after {} calls",
						accum, fp, g_accumFPTrueCount, g_totalSetupCalls);
				}
			}
		}

		// Periodic summary every 5000 SetupGeometry calls (~few seconds)
		if ((g_totalSetupCalls % 5000) == 0) {
			SKSE::log::info("FPMask SUMMARY: setupTotal={} fpDetect={} restoreFP={} redraws={} fpIBs={} fpRoot={:p}",
				g_totalSetupCalls, g_totalFPSetups, g_totalRestoreFP, g_totalRedraws,
				g_fpIBCount,
				g_pBridge ? (void*)(uintptr_t)g_pBridge->playerFirstPersonRootPtr : nullptr);
		}
		// (RE-WALK diagnostic removed — confirmed chains are valid)

		// Parent chain walk to identify FP geometry
		bool isFP = false;
		uintptr_t geometry = 0;
		if (g_pBridge && g_pBridge->status == 1) {
			uintptr_t fpRoot = static_cast<uintptr_t>(g_pBridge->playerFirstPersonRootPtr);
			if (fpRoot) {
				uintptr_t passAddr = reinterpret_cast<uintptr_t>(renderPass);
				geometry = *reinterpret_cast<uintptr_t*>(passAddr + 0x10);
				if (geometry) {
					// Fast check: is this a known FP geometry pointer?
					for (int i = 0; i < g_knownFPGeomCount; i++) {
						if (geometry == g_knownFPGeoms[i]) { isFP = true; break; }
					}
					// Slow path: parent chain walk for unknown geometries
					if (!isFP) {
						uintptr_t node = geometry;
						for (int depth = 0; depth < 20 && node; depth++) {
							if (node == fpRoot) { isFP = true; break; }
							node = *reinterpret_cast<uintptr_t*>(node + 0x30);
						}
					}
				}
			}
		}

		// Non-FP draw after FP draws → FP batch ended → reset flag so next FP batch clears
		if (!isFP && g_fpHadFPDraws) {
			g_fpHadFPDraws = false;
			g_fpMaskNeedsClear = true; // Next FP draw (MinHook) will clear mask

			// Publish FP replay captures — batch is complete
			if (g_fpReplayInitialized) {
				auto& ws = g_fpReplay.sets[g_fpReplay.writeIdx];
				if (ws.drawCount > 0) {
					int published = g_fpReplay.writeIdx;
					g_fpReplay.readyIdx.store(published, std::memory_order_release);
					g_fpReplay.writeIdx = (g_fpReplay.writeIdx + 1) % FPReplayData::NUM_BUFS;
					static int s_pubLog = 0;
					if (s_pubLog++ < 10) {
						SKSE::log::info("FPReplay: published buf={} draws={} cb12={}",
							published, ws.drawCount, ws.hasCB12);
					}
				}
			}
		}

		// IB capture stays open — new FP IBs added as they're found.
		// g_fpIBsCaptured enables MinHook matching once we have at least 1 IB.
		if (!g_fpIBsCaptured && g_fpIBCount > 0) {
			g_fpIBsCaptured = true;
			SKSE::log::info("FPMask: IB matching enabled ({} IBs so far)", g_fpIBCount);
		}

		if (isFP) {
			g_totalFPSetups++;
			// Save known FP geometry pointers for reappear check
			if (g_knownFPGeomCount < 16) {
				bool alreadyKnown = false;
				for (int i = 0; i < g_knownFPGeomCount; i++) {
					if (g_knownFPGeoms[i] == geometry) { alreadyKnown = true; break; }
				}
				if (!alreadyKnown) g_knownFPGeoms[g_knownFPGeomCount++] = geometry;
			}
			// Clear mask before the FIRST FP draw of each batch
			if (!g_fpHadFPDraws && g_fpMaskRTV && g_setupGeomCtx) {
				FLOAT clearColor[4] = { 0, 0, 0, 0 };
				g_setupGeomCtx->ClearRenderTargetView(g_fpMaskRTV, clearColor);
				g_fpHadFPDraws = true;

				// Reset FP replay write buffer for new batch
				if (g_fpReplayInitialized) {
					auto& ws = g_fpReplay.sets[g_fpReplay.writeIdx];
					ws.drawCount = 0;
					ws.hasCB12 = false;
					// Invalidate all draws
					for (int i = 0; i < FPReplayData::MAX_DRAWS; i++)
						ws.draws[i].valid = false;
				}
			}
			g_inFPDraw = true;
			g_diag_setupFP++;
			if (g_pBridge) {
				g_pBridge->fpStencilInjectNow = 1;
				g_pBridge->fpStencilDrawCount++;
				g_pBridge->fpStencilDrawCountTotal++;
			}
		}

		g_origSetupGeometry(shader, renderPass, flags);

		// Log BSRenderPass fields for FP and non-FP draws to find classifiers
		if (g_fpSetupGeomLogCount < 30) {
			uintptr_t passAddr2 = reinterpret_cast<uintptr_t>(renderPass);
			uint32_t passEnum = *reinterpret_cast<uint32_t*>(passAddr2 + 0x18);
			uint8_t accumHint = *reinterpret_cast<uint8_t*>(passAddr2 + 0x1C);
			uintptr_t shaderPtr = *reinterpret_cast<uintptr_t*>(passAddr2 + 0x00);
			uintptr_t shaderPropPtr = *reinterpret_cast<uintptr_t*>(passAddr2 + 0x08);
			// BSShader::shaderType at offset 0x20 (RE::BSShader members start after NiObject + NiBoneMatrixSetterI + BSReloadShaderI)
			uint32_t shaderType = shaderPtr ? *reinterpret_cast<uint32_t*>(shaderPtr + 0x20) : 0;
			// BSShaderProperty::flags at offset 0x30 (after NiShadeProperty+NiBSShaderLightingProperty)
			uint64_t propFlags = shaderPropPtr ? *reinterpret_cast<uint64_t*>(shaderPropPtr + 0x30) : 0;

			if (isFP) {
				g_fpSetupGeomLogCount++;
				SKSE::log::info("FPMask: SetupGeom FP #{} geom={:p} passEnum=0x{:X} accumHint={} shType={} propFlags=0x{:016X}",
					g_fpSetupGeomLogCount, (void*)geometry, passEnum, accumHint, shaderType, propFlags);
			} else if (g_fpSetupGeomLogCount >= 13 && g_fpSetupGeomLogCount < 20) {
				// Log a few non-FP draws right after the FP batch for comparison
				g_fpSetupGeomLogCount++;
				SKSE::log::info("FPMask: SetupGeom WORLD #{} geom={:p} passEnum=0x{:X} accumHint={} shType={} propFlags=0x{:016X}",
					g_fpSetupGeomLogCount, (void*)geometry, passEnum, accumHint, shaderType, propFlags);
			}
		}
		// Also: on frame 2+, check if any draw matches frame 1's FP passEnum values
		static uint32_t g_fpPassEnums[16] = {};
		static int g_fpPassEnumCount = 0;
		static bool g_fpPassEnumMatchLogged = false;
		if (isFP && g_fpPassEnumCount < 16) {
			bool found = false;
			for (int i = 0; i < g_fpPassEnumCount; i++) {
				if (g_fpPassEnums[i] == *reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(renderPass) + 0x18)) { found = true; break; }
			}
			if (!found) g_fpPassEnums[g_fpPassEnumCount++] = *reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(renderPass) + 0x18);
		}
		if (!isFP && !g_fpPassEnumMatchLogged && g_fpPassEnumCount > 0 && g_totalSetupCalls > 5000) {
			uint32_t pe = *reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(renderPass) + 0x18);
			for (int i = 0; i < g_fpPassEnumCount; i++) {
				if (pe == g_fpPassEnums[i]) {
					g_fpPassEnumMatchLogged = true;
					SKSE::log::info("FPMask: WORLD draw has FP passEnum=0x{:X} at setupTotal={} — NOT a unique FP classifier!",
						pe, g_totalSetupCalls);
					break;
				}
			}
		}
	}

	void __fastcall Hook_RestoreGeometry(void* shader, void* renderPass, uint32_t flags)
	{
		if (g_inFPDraw) {
			g_diag_restoreFP++;
			g_totalRestoreFP++;

			// Capture FP IB pointers on first frame for MinHook matching
			if (g_fpIBCount < 16 && g_setupGeomCtx) {
				ID3D11Buffer* ib = nullptr;
				DXGI_FORMAT fmt; UINT off;
				g_setupGeomCtx->IAGetIndexBuffer(&ib, &fmt, &off);
				if (ib) {
					bool known = false;
					for (int i = 0; i < g_fpIBCount; i++) {
						if (g_fpIBs[i] == ib) { known = true; break; }
					}
					if (!known) {
						g_fpIBs[g_fpIBCount++] = ib; // intentionally keep AddRef — pointers stay valid
						D3D11_BUFFER_DESC d; ib->GetDesc(&d);
						SKSE::log::info("FPMask: Captured FP IB[{}]={:p} size={}", g_fpIBCount-1, (void*)ib, d.ByteWidth);
					} else {
						ib->Release(); // already known, release the AddRef from IAGetIndexBuffer
					}
				}
			}

			// Phase 2 diagnostic: dump VS CB contents to find VP matrix location.
			// Compare against known RSS VP matrix at rssBase + 0x3E0 + eye*0x250 + 0x130.
			static int g_vsCBDumpCount = 0;
			if (g_vsCBDumpCount < 1 && g_setupGeomCtx && g_pBridge && g_pBridge->rssBasePtr) {
				g_vsCBDumpCount++;
				// Read RSS VP matrix (row-major, 16 floats)
				float rssVP[16] = {};
				uintptr_t rssBase = static_cast<uintptr_t>(g_pBridge->rssBasePtr);
				// Eye 0 VP: rssBase + 0x3E0 + 0*0x250 + 0x130 = rssBase + 0x510
				memcpy(rssVP, reinterpret_cast<void*>(rssBase + 0x510), sizeof(rssVP));
				SKSE::log::info("VP-DIAG: RSS VP eye0 row0: {:.4f} {:.4f} {:.4f} {:.4f}",
					rssVP[0], rssVP[1], rssVP[2], rssVP[3]);
				SKSE::log::info("VP-DIAG: RSS VP eye0 row1: {:.4f} {:.4f} {:.4f} {:.4f}",
					rssVP[4], rssVP[5], rssVP[6], rssVP[7]);
				SKSE::log::info("VP-DIAG: RSS VP eye0 row2: {:.4f} {:.4f} {:.4f} {:.4f}",
					rssVP[8], rssVP[9], rssVP[10], rssVP[11]);
				SKSE::log::info("VP-DIAG: RSS VP eye0 row3: {:.4f} {:.4f} {:.4f} {:.4f}",
					rssVP[12], rssVP[13], rssVP[14], rssVP[15]);

				// Dump all VS constant buffers (slots 0-14)
				ID3D11Buffer* vsCBs[15] = {};
				g_setupGeomCtx->VSGetConstantBuffers(0, 15, vsCBs);
				auto* device = reinterpret_cast<ID3D11Device*>(g_pBridge->d3dDevice);

				for (int slot = 0; slot < 15; slot++) {
					if (!vsCBs[slot]) continue;
					D3D11_BUFFER_DESC cbDesc = {};
					vsCBs[slot]->GetDesc(&cbDesc);

					// Create staging buffer to read CB contents
					D3D11_BUFFER_DESC stagingDesc = cbDesc;
					stagingDesc.Usage = D3D11_USAGE_STAGING;
					stagingDesc.BindFlags = 0;
					stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
					ID3D11Buffer* staging = nullptr;
					if (SUCCEEDED(device->CreateBuffer(&stagingDesc, nullptr, &staging))) {
						g_setupGeomCtx->CopyResource(staging, vsCBs[slot]);
						D3D11_MAPPED_SUBRESOURCE mapped = {};
						if (SUCCEEDED(g_setupGeomCtx->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
							float* data = reinterpret_cast<float*>(mapped.pData);
							int numFloats = cbDesc.ByteWidth / 4;
							SKSE::log::info("VP-DIAG: VS CB[{}] size={} ({} floats)", slot, cbDesc.ByteWidth, numFloats);

							// Search for RSS VP matrix within the CB
							for (int offset = 0; offset <= numFloats - 16; offset++) {
								bool match = true;
								for (int k = 0; k < 16; k++) {
									if (fabsf(data[offset + k] - rssVP[k]) > 0.001f) {
										match = false;
										break;
									}
								}
								if (match) {
									SKSE::log::info("VP-DIAG: *** MATCH *** VS CB[{}] offset={} (byte offset={})",
										slot, offset, offset * 4);
								}
							}

							// Also dump first 64 floats of each CB for manual inspection
							int dumpCount = (numFloats < 64) ? numFloats : 64;
							for (int row = 0; row < dumpCount; row += 4) {
								int remaining = dumpCount - row;
								if (remaining >= 4) {
									SKSE::log::info("  [{}] {:.4f} {:.4f} {:.4f} {:.4f}",
										row, data[row], data[row+1], data[row+2], data[row+3]);
								}
							}
							g_setupGeomCtx->Unmap(staging, 0);
						}
						staging->Release();
					}
					vsCBs[slot]->Release();
				}
			}

			// Re-draw FP geometry to mask texture.
			// D3D11 state (VBs, IBs, VS, input layout) is still bound from the game's draw.
			// We swap PS/RTV/DSS, re-issue DrawIndexed, then restore.
			if (g_fpMaskRTV && g_fpConstPS && g_fpNoDepthDSS && g_setupGeomCtx) {
				// Get index count from the currently bound IB
				ID3D11Buffer* boundIB = nullptr;
				DXGI_FORMAT ibFormat = DXGI_FORMAT_UNKNOWN;
				UINT ibOffset = 0;
				g_setupGeomCtx->IAGetIndexBuffer(&boundIB, &ibFormat, &ibOffset);
				UINT indexCount = 0;
				if (boundIB) {
					D3D11_BUFFER_DESC ibDesc = {};
					boundIB->GetDesc(&ibDesc);
					boundIB->Release();
					UINT bytesPerIndex = (ibFormat == DXGI_FORMAT_R32_UINT) ? 4 : 2;
					indexCount = ibDesc.ByteWidth / bytesPerIndex;
				}

				if (indexCount > 0) {

					// Save OM + PS state
					ID3D11RenderTargetView* savedRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
					ID3D11DepthStencilView* savedDSV = nullptr;
					ID3D11PixelShader* savedPS = nullptr;
					ID3D11ClassInstance* savedPSCI[256] = {};
					UINT savedPSCICount = 256;
					ID3D11DepthStencilState* savedDSS = nullptr;
					UINT savedStencilRef = 0;

					g_setupGeomCtx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, &savedDSV);
					g_setupGeomCtx->PSGetShader(&savedPS, savedPSCI, &savedPSCICount);
					g_setupGeomCtx->OMGetDepthStencilState(&savedDSS, &savedStencilRef);

					// Bind mask state
					ID3D11RenderTargetView* maskRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
					maskRTVs[0] = g_fpMaskRTV;
					g_setupGeomCtx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, maskRTVs, nullptr);
					g_setupGeomCtx->PSSetShader(g_fpConstPS, nullptr, 0);
					g_setupGeomCtx->OMSetDepthStencilState(g_fpNoDepthDSS, 0);

					// Re-draw using original D3D11 function (bypasses vtable)
					g_origDI(g_setupGeomCtx, indexCount, 0, 0);

					// Restore
					g_setupGeomCtx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, savedDSV);
					g_setupGeomCtx->PSSetShader(savedPS, savedPSCICount > 0 ? savedPSCI : nullptr, savedPSCICount);
					g_setupGeomCtx->OMSetDepthStencilState(savedDSS, savedStencilRef);
					for (auto& rtv : savedRTVs) if (rtv) rtv->Release();
					if (savedDSV) savedDSV->Release();
					if (savedPS) savedPS->Release();
					for (UINT i = 0; i < savedPSCICount; i++) if (savedPSCI[i]) savedPSCI[i]->Release();
					if (savedDSS) savedDSS->Release();

					g_fpRedrawCount++;
					g_totalRedraws++;
					if (g_fpRedrawCount <= 20) {
						SKSE::log::info("FPMask: RestoreGeom re-draw #{} idx={}",
							g_fpRedrawCount, indexCount);
					}
				}
			}
		}

		// ── FP Draw Replay: capture full D3D11 state for warp-frame re-rendering ──
		if (g_fpReplayInitialized && g_setupGeomCtx) {
			auto& ws = g_fpReplay.sets[g_fpReplay.writeIdx];
			int di = ws.drawCount;
			if (di < FPReplayData::MAX_DRAWS) {
				auto& cap = ws.draws[di];
				cap = FPDrawCapture{}; // zero-init
				auto* ctx = g_setupGeomCtx;

				// IA state
				ctx->IAGetIndexBuffer(&cap.ib, &cap.ibFmt, &cap.ibOff);
				ctx->IAGetVertexBuffers(0, 1, &cap.vb, &cap.vbStride, &cap.vbOff);
				ctx->IAGetInputLayout(&cap.il);
				ctx->IAGetPrimitiveTopology(&cap.topo);

				// Index count from IB desc
				if (cap.ib) {
					D3D11_BUFFER_DESC ibDesc = {};
					cap.ib->GetDesc(&ibDesc);
					UINT bpi = (cap.ibFmt == DXGI_FORMAT_R32_UINT) ? 4 : 2;
					cap.indexCount = ibDesc.ByteWidth / bpi;
				}
				cap.instanceCount = 1; // force single-eye replay

				// VS + PS shaders
				ctx->VSGetShader(&cap.vs, nullptr, nullptr);
				ctx->PSGetShader(&cap.ps, nullptr, nullptr);

				// PS SRVs + samplers
				ctx->PSGetShaderResources(0, 8, cap.psSRVs);
				ctx->PSGetSamplers(0, 4, cap.psSamplers);

				// OM state
				ctx->OMGetBlendState(&cap.blendState, cap.blendFactor, &cap.sampleMask);
				ctx->OMGetDepthStencilState(&cap.dss, &cap.stencilRef);

				// RS state
				ctx->RSGetState(&cap.rs);

				// Viewport (first draw only)
				if (di == 0) {
					UINT numVP = 1;
					ctx->RSGetViewports(&numVP, &ws.viewport);
				}

				// VS CB snapshots
				ID3D11Buffer* vsCBs[14] = {};
				ctx->VSGetConstantBuffers(0, 14, vsCBs);

				// CB[12] — shared, capture once per batch
				if (!ws.hasCB12 && vsCBs[12]) {
					D3D11_BUFFER_DESC d = {}; vsCBs[12]->GetDesc(&d);
					UINT sz = (d.ByteWidth < 1408) ? d.ByteWidth : 1408;
					if (SnapshotCB(ctx, vsCBs[12], ws.cb12, sz) > 0)
						ws.hasCB12 = true;
				}

				// Per-draw CBs
				if (vsCBs[0]) { if (SnapshotCB(ctx, vsCBs[0], cap.cb0, 80) > 0) cap.hasCB0 = true; }
				if (vsCBs[1]) { if (SnapshotCB(ctx, vsCBs[1], cap.cb1, 48) > 0) cap.hasCB1 = true; }
				if (vsCBs[2]) { if (SnapshotCB(ctx, vsCBs[2], cap.cb2, 400) > 0) cap.hasCB2 = true; }
				if (vsCBs[9]) { if (SnapshotCB(ctx, vsCBs[9], cap.cb9, 3840) > 0) cap.hasCB9 = true; }
				if (vsCBs[10]) { if (SnapshotCB(ctx, vsCBs[10], cap.cb10, 3840) > 0) cap.hasCB10 = true; }
				if (vsCBs[13]) { if (SnapshotCB(ctx, vsCBs[13], cap.cb13, 48) > 0) cap.hasCB13 = true; }

				for (auto& cb : vsCBs) if (cb) cb->Release();

				// PS CB snapshots (slots 0-2)
				ID3D11Buffer* psCBs[3] = {};
				ctx->PSGetConstantBuffers(0, 3, psCBs);
				if (psCBs[0]) {
					D3D11_BUFFER_DESC d = {}; psCBs[0]->GetDesc(&d);
					UINT sz = (d.ByteWidth < 256) ? d.ByteWidth : 256;
					if (SnapshotCB(ctx, psCBs[0], cap.psCB0, sz) > 0) {
						cap.psCB0Size = sz; cap.hasPSCB0 = true;
					}
				}
				if (psCBs[1]) {
					D3D11_BUFFER_DESC d = {}; psCBs[1]->GetDesc(&d);
					UINT sz = (d.ByteWidth < 256) ? d.ByteWidth : 256;
					if (SnapshotCB(ctx, psCBs[1], cap.psCB1, sz) > 0) {
						cap.psCB1Size = sz; cap.hasPSCB1 = true;
					}
				}
				if (psCBs[2]) {
					D3D11_BUFFER_DESC d = {}; psCBs[2]->GetDesc(&d);
					UINT sz = (d.ByteWidth < 256) ? d.ByteWidth : 256;
					if (SnapshotCB(ctx, psCBs[2], cap.psCB2, sz) > 0) {
						cap.psCB2Size = sz; cap.hasPSCB2 = true;
					}
				}
				for (auto& cb : psCBs) if (cb) cb->Release();

				cap.valid = true;
				ws.drawCount++;

				static int s_capLog = 0;
				if (s_capLog++ < 20) {
					SKSE::log::info("FPReplay: captured draw #{} idx={} vs={:p} ps={:p} cb12={}",
						ws.drawCount, cap.indexCount, (void*)cap.vs, (void*)cap.ps, ws.hasCB12);
				}
			}
		}
		g_inFPDraw = false;
		g_origRestoreGeometry(shader, renderPass, flags);
	}

	void InstallSetupGeometryHook()
	{
		if (!g_pBridge || !g_pBridge->d3dDevice) return;

		// Get immediate context (same approach as InstallClearDSVHook)
		auto* device = reinterpret_cast<ID3D11Device*>(g_pBridge->d3dDevice);
		device->GetImmediateContext(&g_setupGeomCtx);
		if (!g_setupGeomCtx) {
			SKSE::log::error("FPStencil: GetImmediateContext returned null");
			return;
		}
		// Note: we intentionally keep the AddRef — context lives for the process lifetime

		// (Pre-FP depth texture created below with FP mask infrastructure)

		// Resolve BSLightingShader vtable (primary vtable = index [0])
		// VTABLE_BSLightingShader[0] = REL::VariantID(305261, 255053, 0x19050d0)
		auto vtableAddr = REL::VariantID(305261, 255053, 0x19050d0).address();
		if (!vtableAddr) {
			SKSE::log::error("FPStencil: BSLightingShader vtable not found");
			g_setupGeomCtx->Release();
			g_setupGeomCtx = nullptr;
			return;
		}

		auto* vtable = reinterpret_cast<uintptr_t*>(vtableAddr);

		// SetupGeometry = slot 6, RestoreGeometry = slot 7
		constexpr size_t kSetupGeomSlot = 6;
		constexpr size_t kRestoreGeomSlot = 7;

		SKSE::log::info("FPStencil: BSLightingShader vtable at {:p}", (void*)vtableAddr);
		SKSE::log::info("FPStencil: SetupGeometry[{}]={:p} RestoreGeometry[{}]={:p}",
			kSetupGeomSlot, (void*)vtable[kSetupGeomSlot],
			kRestoreGeomSlot, (void*)vtable[kRestoreGeomSlot]);

		g_origSetupGeometry = reinterpret_cast<SetupGeometry_fn>(vtable[kSetupGeomSlot]);
		g_origRestoreGeometry = reinterpret_cast<RestoreGeometry_fn>(vtable[kRestoreGeomSlot]);

		DWORD oldProtect = 0;
		// Patch both slots with one VirtualProtect call (they're adjacent)
		if (VirtualProtect(&vtable[kSetupGeomSlot], sizeof(uintptr_t) * 2, PAGE_EXECUTE_READWRITE, &oldProtect)) {
			vtable[kSetupGeomSlot] = reinterpret_cast<uintptr_t>(&Hook_SetupGeometry);
			vtable[kRestoreGeomSlot] = reinterpret_cast<uintptr_t>(&Hook_RestoreGeometry);
			VirtualProtect(&vtable[kSetupGeomSlot], sizeof(uintptr_t) * 2, oldProtect, &oldProtect);
			SKSE::log::info("FPStencil: SetupGeometry + RestoreGeometry hooks installed");
		} else {
			SKSE::log::error("FPStencil: VirtualProtect BSLightingShader failed (error: {})", GetLastError());
		}

		// Create re-draw FP mask infrastructure
		{
			auto* device = reinterpret_cast<ID3D11Device*>(g_pBridge->d3dDevice);
			D3D11_TEXTURE2D_DESC depthDesc;
			g_mainDepthTex->GetDesc(&depthDesc);
			g_fpMaskW = depthDesc.Width;
			g_fpMaskH = depthDesc.Height;

			// R8_UINT mask texture + RTV (re-draw target)
			{
				D3D11_TEXTURE2D_DESC td = {};
				td.Width = g_fpMaskW; td.Height = g_fpMaskH;
				td.MipLevels = 1; td.ArraySize = 1;
				td.Format = DXGI_FORMAT_R8_UINT;
				td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
				td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
				HRESULT hr = device->CreateTexture2D(&td, nullptr, &g_fpMaskTex);
				if (FAILED(hr)) {
					SKSE::log::error("FPMask: CreateTexture2D R8_UINT failed hr=0x{:08X}", (unsigned)hr);
				}
				if (g_fpMaskTex) {
					D3D11_RENDER_TARGET_VIEW_DESC rv = {};
					rv.Format = DXGI_FORMAT_R8_UINT;
					rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
					hr = device->CreateRenderTargetView(g_fpMaskTex, &rv, &g_fpMaskRTV);
					if (FAILED(hr)) {
						SKSE::log::error("FPMask: CreateRTV failed hr=0x{:08X}", (unsigned)hr);
					}
					g_pBridge->preFPDepthTexture = reinterpret_cast<uint64_t>(g_fpMaskTex);
				}
			}

			// Compile constant pixel shader: writes 255 to R8_UINT mask
			{
				ID3DBlob* blob = nullptr;
				ID3DBlob* err = nullptr;
				D3DCompile(g_fpConstPSSrc, sizeof(g_fpConstPSSrc) - 1,
					"FPConstPS", nullptr, nullptr, "main", "ps_5_0", 0, 0, &blob, &err);
				if (err) { SKSE::log::error("FPMask: PS compile: {}", (char*)err->GetBufferPointer()); err->Release(); }
				if (blob) {
					device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &g_fpConstPS);
					blob->Release();
				}
			}

			// Depth stencil state: depth test and write both disabled
			{
				D3D11_DEPTH_STENCIL_DESC dd = {};
				dd.DepthEnable = FALSE;
				dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
				dd.StencilEnable = FALSE;
				device->CreateDepthStencilState(&dd, &g_fpNoDepthDSS);
			}

			// Hook DrawIndexed + DrawIndexedInstanced via MinHook (function body patch)
			if (!g_drawHooksInstalled) {
				auto* ctxVtable = *reinterpret_cast<uintptr_t**>(g_setupGeomCtx);
				constexpr size_t kDISlot = 12;
				constexpr size_t kDIISlot = 20;
				void* diTarget = reinterpret_cast<void*>(ctxVtable[kDISlot]);
				void* diiTarget = reinterpret_cast<void*>(ctxVtable[kDIISlot]);
				SKSE::log::info("FPMask: DI target={:p} DII target={:p}", diTarget, diiTarget);

				MH_STATUS st = MH_Initialize();
				if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
					SKSE::log::error("FPMask: MH_Initialize failed ({})", (int)st);
				} else {
					// Hook DrawIndexed
					st = MH_CreateHook(diTarget, (void*)&Hook_DrawIndexed_SKSE, (void**)&g_origDI);
					if (st == MH_OK) st = MH_EnableHook(diTarget);
					if (st == MH_OK) {
						SKSE::log::info("FPMask: MinHook DrawIndexed OK trampoline={:p}", (void*)g_origDI);
					} else {
						SKSE::log::error("FPMask: MinHook DrawIndexed FAILED ({})", (int)st);
					}

					// Hook DrawIndexedInstanced
					st = MH_CreateHook(diiTarget, (void*)&Hook_DrawIndexedInstanced_SKSE, (void**)&g_origDII);
					if (st == MH_OK) st = MH_EnableHook(diiTarget);
					if (st == MH_OK) {
						SKSE::log::info("FPMask: MinHook DrawIndexedInstanced OK trampoline={:p}", (void*)g_origDII);
					} else {
						SKSE::log::error("FPMask: MinHook DrawIndexedInstanced FAILED ({})", (int)st);
					}

					g_drawHooksInstalled = true;
				}
			}

			if (g_pBridge) {
				g_pBridge->fpStencilInjectionActive = 1;
				g_pBridge->preFPDepthCaptured = 1; // Start in "writing" state
			}
			SKSE::log::info("FPMask: Re-draw mode: mask={}/{} PS={} DSS={} hooks={}",
				g_fpMaskTex != nullptr, g_fpMaskRTV != nullptr,
				g_fpConstPS != nullptr, g_fpNoDepthDSS != nullptr, g_drawHooksInstalled);

			// ── FP Draw Replay: pre-allocate staging buffer + set bridge pointer ──
			{
				D3D11_BUFFER_DESC sd = {};
				sd.ByteWidth = 3840; // max CB size (bone matrices)
				sd.Usage = D3D11_USAGE_STAGING;
				sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
				HRESULT hr = device->CreateBuffer(&sd, nullptr, &g_fpStagingCB);
				if (SUCCEEDED(hr)) {
					g_fpReplayInitialized = true;
					g_pBridge->fpReplayDataPtr = reinterpret_cast<uint64_t>(&g_fpReplay);
					SKSE::log::info("FPReplay: staging CB created, bridge ptr set ({:p})",
						(void*)&g_fpReplay);
				} else {
					SKSE::log::error("FPReplay: staging CB creation failed hr=0x{:08X}", (unsigned)hr);
				}
			}
		}
	}

	// =========================================================================
	// BSShaderAccumulator::FinishAccumulating hook — detect first-person render pass
	// =========================================================================
	// When the first-person accumulator finishes, set a bridge flag so the compositor
	// knows to snapshot the depth buffer (which now contains FP geometry).
	// FinishAccumulating is vtable index 0x26 (slot 38).
	void InstallShaderAccumulatorHook()
	{
		if (!g_pBridge) return;

		// Resolve BSShaderAccumulator vtable and extract FinishAccumulating address.
		// Pass it to the OC DLL via bridge — OC hooks it with MinHook (proven working).
		auto vtableAddr = REL::VariantID(304459, 254680, 0x18fd880).address();
		if (!vtableAddr) {
			SKSE::log::error("FPRender: BSShaderAccumulator vtable not found");
			return;
		}

		auto* vtable = reinterpret_cast<uintptr_t*>(vtableAddr);
		uintptr_t finishAccumAddr = vtable[0x26];
		// Slot 0x26 (FinishAccumulating) is never called in VR.
		// Try VR-specific slots: 0x2A (PreResolveDepth) and 0x2B (PostResolveDepth).
		// Pass both to the bridge — OC hooks whichever works.
		g_pBridge->finishAccumulatingAddr = static_cast<uint64_t>(vtable[0x2B]); // PostResolveDepth
		SKSE::log::info("FPRender: vtable[0x2B] (PostResolveDepth) = {:p}", reinterpret_cast<void*>(vtable[0x2B]));
		SKSE::log::info("FPRender: vtable[0x2A] (PreResolveDepth)  = {:p}", reinterpret_cast<void*>(vtable[0x2A]));
		SKSE::log::info("FPRender: vtable[0x26] (FinishAccum)      = {:p}", reinterpret_cast<void*>(vtable[0x26]));
		SKSE::log::info("FPRender: vtable[0x25] (StartAccum)       = {:p}", reinterpret_cast<void*>(vtable[0x25]));
	}

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
					SKSE::log::info("RemapMovementKeys: {} (0x{:02X} -> 0x{:02X})",
						remap.eventName, mapping.inputKey, remap.newKey);
					mapping.inputKey = remap.newKey;
					remapped++;
					break;
				}
			}
		}

		SKSE::log::info("RemapMovementKeys: Remapped {} keyboard bindings (WASD+E freed for typing)", remapped);
	}

	// =========================================================================
	// SKSE message handler
	// =========================================================================

	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kDataLoaded:
			SKSE::log::info("Game data loaded, installing hooks");
			InstallWndProcHook();
			InstallVirtualKeyboardHook();
			CreateSharedMemory();
			CreateRenderTargetBridge();
			InstallShaderAccumulatorHook();
			CreateStencilStagingTexture();
			InstallClearDSVHook();
			InstallSetupGeometryHook();
			FindAndStoreNiCamera();  // Try immediately (may need retry after save/new game)

			// Register menu state watcher for MCM laser pointer system
			if (auto ui = RE::UI::GetSingleton()) {
				ui->AddEventSink<RE::MenuOpenCloseEvent>(new MenuWatcher());
				SKSE::log::info("MenuOpenCloseEvent sink registered");
			}

			break;

		case SKSE::MessagingInterface::kPostLoadGame:
		case SKSE::MessagingInterface::kNewGame:
			FindAndStoreNiCamera();  // Retry after scene graph is fully loaded
			TestRendererShadowState();  // Diagnostic: verify game VP matrices
			break;

		case SKSE::MessagingInterface::kInputLoaded:
			SKSE::log::info("Input loaded");
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
		log->flush_on(spdlog::level::info);
		spdlog::set_default_logger(std::move(log));
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);
	SetupLogging();

	SKSE::log::info("OpenCompositeInput v3.2.0 loaded");
	SKSE::log::info("  VR keyboard bridge + Scaleform char injection + menu state tracking");
	SKSE::log::info("  + Render target bridge (MV + depth) for FSR 2/3 integration");
	SKSE::log::info("  [EXPERIMENTAL laser mouse injection DISABLED — Sovngarde bug]");

	auto messaging = SKSE::GetMessagingInterface();
	if (!messaging || !messaging->RegisterListener(OnMessage)) {
		SKSE::log::error("Failed to register messaging listener");
		return false;
	}

	SKSE::log::info("Messaging listener registered, waiting for game data load");
	return true;
}
