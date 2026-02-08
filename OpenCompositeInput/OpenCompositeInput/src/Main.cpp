// CommonLibVR headers MUST come before Windows.h (REX/W32/BASE.h enforces this)
#include <RE/B/BSInputDeviceManager.h>
#include <RE/B/BSInputEventQueue.h>
#include <RE/B/BSOpenVR.h>
#include <RE/B/BSVirtualKeyboardDevice.h>
#include <RE/B/BSWin32VirtualKeyboardDevice.h>
#include <RE/B/BSTEvent.h>
#include <RE/G/GFxEvent.h>
#include <RE/G/GFxMovieDef.h>
#include <RE/G/GFxMovieView.h>
#include <RE/G/GMatrix3D.h>
namespace RE { class GASGlobalContext; } // Forward decl needed by GFxMovieRoot.h
#include <RE/G/GFxMovieRoot.h>
#include <RE/I/IMenu.h>
#include <RE/M/MenuOpenCloseEvent.h>
#include <RE/R/Renderer.h>
#include <RE/U/UI.h>
#include <SKSE/SKSE.h>

#include <spdlog/sinks/basic_file_sink.h>
#include <set>
#include <string>

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
	using DoneCallback_t = RE::BSVirtualKeyboardDevice::kbInfo::DoneCallback;
	using CancelCallback_t = RE::BSVirtualKeyboardDevice::kbInfo::CancelCallback;

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
	static constexpr std::string_view kTrackedMenus[] = {
		"Journal Menu",
		"InventoryMenu",
		"MagicMenu",
		"MapMenu",
		"TweenMenu",
		"ContainerMenu",
		"BarterMenu",
		"FavoritesMenu",
		"CustomMenu",
		"Crafting Menu",
		"Dialogue Menu",
		"Book Menu",
		"GiftMenu",
		"Sleep/Wait Menu",
		"Lockpicking Menu",
		"Training Menu",
		"MessageBoxMenu",
		// 3D perspective menus — tracked for DETECTION ONLY so the DLL
		// and WM_OC_LASER handler know to suppress lasers/mouse injection.
		// UpdateMenuTransform skips GetMovieDef() for these.
		"StatsMenu",
		"Loading Menu",
		"Main Menu",
		"Mist Menu",
	};

	// Track which of our target menus are currently open (avoids calling
	// ui->IsMenuOpen from inside the event handler, which deadlocks because
	// Bethesda's UI holds a lock during MenuOpenCloseEvent dispatch).
	std::set<std::string> g_activeTrackedMenus;

	// Update shared memory with the active menu's 3D transform data
	void UpdateMenuTransform()
	{
		if (!g_pTransform)
			return;

		auto ui = RE::UI::GetSingleton();
		if (!ui)
			return;

		bool anyActive = !g_activeTrackedMenus.empty();

		// Begin write (odd counter = writing)
		g_pTransform->updateCounter++;

		g_pTransform->active = anyActive;

		if (!anyActive) {
			g_pTransform->menuName[0] = '\0';
			g_pTransform->hasPerspective = false;
			g_pTransform->updateCounter++; // End write (even = stable)
			return;
		}

		// Get the top-most tracked menu
		RE::IMenu* topMenu = nullptr;
		ui->GetTopMostMenu(&topMenu, 10);

		if (!topMenu || !topMenu->uiMovie) {
			g_pTransform->updateCounter++;
			return;
		}

		// Menu name priority: CustomMenu (MCM) > content menus > TweenMenu
		const char* menuNameStr;
		if (g_activeTrackedMenus.count("CustomMenu") > 0)
			menuNameStr = "CustomMenu";
		else {
			// Prefer any content menu over TweenMenu (navigation hub)
			menuNameStr = nullptr;
			for (auto& m : g_activeTrackedMenus) {
				if (m != "TweenMenu") { menuNameStr = m.c_str(); break; }
			}
			if (!menuNameStr)
				menuNameStr = g_activeTrackedMenus.begin()->c_str();
		}
		strncpy_s(g_pTransform->menuName, menuNameStr, 63);
		g_pTransform->menuName[63] = '\0';

		// Depth priority
		g_pTransform->depthPriority = topMenu->depthPriority;

		// 3D perspective menus (StatsMenu, Loading, Main, Mist) — do NOT
		// call GetMovieDef() or touch their Scaleform state in any way.
		// Accessing their movie view corrupts the 3D rendering pipeline
		// and causes the Sovngarde constellation bug.
		bool isDangerousMenu =
			strcmp(menuNameStr, "StatsMenu") == 0 ||
			strcmp(menuNameStr, "Loading Menu") == 0 ||
			strcmp(menuNameStr, "Main Menu") == 0 ||
			strcmp(menuNameStr, "Mist Menu") == 0;

		if (!isDangerousMenu) {
			// Stage dimensions — safe to query for normal 2D menus
			auto def = topMenu->uiMovie->GetMovieDef();
			if (def) {
				g_pTransform->stageWidth = def->GetWidth();
				g_pTransform->stageHeight = def->GetHeight();
			}
		}
		// else: keep previous stageWidth/stageHeight (from the safe menu underneath)

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

			// Only track menus we care about
			std::string_view name = a_event->menuName.c_str();
			bool isTracked = false;
			for (auto& m : kTrackedMenus) {
				if (name == m) {
					isTracked = true;
					break;
				}
			}

			if (!isTracked)
				return RE::BSEventNotifyControl::kContinue;

			bool wasPreviouslyActive = !g_activeTrackedMenus.empty();

			if (a_event->opening)
				g_activeTrackedMenus.insert(std::string(name));
			else
				g_activeTrackedMenus.erase(std::string(name));

			bool anyMenu = !g_activeTrackedMenus.empty();

			// Update OC_MENU_ACTIVE property on state transitions
			if (anyMenu != wasPreviouslyActive) {
				SetPropW(g_gameHwnd, L"OC_MENU_ACTIVE",
				    (HANDLE)(intptr_t)(anyMenu ? 1 : 0));
				SKSE::log::info("Menu active: {} (triggered by {} {})",
				    anyMenu, a_event->menuName.c_str(),
				    a_event->opening ? "opened" : "closed");
			}

			// Always update shared memory with current menu name
			// (e.g. TweenMenu→InventoryMenu transitions need name refresh)
			UpdateMenuTransform();

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
		g_doneCallback = a_info->doneCallback;
		g_cancelCallback = a_info->cancelCallback;
		g_userParam = a_info->userParam;
		g_waitingForKeyboard = true;

		SKSE::log::info("BSVirtualKeyboardDevice::Start() intercepted");
		SKSE::log::info("  startingText: \"{}\"", a_info->startingText ? a_info->startingText : "(null)");
		SKSE::log::info("  maxChars: {}", a_info->maxChars);
		SKSE::log::info("  doneCallback: {:p}", reinterpret_cast<void*>(a_info->doneCallback));
		SKSE::log::info("  cancelCallback: {:p}", reinterpret_cast<void*>(a_info->cancelCallback));

		// Call ShowKeyboard through the OpenVR overlay interface (Open Composite intercepts this)
		auto overlay = RE::BSOpenVR::GetCleanIVROverlay();
		if (overlay) {
			auto err = overlay->ShowKeyboard(
				vr::k_EGamepadTextInputModeNormal,
				vr::k_EGamepadTextInputLineModeSingleLine,
				0,                                                         // flags
				"Enter text",                                              // description
				a_info->maxChars > 0 ? a_info->maxChars : 256,            // max chars
				a_info->startingText ? a_info->startingText : "",          // existing text
				0);                                                        // user value

			if (err != vr::VROverlayError_None) {
				SKSE::log::error("ShowKeyboard failed with error {}", static_cast<int>(err));
				g_waitingForKeyboard = false;
			} else {
				SKSE::log::info("VR keyboard shown successfully");
			}
		} else {
			SKSE::log::error("Failed to get IVROverlay interface");
			g_waitingForKeyboard = false;
		}
	}

	// =========================================================================
	// WndProc hook — keyboard input forwarding + keyboard bridge messages
	// =========================================================================

	LRESULT CALLBACK HookedWndProc(HWND a_hwnd, UINT a_msg, WPARAM a_wParam, LPARAM a_lParam)
	{
		switch (a_msg) {

		// --- Open Composite keyboard completion signal ---
		case WM_OC_KEYBOARD: {
			if (g_waitingForKeyboard) {
				g_waitingForKeyboard = false;

				if (a_wParam == 1 && g_doneCallback) {
					// Keyboard Done — retrieve text and invoke callback
					char text[512] = {};
					auto overlay = RE::BSOpenVR::GetCleanIVROverlay();
					if (overlay) {
						overlay->GetKeyboardText(text, sizeof(text));
					}

					SKSE::log::info("Keyboard done, text: \"{}\"", text);
					g_doneCallback(g_userParam, text);
				} else if (a_wParam == 0 && g_cancelCallback) {
					// Keyboard Cancelled
					SKSE::log::info("Keyboard cancelled");
					g_cancelCallback();
				}

				g_doneCallback = nullptr;
				g_cancelCallback = nullptr;
				g_userParam = nullptr;
			}
			return 0;
		}

		// --- Open Composite menu laser pointer signal ---
		case WM_OC_LASER: {
			// NOTE: Do NOT call UpdateMenuTransform() here — it runs on menu
			// open/close events. Calling it every frame touches GetMovieDef()
			// from WndProc context which can corrupt Scaleform rendering state.

			// WPARAM: packed UV — low 16 bits = u*10000, high 16 bits = v*10000
			// LPARAM: action — 0=move, 1=press, 2=release
			float u = static_cast<float>(LOWORD(a_wParam)) / 10000.0f;
			float v = static_cast<float>(HIWORD(a_wParam)) / 10000.0f;
			int action = static_cast<int>(a_lParam);

			auto ui = RE::UI::GetSingleton();
			if (!ui)
				break;

			// Find the active menu we should inject mouse into.
			// Use the tracked menu name from shared memory (matches DLL's quad).
			RE::IMenu* targetMenu = nullptr;
			if (g_pTransform && g_pTransform->active && g_pTransform->menuName[0] != '\0') {
				RE::BSFixedString menuStr(g_pTransform->menuName);
				auto menuPtr = ui->GetMenu(menuStr);
				if (menuPtr)
					targetMenu = menuPtr.get();
			}

			// Fallback to top-most menu if shared memory name didn't resolve
			if (!targetMenu) {
				ui->GetTopMostMenu(&targetMenu, 10);
			}

			if (!targetMenu || !targetMenu->uiMovie)
				break;

			// Skip menus with 3D perspective content (Sovngarde bug prevention)
			// StatsMenu = level-up constellation scene
			if (g_pTransform && g_pTransform->menuName[0] != '\0') {
				const char* mn = g_pTransform->menuName;
				if (strcmp(mn, "StatsMenu") == 0 ||
				    strcmp(mn, "Loading Menu") == 0 ||
				    strcmp(mn, "Main Menu") == 0 ||
				    strcmp(mn, "Mist Menu") == 0)
					break;
			}

			// Use cached stage dimensions from shared memory (avoids touching GetMovieDef)
			float stageW = 1280.0f, stageH = 720.0f; // sensible defaults
			if (g_pTransform && g_pTransform->stageWidth > 0 && g_pTransform->stageHeight > 0) {
				stageW = g_pTransform->stageWidth;
				stageH = g_pTransform->stageHeight;
			}

			float x = u * stageW;
			float y = v * stageH;

			// Dispatch GFxMouseEvent::kMouseMove for hover highlighting.
			// Only move events — no clicks. The game handles trigger input
			// for menu selection through its own controller system.
			RE::GFxMouseEvent evt(RE::GFxEvent::EventType::kMouseMove,
				0, x, y, 0.f, 0);
			targetMenu->uiMovie->HandleEvent(evt);
			return 0;
		}

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

			// Register menu state watcher for MCM laser pointer system
			if (auto ui = RE::UI::GetSingleton()) {
				ui->AddEventSink<RE::MenuOpenCloseEvent>(new MenuWatcher());
				SKSE::log::info("MenuOpenCloseEvent sink registered");
			}
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

	SKSE::log::info("OpenCompositeInput v3.0.0 loaded");
	SKSE::log::info("  Scaleform input bridge + VR keyboard + MCM laser pointers");

	auto messaging = SKSE::GetMessagingInterface();
	if (!messaging || !messaging->RegisterListener(OnMessage)) {
		SKSE::log::error("Failed to register messaging listener");
		return false;
	}

	SKSE::log::info("Messaging listener registered, waiting for game data load");
	return true;
}
