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
#include <RE/R/Renderer.h>
#include <RE/U/UI.h>
#include <SKSE/SKSE.h>

#include <spdlog/sinks/basic_file_sink.h>
#include <set>
#include <string>
#include <mutex>

// Win32 API for WndProc hook (REX::W32 doesn't provide CallWindowProcW)
#include <Windows.h>

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
	uint8_t  reserved2[16];   // Future use (24 - 8 = 16 bytes)
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
				// Store viewFrustum pointer (inline frustum from RUNTIME_DATA2)
				auto& rtData2 = niCam->GetRuntimeData2();
				g_pBridge->viewFrustumPtr = reinterpret_cast<uint64_t>(&rtData2.viewFrustum);
				SKSE::log::info("RT Bridge: NiCamera found at {:p}, worldToCam at {:p}, viewFrustum at {:p}",
					reinterpret_cast<void*>(niCam),
					reinterpret_cast<void*>(g_pBridge->worldToCamPtr),
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
				if (!a_event->opening)
					FindAndStoreNiCamera();
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
				0,                                                         // flags
				"Enter text",                                              // description
				charLimit,                                                 // max chars (hard-capped at 31)
				a_info->startingText ? a_info->startingText : "",          // existing text
				0);                                                        // user value

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
