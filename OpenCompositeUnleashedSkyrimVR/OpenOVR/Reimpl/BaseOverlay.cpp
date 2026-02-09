#include "stdafx.h"
#define BASE_IMPL
#include "BaseCompositor.h"
#include "BaseOverlay.h"
// [EXPERIMENTAL — DISABLED] VR laser→menu system. Entire laser pointer subsystem
// is disabled because the SKSE-side Scaleform injection causes the Sovngarde bug
// (accessing wrong Scaleform movie permanently corrupts VR rendering).
// TODO: When re-enabling, extract all laser code into a separate file for proper
// separation of concerns — it's currently interleaved in BaseOverlay::Submit().
// #include "../Misc/Keyboard/VRMenuLaser.h"
#include "BaseSystem.h"
#include "Compositor/compositor.h"
#include "Drivers/Backend.h"
#include "Misc/Config.h"
#include "Misc/ScopeGuard.h"
#include "convert.h"
#include "generated/static_bases.gen.h"
#include <algorithm>
#include <direct.h>
#include <string>
#include <sstream>
#include <vector>

using glm::mat4;
using glm::vec3;

using namespace vr;

// Global flag: laser dot is on a menu surface and consuming trigger input.
// When true, BaseSystem::GetControllerState() masks the trigger button
// so the game doesn't double-process it (prevents the same menu from
// being opened multiple times). Only active when the laser is actually
// hitting the menu quad — trigger works normally when not pointing at it.
// Per-hand flag: keyboard laser is hitting the keyboard quad for this hand.
// When true, BaseSystem::GetControllerState() masks the trigger so the game
// doesn't see it — only the keyboard processes it. Index 0=left, 1=right.
// The keyboard itself uses GetUnmaskedControllerState() so it still sees triggers.
bool g_kbLaserConsumesTrigger[2] = { false, false };

bool g_menuLaserActive = false; // Kept defined — BaseSystem.cpp extern's it (always false when laser disabled)

// [EXPERIMENTAL — DISABLED] Custom Windows message for laser→Scaleform injection
// static constexpr UINT WM_OC_LASER = WM_APP + 0x4F44;

// ── Shared memory struct for menu transform (written by SKSE plugin) ──
#ifdef _WIN32
#pragma pack(push, 1)
struct OCMenuTransform {
	static constexpr uint32_t MAGIC = 0x54434D4F; // 'OCMT'
	static constexpr uint32_t VERSION = 1;

	uint32_t magic;
	uint32_t version;
	uint32_t updateCounter;

	bool     active;
	char     menuName[64];
	int8_t   depthPriority;

	float    stageWidth;
	float    stageHeight;

	bool     hasPerspective;
	float    perspectiveMatrix[4][4];

	uint8_t  reserved[64];
};
#pragma pack(pop)

static HANDLE           s_hMapFile = nullptr;
static OCMenuTransform* s_pTransform = nullptr;
static bool             s_sharedMemTried = false;

static void OpenSharedMemory()
{
	if (s_pTransform) // Already connected
		return;

	// Retry every ~2 seconds (assuming ~90fps, every 180 frames)
	static int retryCounter = 0;
	if (s_sharedMemTried && (++retryCounter % 180) != 0)
		return;
	s_sharedMemTried = true;

	s_hMapFile = OpenFileMappingW(FILE_MAP_READ, FALSE, L"Local\\OpenCompositeMenuTransform");
	if (!s_hMapFile)
		return; // SKSE plugin hasn't created it yet, retry later

	s_pTransform = static_cast<OCMenuTransform*>(
	    MapViewOfFile(s_hMapFile, FILE_MAP_READ, 0, 0, sizeof(OCMenuTransform)));
	if (!s_pTransform) {
		CloseHandle(s_hMapFile);
		s_hMapFile = nullptr;
		return;
	}

	OOVR_LOG("Shared memory connected to SKSE plugin");
}

// Read the shared memory with seqlock protection. Returns true if valid data.
static bool ReadMenuTransform(OCMenuTransform& out)
{
	if (!s_pTransform)
		return false;

	// Seqlock: read until we get a consistent snapshot (even counter)
	for (int attempts = 0; attempts < 3; attempts++) {
		uint32_t seq1 = s_pTransform->updateCounter;
		if (seq1 & 1) continue; // Writer is mid-update, retry

		memcpy(&out, s_pTransform, sizeof(OCMenuTransform));

		uint32_t seq2 = s_pTransform->updateCounter;
		if (seq1 == seq2 && out.magic == OCMenuTransform::MAGIC)
			return true;
	}
	return false;
}
// =========================================================================
// HARDCODED PER-MENU QUAD PROFILES
// =========================================================================
// These define the exact position, size, and offset of the laser interaction
// quad for each Skyrim VR menu. Values were hand-calibrated in-headset to
// match the Scaleform rendering quad that the game draws for each menu.
//
// The quad is positioned relative to the player's head at menu-open time:
//   distance   — how far in front of the head (meters)
//   widthScale / heightScale — quad dimensions (meters)
//   yOffset    — vertical shift (negative = lower)
//   xOffset    — horizontal shift (negative = left)
//
// [EXPERIMENTAL — DISABLED] Per-menu quad profile system.
// Each Skyrim menu (Journal, Inventory, Magic, etc.) has a different Scaleform
// layout, so the VR laser quad needs different size/position per menu. These
// hardcoded profiles were hand-calibrated in-headset to match each menu's extent.
//
// DISABLED because the laser→Scaleform injection in SKSE causes the Sovngarde
// bug. When the laser system is re-enabled, these profiles will be needed.
//
// Calibrated values (for reference / future use):
//   Journal Menu:  dist=0.86 w=1.16 h=0.75 yOff=-0.14 xOff=0.00
//   TweenMenu:     dist=0.86 w=0.45 h=0.42 yOff=-0.17 xOff=0.00
//   InventoryMenu: dist=0.86 w=0.73 h=0.89 yOff=-0.14 xOff=-0.42
//   MagicMenu:     dist=0.86 w=0.73 h=0.89 yOff=-0.14 xOff=-0.42
//   FavoritesMenu: dist=0.86 w=0.43 h=0.58 yOff=-0.23 xOff=-0.28
//   CustomMenu:    dist=0.86 w=1.17 h=0.64 yOff=-0.11 xOff=0.00
//   MapMenu:       dist=0.86 w=1.16 h=0.75 yOff=-0.14 xOff=0.00
//   ContainerMenu: dist=0.86 w=0.73 h=0.89 yOff=-0.14 xOff=-0.42
//   BarterMenu:    dist=0.86 w=0.73 h=0.89 yOff=-0.14 xOff=-0.42
//   GiftMenu:      dist=0.86 w=0.73 h=0.89 yOff=-0.14 xOff=-0.42
//   Default:       dist=0.85 w=0.80 h=0.42 yOff=-0.13 xOff=0.00
//
// struct MenuQuadProfile { const char* menuName; float distance, widthScale, heightScale, yOffset, xOffset; int opacity; };
// static constexpr MenuQuadProfile kDefaultProfile = { "(default)", 0.85f, 0.80f, 0.42f, -0.13f, 0.00f, 20 };
// static constexpr MenuQuadProfile kMenuProfiles[] = { ... };  // 10 menus
// static bool GetMenuProfile(const char* menuName, ...) { ... }
#endif // _WIN32

// Reloadable keyboard shortcut settings (updated by file watcher)
static bool s_shortcutEnabled = true;
static std::string s_shortcutButton = "left_stick";
static std::string s_shortcutMode = "double_tap";
static int s_shortcutTiming = 500;

// Initialize shortcut settings from global config
static void InitShortcutSettings()
{
	s_shortcutEnabled = oovr_global_configuration.KbShortcutEnabled();
	s_shortcutButton = oovr_global_configuration.KbShortcutButton();
	s_shortcutMode = oovr_global_configuration.KbShortcutMode();
	s_shortcutTiming = oovr_global_configuration.KbShortcutTiming();
}

// Reload shortcut settings from INI file
static bool ReloadShortcutSettings()
{
	wchar_t dllPath[MAX_PATH];
	GetModuleFileNameW(nullptr, dllPath, MAX_PATH);
	std::wstring path(dllPath);
	size_t pos = path.find_last_of(L"\\/");
	if (pos != std::wstring::npos)
		path = path.substr(0, pos + 1);
	path += L"opencomposite.ini";

	FILE* f = nullptr;
	for (int retry = 0; retry < 5 && !f; retry++) {
		f = _wfopen(path.c_str(), L"r");
		if (!f && retry < 4)
			Sleep(50);
	}
	if (!f)
		return false;

	bool inKeyboardSection = false;
	char line[512];

	while (fgets(line, sizeof(line), f)) {
		if (line[0] == '[') {
			inKeyboardSection = (strstr(line, "[keyboard]") != nullptr);
			continue;
		}
		if (!inKeyboardSection)
			continue;

		char sval[256];
		int ival;
		if (sscanf(line, "shortcutEnabled=%255s", sval) == 1)
			s_shortcutEnabled = (strcmp(sval, "true") == 0 || strcmp(sval, "1") == 0);
		if (sscanf(line, "shortcutButton=%255s", sval) == 1)
			s_shortcutButton = sval;
		if (sscanf(line, "shortcutMode=%255s", sval) == 1)
			s_shortcutMode = sval;
		if (sscanf(line, "shortcutTiming=%d", &ival) == 1)
			s_shortcutTiming = ival;
	}
	fclose(f);
	return true;
}

static bool s_shortcutSettingsInitialized = false;

// =========================================================================
// CONTROLLER COMBO SYSTEM — maps button combos to keyboard scancodes
// =========================================================================

struct ComboBinding {
	struct BtnReq {
		int ctrl;             // 0=left, 1=right
		uint64_t mask;        // digital button mask (0 if stick direction)
		int stickAxis;        // 0=X, 1=Y (only when mask==0)
		float stickThreshold; // +0.7 or -0.7 (only when mask==0)
	};

	std::vector<BtnReq> buttons;
	std::string mode;  // "press", "double_tap", "triple_tap", "quadruple_tap", "long_press"
	int timingMs;
	int scancode;

	// Per-combo detection state
	int tapCount;
	ULONGLONG tapTimes[4];
	bool wasAllPressed;
	ULONGLONG holdStart;
	bool firedThisPress;

	ComboBinding() : timingMs(500), scancode(0), tapCount(0),
		wasAllPressed(false), holdStart(0), firedThisPress(false) {
		memset(tapTimes, 0, sizeof(tapTimes));
	}
};

static std::vector<ComboBinding> s_combos;
static bool s_combosLoaded = false;

static void SendScancode(int scancode)
{
	INPUT inputs[2] = {};
	inputs[0].type = INPUT_KEYBOARD;
	inputs[0].ki.wScan = (WORD)scancode;
	inputs[0].ki.dwFlags = KEYEVENTF_SCANCODE;
	inputs[1].type = INPUT_KEYBOARD;
	inputs[1].ki.wScan = (WORD)scancode;
	inputs[1].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
	SendInput(2, inputs, sizeof(INPUT));
}

static bool ParseComboButton(const std::string& token, ComboBinding::BtnReq& out)
{
	out.mask = 0;
	out.stickAxis = 0;
	out.stickThreshold = 0;

	// Face buttons
	if (token == "a")            { out.ctrl = 1; out.mask = ButtonMaskFromId(k_EButton_A); return true; }
	if (token == "b")            { out.ctrl = 1; out.mask = ButtonMaskFromId(k_EButton_ApplicationMenu); return true; }
	if (token == "x")            { out.ctrl = 0; out.mask = ButtonMaskFromId(k_EButton_A); return true; }
	if (token == "y")            { out.ctrl = 0; out.mask = ButtonMaskFromId(k_EButton_ApplicationMenu); return true; }

	// Stick clicks
	if (token == "left_stick")   { out.ctrl = 0; out.mask = ButtonMaskFromId(k_EButton_SteamVR_Touchpad); return true; }
	if (token == "right_stick")  { out.ctrl = 1; out.mask = ButtonMaskFromId(k_EButton_SteamVR_Touchpad); return true; }

	// Grips and triggers
	if (token == "left_grip")    { out.ctrl = 0; out.mask = ButtonMaskFromId(k_EButton_Grip); return true; }
	if (token == "right_grip")   { out.ctrl = 1; out.mask = ButtonMaskFromId(k_EButton_Grip); return true; }
	if (token == "left_trigger") { out.ctrl = 0; out.mask = ButtonMaskFromId(k_EButton_Axis1); return true; }
	if (token == "right_trigger"){ out.ctrl = 1; out.mask = ButtonMaskFromId(k_EButton_Axis1); return true; }

	// Stick directions (analog threshold)
	if (token == "left_stick_up")    { out.ctrl = 0; out.stickAxis = 1; out.stickThreshold = 0.7f; return true; }
	if (token == "left_stick_down")  { out.ctrl = 0; out.stickAxis = 1; out.stickThreshold = -0.7f; return true; }
	if (token == "left_stick_left")  { out.ctrl = 0; out.stickAxis = 0; out.stickThreshold = -0.7f; return true; }
	if (token == "left_stick_right") { out.ctrl = 0; out.stickAxis = 0; out.stickThreshold = 0.7f; return true; }
	if (token == "right_stick_up")   { out.ctrl = 1; out.stickAxis = 1; out.stickThreshold = 0.7f; return true; }
	if (token == "right_stick_down") { out.ctrl = 1; out.stickAxis = 1; out.stickThreshold = -0.7f; return true; }
	if (token == "right_stick_left") { out.ctrl = 1; out.stickAxis = 0; out.stickThreshold = -0.7f; return true; }
	if (token == "right_stick_right"){ out.ctrl = 1; out.stickAxis = 0; out.stickThreshold = 0.7f; return true; }

	return false;
}

static void LoadCombos()
{
	s_combos.clear();

	wchar_t dllPath[MAX_PATH];
	GetModuleFileNameW(nullptr, dllPath, MAX_PATH);
	std::wstring path(dllPath);
	size_t pos = path.find_last_of(L"\\/");
	if (pos != std::wstring::npos)
		path = path.substr(0, pos + 1);
	path += L"opencomposite.ini";

	FILE* f = nullptr;
	for (int retry = 0; retry < 5 && !f; retry++) {
		f = _wfopen(path.c_str(), L"r");
		if (!f && retry < 4)
			Sleep(50);
	}
	if (!f)
		return;

	bool inCombosSection = false;
	char line[512];

	while (fgets(line, sizeof(line), f)) {
		if (line[0] == '[') {
			inCombosSection = (strstr(line, "[combos]") != nullptr);
			continue;
		}
		if (!inCombosSection)
			continue;
		if (s_combos.size() >= 16)
			break;

		// Parse: comboN=button_string,mode,timing_ms,0xSC
		char* eq = strchr(line, '=');
		if (!eq) continue;
		char* val = eq + 1;

		// Split value by commas
		char btnStr[256] = {}, modeStr[64] = {}, timingStr[16] = {}, scStr[16] = {};
		if (sscanf(val, "%255[^,],%63[^,],%15[^,],%15s", btnStr, modeStr, timingStr, scStr) < 4)
			continue;

		// Parse scancode (hex)
		int sc = 0;
		if (strncmp(scStr, "0x", 2) == 0 || strncmp(scStr, "0X", 2) == 0)
			sc = (int)strtol(scStr + 2, nullptr, 16);
		else
			sc = (int)strtol(scStr, nullptr, 16);
		if (sc == 0) continue;

		// Parse timing
		int timing = atoi(timingStr);

		// Parse buttons
		ComboBinding combo;
		combo.mode = modeStr;
		combo.timingMs = timing;
		combo.scancode = sc;

		std::istringstream bss(btnStr);
		std::string token;
		bool valid = true;
		while (std::getline(bss, token, '+')) {
			while (!token.empty() && token.front() == ' ') token.erase(token.begin());
			while (!token.empty() && token.back() == ' ') token.pop_back();
			if (token.empty()) continue;

			ComboBinding::BtnReq req;
			if (ParseComboButton(token, req))
				combo.buttons.push_back(req);
			else
				valid = false;
		}

		if (valid && !combo.buttons.empty())
			s_combos.push_back(std::move(combo));
	}
	fclose(f);

	// Sort: combos with more buttons checked first (prevents grip+A from eating grip+A+B)
	std::sort(s_combos.begin(), s_combos.end(),
		[](const ComboBinding& a, const ComboBinding& b) { return a.buttons.size() > b.buttons.size(); });

	OOVR_LOGF("Loaded %d controller combos", (int)s_combos.size());
}

static void ProcessCombos(BaseSystem* sys, const VRControllerState_t ctrlState[2], const bool ctrlValid[2])
{
	for (auto& combo : s_combos) {
		// Check if ALL required buttons are pressed
		bool allPressed = true;
		for (const auto& req : combo.buttons) {
			int ci = req.ctrl;
			if (!ctrlValid[ci]) { allPressed = false; break; }

			bool pressed = false;
			if (req.mask != 0) {
				// Digital button check
				pressed = (ctrlState[ci].ulButtonPressed & req.mask) != 0;
				// Analog fallback for grip/trigger
				if (!pressed) {
					if (req.mask == ButtonMaskFromId(k_EButton_Grip))
						pressed = ctrlState[ci].rAxis[2].x >= 0.5f;
					else if (req.mask == ButtonMaskFromId(k_EButton_Axis1))
						pressed = ctrlState[ci].rAxis[1].x >= 0.5f;
				}
			} else {
				// Stick direction check (analog axis)
				float axisVal = (req.stickAxis == 0)
					? ctrlState[ci].rAxis[0].x
					: ctrlState[ci].rAxis[0].y;
				pressed = (req.stickThreshold > 0)
					? (axisVal >= req.stickThreshold)
					: (axisVal <= req.stickThreshold);
			}

			if (!pressed) { allPressed = false; break; }
		}

		if (combo.mode == "press") {
			// Modifier combo: fire once when all held, reset when released
			if (allPressed && !combo.firedThisPress) {
				SendScancode(combo.scancode);
				combo.firedThisPress = true;
				OOVR_LOGF("Combo fired (press): scancode 0x%02x", combo.scancode);
			}
			if (!allPressed)
				combo.firedThisPress = false;
		} else if (combo.mode == "long_press") {
			if (allPressed) {
				if (combo.holdStart == 0)
					combo.holdStart = GetTickCount64();
				else if (!combo.firedThisPress &&
					(GetTickCount64() - combo.holdStart) >= (ULONGLONG)combo.timingMs) {
					SendScancode(combo.scancode);
					combo.firedThisPress = true;
					OOVR_LOGF("Combo fired (long_press): scancode 0x%02x", combo.scancode);
				}
			} else {
				combo.holdStart = 0;
				combo.firedThisPress = false;
			}
		} else {
			// Multi-tap modes: double_tap, triple_tap, quadruple_tap
			int requiredTaps = 2;
			if (combo.mode == "triple_tap") requiredTaps = 3;
			else if (combo.mode == "quadruple_tap" || combo.mode == "quad_tap") requiredTaps = 4;
			int timing = combo.timingMs > 0 ? combo.timingMs : 500;

			bool justPressed = allPressed && !combo.wasAllPressed;

			if (justPressed) {
				ULONGLONG now = GetTickCount64();
				if (combo.tapCount > 0 && (now - combo.tapTimes[combo.tapCount - 1]) > (ULONGLONG)timing)
					combo.tapCount = 0;
				if (combo.tapCount < 4)
					combo.tapTimes[combo.tapCount] = now;
				combo.tapCount++;
				if (combo.tapCount >= requiredTaps) {
					SendScancode(combo.scancode);
					combo.tapCount = 0;
					OOVR_LOGF("Combo fired (%s): scancode 0x%02x", combo.mode.c_str(), combo.scancode);
				}
			}
			// Expire stale taps
			if (combo.tapCount > 0 && (GetTickCount64() - combo.tapTimes[combo.tapCount - 1]) > (ULONGLONG)timing)
				combo.tapCount = 0;
		}
		combo.wasAllPressed = allPressed;
	}
}

// Class to represent an overlay
class BaseOverlay::OverlayData {
public:
	const string key;
	string name;
	HmdColor_t colour;

	float widthMeters = 1; // default 1 meter

	float autoCurveDistanceRangeMin, autoCurveDistanceRangeMax; // WTF does this do?
	EColorSpace colourSpace = ColorSpace_Auto;
	bool visible = false; // TODO check against SteamVR
	VRTextureBounds_t textureBounds = { 0, 0, 1, 1 };
	VROverlayInputMethod inputMethod = VROverlayInputMethod_None; // TODO fire events
	HmdVector2_t mouseScale = { 1.0f, 1.0f };
	bool highQuality = false;
	uint64_t flags = 0;
	float texelAspect = 1;
	std::queue<VREvent_t> eventQueue;
	std::mutex eventMutex; // protects eventQueue (written from main thread, read by SkyUI bg thread)

	// Rendering
	Texture_t texture = {};
	XrCompositionLayerQuad layerQuad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
	std::unique_ptr<Compositor> compositor;

	// Transform
	VROverlayTransformType transformType = VROverlayTransform_Absolute;
	union {
		struct {
			HmdMatrix34_t offset;
			TrackedDeviceIndex_t device;
		} deviceRelative;
	} transformData;

	MfMatrix4f overlayTransform{
		1.0f, 0.0f, 0.0f, 0.0f,
		0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, -1.01f,
		0.0f, 0.0f, 0.0f, 1.0f
	};

	OverlayData(string key, string name)
	    : key(key), name(name)
	{
	}
};

// TODO don't pass around handles, as it will cause
// crashes when we should merely return VROverlayError_InvalidHandle
#define OVL (*((OverlayData**)pOverlayHandle))
#define USEH()                                                                        \
	OverlayData* overlay = (OverlayData*)ulOverlayHandle;                             \
	if (!overlay || !validOverlays.count(overlay) || !overlays.count(overlay->key)) { \
		return VROverlayError_InvalidHandle;                                          \
	}

#define USEHB()                                           \
	OverlayData* overlay = (OverlayData*)ulOverlayHandle; \
	if (!overlay || !overlays.count(overlay->key)) {      \
		return false;                                     \
	}

BaseOverlay::~BaseOverlay()
{
	for (const auto& kv : overlays) {
		if (kv.second) {
			delete kv.second;
		}
	}
}

// SEH helper — reads ControlMap::textEntryCount safely.
// Must be a standalone function (no C++ objects with destructors) for __try/__except.
#ifdef _WIN32
static int8_t ReadTextEntryCount(uintptr_t gameBase) noexcept
{
	__try {
		uintptr_t controlMap = *(uintptr_t*)(gameBase + 0x2F8AAA0);
		if (controlMap)
			return *(int8_t*)(controlMap + 0x140);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
	return 0;
}
#endif

int BaseOverlay::_BuildLayers(XrCompositionLayerBaseHeader* sceneLayer, XrCompositionLayerBaseHeader const* const*& layers)
{
	// Note that at least on MSVC, this shouldn't be doing any memory allocations
	//  unless the list is expanding from new layers.
	layerHeaders.clear();
	if (sceneLayer)
		layerHeaders.push_back(sceneLayer);

	// Controller shortcut to open a SendInput-only keyboard (configurable via opencomposite.ini)
#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
	// Initialize shortcut settings from config on first run
	if (!s_shortcutSettingsInitialized) {
		InitShortcutSettings();
		LoadCombos();
		s_shortcutSettingsInitialized = true;
	}

	// Watch opencomposite.ini for shortcut setting changes (check once per second)
	{
		static ULONGLONG lastShortcutCheck = 0;
		static FILETIME lastShortcutWriteTime = {};
		ULONGLONG now = GetTickCount64();
		if (now - lastShortcutCheck > 1000) {
			lastShortcutCheck = now;
			wchar_t dllPath[MAX_PATH];
			GetModuleFileNameW(nullptr, dllPath, MAX_PATH);
			std::wstring settingsPath(dllPath);
			size_t pos = settingsPath.find_last_of(L"\\/");
			if (pos != std::wstring::npos)
				settingsPath = settingsPath.substr(0, pos + 1);
			settingsPath += L"opencomposite.ini";
			WIN32_FILE_ATTRIBUTE_DATA fad = {};
			if (GetFileAttributesExW(settingsPath.c_str(), GetFileExInfoStandard, &fad)) {
				if (CompareFileTime(&fad.ftLastWriteTime, &lastShortcutWriteTime) != 0) {
					lastShortcutWriteTime = fad.ftLastWriteTime;
					if (ReloadShortcutSettings()) {
						OOVR_LOGF("Shortcut settings reloaded: enabled=%d button=%s mode=%s timing=%d",
							s_shortcutEnabled, s_shortcutButton.c_str(), s_shortcutMode.c_str(), s_shortcutTiming);
					}
					LoadCombos();
				}
			}
		}
	}

	if (!keyboard && s_shortcutEnabled && BaseCompositor::dxcomp) {
		BaseSystem* sys = GetUnsafeBaseSystem();
		if (sys) {
			const std::string& btnName = s_shortcutButton;
			const std::string& modeName = s_shortcutMode;
			int timing = s_shortcutTiming;

			// Parse button combo string (e.g. "left_grip+right_grip" or "left_stick")
			// Each token maps to a controller index (0=left, 1=right) and a button mask.
			struct BtnReq { int ctrl; uint64_t mask; };
			std::vector<BtnReq> requirements;
			{
				std::istringstream ss(btnName);
				std::string token;
				while (std::getline(ss, token, '+')) {
					// trim whitespace
					while (!token.empty() && token.front() == ' ') token.erase(token.begin());
					while (!token.empty() && token.back() == ' ') token.pop_back();
					if (token.empty()) continue;

					int ci = -1;
					uint64_t bm = 0;
					if (token == "left_stick")       { ci = 0; bm = ButtonMaskFromId(k_EButton_SteamVR_Touchpad); }
					else if (token == "right_stick")  { ci = 1; bm = ButtonMaskFromId(k_EButton_SteamVR_Touchpad); }
					else if (token == "a")            { ci = 1; bm = ButtonMaskFromId(k_EButton_A); }
					else if (token == "b")            { ci = 1; bm = ButtonMaskFromId(k_EButton_ApplicationMenu); }
					else if (token == "x")            { ci = 0; bm = ButtonMaskFromId(k_EButton_A); }
					else if (token == "y")            { ci = 0; bm = ButtonMaskFromId(k_EButton_ApplicationMenu); }
					// Grip buttons reserved for "Exit Keyboard", triggers reserved for laser click — skip both
					else if (token == "left_grip" || token == "right_grip" || token == "both_grips") { continue; }
					else if (token == "left_trigger" || token == "right_trigger") { continue; }
					if (bm != 0)
						requirements.push_back({ ci, bm });
				}
			}

			if (!requirements.empty()) {
				// Get controller states using proper hand assignments (not hardcoded indices)
				VRControllerState_t ctrlState[2] = {};
				bool ctrlValid[2] = { false, false };
				TrackedDeviceIndex_t leftIdx = sys->GetTrackedDeviceIndexForControllerRole(TrackedControllerRole_LeftHand);
				TrackedDeviceIndex_t rightIdx = sys->GetTrackedDeviceIndexForControllerRole(TrackedControllerRole_RightHand);
				if (leftIdx != k_unTrackedDeviceIndexInvalid)
					ctrlValid[0] = sys->GetControllerState(leftIdx, &ctrlState[0], sizeof(ctrlState[0]));
				if (rightIdx != k_unTrackedDeviceIndexInvalid)
					ctrlValid[1] = sys->GetControllerState(rightIdx, &ctrlState[1], sizeof(ctrlState[1]));

				// Check ALL required buttons are pressed simultaneously
				bool btnPressed = true;
				for (const auto& req : requirements) {
					int idx = req.ctrl; // 0=left, 1=right
					if (!ctrlValid[idx]) {
						btnPressed = false;
						break;
					}
					// Check digital button bit first
					bool pressed = (ctrlState[idx].ulButtonPressed & req.mask) != 0;
					// Analog fallback for grip and trigger — Quest/Touch controllers
					// bind gripClick to squeeze/value (analog) and the OpenXR runtime's
					// boolean threshold may be too high to ever set the digital bit.
					if (!pressed) {
						if (req.mask == ButtonMaskFromId(k_EButton_Grip))
							pressed = ctrlState[idx].rAxis[2].x >= 0.5f;
						else if (req.mask == ButtonMaskFromId(k_EButton_Axis1))
							pressed = ctrlState[idx].rAxis[1].x >= 0.5f;
					}
					if (!pressed) {
						btnPressed = false;
						break;
					}
				}

				static ULONGLONG shortcutPressTime[4] = { 0, 0, 0, 0 };
				static int shortcutTapCount = 0;
				static bool shortcutBtnWasPressed = false;
				static ULONGLONG shortcutHoldStart = 0;

				int requiredTaps = 2;
				if (modeName == "triple_tap") requiredTaps = 3;
				else if (modeName == "quadruple_tap" || modeName == "quad_tap") requiredTaps = 4;

				bool activate = false;

				if (modeName == "long_press") {
					if (btnPressed) {
						if (shortcutHoldStart == 0)
							shortcutHoldStart = GetTickCount64();
						else if ((ULONGLONG)(GetTickCount64() - shortcutHoldStart) >= (ULONGLONG)timing)
							activate = true;
					} else {
						shortcutHoldStart = 0;
					}
				} else {
					bool justPressed = btnPressed && !shortcutBtnWasPressed;
					shortcutBtnWasPressed = btnPressed;

					if (justPressed) {
						ULONGLONG now = GetTickCount64();
						if (shortcutTapCount > 0 && (now - shortcutPressTime[shortcutTapCount - 1]) > (ULONGLONG)timing) {
							shortcutTapCount = 0;
						}
						shortcutPressTime[shortcutTapCount] = now;
						shortcutTapCount++;
						if (shortcutTapCount >= requiredTaps) {
							activate = true;
							shortcutTapCount = 0;
						}
					}
					if (shortcutTapCount > 0 && (GetTickCount64() - shortcutPressTime[shortcutTapCount - 1]) > (ULONGLONG)timing) {
						shortcutTapCount = 0;
					}
				}

				if (activate) {
					VRKeyboard::eventDispatch_t dispatch = [](VREvent_t ev) {
						BaseSystem* sys = GetUnsafeBaseSystem();
						if (sys) {
							sys->_EnqueueEvent(ev);
						}
					};
					keyboard = make_unique<VRKeyboard>(
					    BaseCompositor::dxcomp->GetDevice(), 0, 256, false, dispatch,
					    VRKeyboard::EGamepadTextInputMode::k_EGamepadTextInputModeNormal);
					keyboard->SetSendInputOnly(true);
					shortcutTapCount = 0;
					shortcutHoldStart = 0;
				}
			}
		}
	}

	// ── Controller combo processing — disabled while keyboard is open ──
	if (!keyboard && !s_combos.empty()) {
		BaseSystem* sys = GetUnsafeBaseSystem();
		if (sys) {
			VRControllerState_t comboCtrlState[2] = {};
			bool comboCtrlValid[2] = { false, false };
			TrackedDeviceIndex_t lIdx = sys->GetTrackedDeviceIndexForControllerRole(TrackedControllerRole_LeftHand);
			TrackedDeviceIndex_t rIdx = sys->GetTrackedDeviceIndexForControllerRole(TrackedControllerRole_RightHand);
			if (lIdx != k_unTrackedDeviceIndexInvalid)
				comboCtrlValid[0] = sys->GetControllerState(lIdx, &comboCtrlState[0], sizeof(comboCtrlState[0]));
			if (rIdx != k_unTrackedDeviceIndexInvalid)
				comboCtrlValid[1] = sys->GetControllerState(rIdx, &comboCtrlState[1], sizeof(comboCtrlState[1]));
			ProcessCombos(sys, comboCtrlState, comboCtrlValid);
		}
	}
#endif

	// ── Auto-detect game text input (AllowTextInput) and pop up VR keyboard ──
	// Skyrim VR 1.4.15: ControlMap singleton at SkyrimVR.exe+0x2F8AAA0 (Address Library ID 514705)
	// textEntryCount at ControlMap+0x140 (int8_t, >0 = text input active)
#if defined(SUPPORT_DX) && defined(SUPPORT_DX11) && defined(_WIN32)
	{
		static uintptr_t gameBase = 0;
		static bool gameBaseSearched = false;
		static bool textInputWasActive = false;
		static bool autoOpenedKeyboard = false;

		if (!gameBaseSearched) {
			gameBaseSearched = true;
			HMODULE hMod = GetModuleHandleW(L"SkyrimVR.exe");
			if (hMod)
				gameBase = (uintptr_t)hMod;
			if (gameBase)
				OOVR_LOGF("TextInput auto-detect: SkyrimVR.exe base = 0x%llX", (unsigned long long)gameBase);
			else
				OOVR_LOG("TextInput auto-detect: SkyrimVR.exe not found, disabled");
		}

		if (gameBase && BaseCompositor::dxcomp) {
			// Safe memory read helper (SEH can't be in functions with C++ destructors)
			int8_t textEntryCount = ReadTextEntryCount(gameBase);
			bool textInputActive = textEntryCount > 0;

			// Transition: text input just became active — auto-open keyboard
			if (textInputActive && !textInputWasActive && !keyboard) {
				OOVR_LOGF("TextInput auto-detect: textEntryCount=%d, opening VR keyboard", textEntryCount);
				VRKeyboard::eventDispatch_t dispatch = [](VREvent_t ev) {
					BaseSystem* sys = GetUnsafeBaseSystem();
					if (sys) {
						sys->_EnqueueEvent(ev);
					}
				};
				keyboard = make_unique<VRKeyboard>(
				    BaseCompositor::dxcomp->GetDevice(), 0, 256, false, dispatch,
				    VRKeyboard::EGamepadTextInputMode::k_EGamepadTextInputModeNormal);
				keyboard->SetSendInputOnly(true);
				autoOpenedKeyboard = true;
			}

			// Transition: text input ended while we auto-opened — close keyboard
			if (!textInputActive && textInputWasActive && keyboard && autoOpenedKeyboard) {
				OOVR_LOG("TextInput auto-detect: text input ended, closing VR keyboard");
				HideKeyboard();
				autoOpenedKeyboard = false;
			}

			textInputWasActive = textInputActive;
		}

		// Reset auto-open flag if keyboard was closed by user (grip, ESC, Done)
		if (!keyboard && autoOpenedKeyboard) {
			autoOpenedKeyboard = false;
		}
	}
#endif

	if (keyboard) {
		const auto& kbLayers = keyboard->Update();

		if (keyboard->IsClosed()) {
			OOVR_LOG("Keyboard closed, destroying before layer submission");
			HideKeyboard();
			g_kbLaserConsumesTrigger[0] = false;
			g_kbLaserConsumesTrigger[1] = false;
		} else {
			for (auto* l : kbLayers)
				layerHeaders.push_back(l);

			// Per-hand: mask trigger from game only when that hand's laser is on keyboard
			g_kbLaserConsumesTrigger[0] = keyboard->IsLaserOnKeyboard(0);
			g_kbLaserConsumesTrigger[1] = keyboard->IsLaserOnKeyboard(1);
		}
	} else {
		g_kbLaserConsumesTrigger[0] = false;
		g_kbLaserConsumesTrigger[1] = false;
	}

// =========================================================================
// [EXPERIMENTAL — DISABLED] MCM Menu Laser Pointer System
// =========================================================================
// This entire block (through the matching #endif EXPERIMENTAL) implements
// a VR laser pointer for Skyrim's Scaleform menus. It is DISABLED because:
//
// 1. SOVNGARDE BUG: The SKSE-side Scaleform mouse injection (WM_OC_LASER
//    → GFxMouseEvent/NotifyMouseState) corrupts VR rendering when it
//    accidentally accesses StatsMenu's Scaleform movie during Sovngarde's
//    constellation scene. The corruption is permanent until game restart.
//
// 2. MOUSE ALIGNMENT: Mapping laser UV hits (2D point on 3D plane) to
//    Scaleform cursor position requires either Scaleform injection (broken)
//    or SetCursorPos (needs calibration work since VR mirror window doesn't
//    map 1:1 to headset view).
//
// Components disabled:
// - Menu quad settings file watcher (menu_quad_settings.ini)
// - Per-menu hardcoded quad profiles (see profile table in comments above)
// - VRMenuLaser object lifecycle (creation, Update(), quad rendering)
// - In-VR thumbstick quad adjustment mode
// - WM_OC_LASER message sending to SKSE plugin
// - Quad visibility toggles (calibration quad, profile quad, Scaleform cursor)
//
// The VR keyboard overlay (above this block) is COMPLETELY SEPARATE and
// remains fully functional. Do not confuse the two systems.
//
// TODO: When re-enabling, extract into a separate file for proper separation
// of concerns. This code should NOT be interleaved in BaseOverlay::Submit().
// =========================================================================
#if 0 // [EXPERIMENTAL — DISABLED] Menu laser system
	// ── MCM Menu Laser Pointer System ──
	// Menu quad parameters — overrideable via menu_quad_settings.ini
	// File is watched every ~1 second (same pattern as keyboard_settings.ini).
	static float s_mqDist = 0.85f;
	static float s_mqWidthScale = 0.80f;
	static float s_mqHeightScale = 0.42f;
	static float s_mqYOffset = -0.13f;
	static float s_mqXOffset = 0;
	static float s_mqYawOffset = 0; // radians
	static float s_mqPitchOffset = 0; // radians
	static float s_mqRollOffset = 0; // radians
	static int   s_mqOpacity = 20;
	static bool  s_mqShowDebug = true;
	static bool  s_mqHeadLocked = false;
	static bool  s_mqThumbstickAdjust = false; // Toggleable from desktop calibrator
	// Mouse cursor calibration: offset + scale to align Scaleform cursor with VR laser
	static float s_mqMouseOffsetX = 0.0f; // fraction of screen width (positive = shift cursor right)
	static float s_mqMouseOffsetY = 0.0f; // fraction of screen height (positive = shift cursor down)
	static float s_mqMouseScaleX = 1.0f;  // UV scale multiplier for X
	static float s_mqMouseScaleY = 1.0f;  // UV scale multiplier for Y
	// Quad mode flags — controlled from Menu Quad Calibrator app
	static bool  s_mqShowCalibQuad = true;  // Show the movable green calibration quad
	static bool  s_mqShowProfileQuad = false; // Show pink profile quads (analysis mode)
	static bool  s_mqShowSfCursor = false; // Tell SKSE to make Scaleform cursor visible
	// (s_mqShowProfileQuad_prev and s_mqForceProfileReload removed —
	// profiles are now hardcoded and always applied on menu change)
	static FILETIME s_mqLastWrite = {};
	static ULONGLONG s_mqNextCheck = 0;
	static XrVector3f s_mqAnchorHeadPos = { 0, 0, 0 };
	static XrVector3f s_mqAnchorHeadFwd = { 0, 0, -1 };
	static float s_mqAnchorYaw = 0;
	static bool  s_mqHasAnchor = false;
	// When a per-menu profile is loaded, the file watcher skips quad
	// dimensions (distance, width, height, offsets, angles, opacity) so
	// the calibrator can't stomp them. Mouse params are always updated.
	static bool  s_profileActive = false;

	{
		ULONGLONG now = GetTickCount64();
		if (now >= s_mqNextCheck) {
			s_mqNextCheck = now + 1000; // check every 1 second
			// Use game EXE dir — USVFS intercepts and finds overwrite files
			char mqBuf[MAX_PATH];
			GetModuleFileNameA(nullptr, mqBuf, MAX_PATH);
			std::string mqPath(mqBuf);
			mqPath = mqPath.substr(0, mqPath.find_last_of("\\/")) + "\\menu_quad_settings.ini";
			WIN32_FILE_ATTRIBUTE_DATA mqAttr = {};
			if (GetFileAttributesExA(mqPath.c_str(), GetFileExInfoStandard, &mqAttr)) {
				if (CompareFileTime(&mqAttr.ftLastWriteTime, &s_mqLastWrite) != 0) {
					s_mqLastWrite = mqAttr.ftLastWriteTime;
					FILE* mf = fopen(mqPath.c_str(), "r");
					if (mf) {
						char line[256];
						while (fgets(line, sizeof(line), mf)) {
							float fv; int iv;
							// Quad dimensions — only apply when NO profile is active
							// (profiles lock these values so the calibrator can't stomp them)
							if (!s_profileActive) {
								if (sscanf(line, "distance=%f", &fv) == 1) { s_mqDist = fv; continue; }
								if (sscanf(line, "width_scale=%f", &fv) == 1) { s_mqWidthScale = fv; continue; }
								if (sscanf(line, "height_scale=%f", &fv) == 1) { s_mqHeightScale = fv; continue; }
								if (sscanf(line, "y_offset=%f", &fv) == 1) { s_mqYOffset = fv; continue; }
								if (sscanf(line, "x_offset=%f", &fv) == 1) { s_mqXOffset = fv; continue; }
								if (sscanf(line, "yaw_degrees=%d", &iv) == 1) { s_mqYawOffset = iv * 3.14159265f / 180.0f; continue; }
								if (sscanf(line, "pitch_degrees=%d", &iv) == 1) { s_mqPitchOffset = iv * 3.14159265f / 180.0f; continue; }
								if (sscanf(line, "roll_degrees=%d", &iv) == 1) { s_mqRollOffset = iv * 3.14159265f / 180.0f; continue; }
								if (sscanf(line, "opacity=%d", &iv) == 1) { s_mqOpacity = iv; continue; }
							}
							// These are ALWAYS read from settings.ini (even with profile active)
							if (sscanf(line, "show_debug=%d", &iv) == 1) s_mqShowDebug = (iv != 0);
							else if (sscanf(line, "head_locked=%d", &iv) == 1) {
								bool newLock = (iv != 0);
								if (newLock && !s_mqHeadLocked)
									s_mqHasAnchor = false;
								s_mqHeadLocked = newLock;
							}
							else if (sscanf(line, "thumbstick_adjust=%d", &iv) == 1) s_mqThumbstickAdjust = (iv != 0);
							else if (sscanf(line, "mouse_offset_x=%f", &fv) == 1) s_mqMouseOffsetX = fv;
							else if (sscanf(line, "mouse_offset_y=%f", &fv) == 1) s_mqMouseOffsetY = fv;
							else if (sscanf(line, "mouse_scale_x=%f", &fv) == 1) s_mqMouseScaleX = fv;
							else if (sscanf(line, "mouse_scale_y=%f", &fv) == 1) s_mqMouseScaleY = fv;
							else if (sscanf(line, "show_calibration_quad=%d", &iv) == 1) s_mqShowCalibQuad = (iv != 0);
							else if (sscanf(line, "show_profile_quad=%d", &iv) == 1) s_mqShowProfileQuad = (iv != 0);
							else if (sscanf(line, "show_sf_cursor=%d", &iv) == 1) s_mqShowSfCursor = (iv != 0);
						}
						fclose(mf);
						OOVR_LOGF("Menu quad settings: dist=%.3f wScale=%.3f hScale=%.3f yOff=%.3f xOff=%.3f yawDeg=%.1f pitchDeg=%.1f rollDeg=%.1f opacity=%d debug=%d headLock=%d",
						    s_mqDist, s_mqWidthScale, s_mqHeightScale, s_mqYOffset, s_mqXOffset,
						    s_mqYawOffset * 180.0f / 3.14159265f, s_mqPitchOffset * 180.0f / 3.14159265f, s_mqRollOffset * 180.0f / 3.14159265f,
						    s_mqOpacity, s_mqShowDebug ? 1 : 0, s_mqHeadLocked ? 1 : 0);

						// (Profile reload transition detection removed —
						// profiles are hardcoded and always applied on menu change)
					}
				}
			}
		}
	}

#ifdef _WIN32
	{
		bool menuActive = false;
		static HWND cachedHwnd = nullptr;
		if (!cachedHwnd || !IsWindow(cachedHwnd)) {
			cachedHwnd = FindWindowW(L"Skyrim Special Edition", nullptr);
			if (!cachedHwnd)
				cachedHwnd = FindWindowW(nullptr, L"Skyrim VR");
		}
		if (cachedHwnd)
			menuActive = (intptr_t)GetPropW(cachedHwnd, L"OC_MENU_ACTIVE") != 0;

		if (menuActive) {
			OOVR_LOG_ONCE("MCM menu detected active via OC_MENU_ACTIVE property");

			// Try to open shared memory from SKSE plugin (once)
			OpenSharedMemory();

			// Create menu laser system on first detection
			static bool s_feedbackWritten = false;

			if (!menuLaser && BaseCompositor::dxcomp) {
				OOVR_LOG("Creating VRMenuLaser system");
				menuLaser = std::make_unique<VRMenuLaser>(BaseCompositor::dxcomp->GetDevice());
				s_mqHasAnchor = false; // Re-anchor from current head on menu reopen
				s_feedbackWritten = false; // Write feedback on first anchored frame

				// Log head pose at menu open
				XrSpaceLocation openHead = { XR_TYPE_SPACE_LOCATION };
				xrLocateSpace(xr_gbl->viewSpace, xr_gbl->floorSpace,
				    xr_gbl->nextPredictedFrameTime, &openHead);
				OOVR_LOGF("CAL MENU-OPEN head(%.4f, %.4f, %.4f) orient(%.4f, %.4f, %.4f, %.4f)",
				    openHead.pose.position.x,
				    openHead.pose.position.y,
				    openHead.pose.position.z,
				    openHead.pose.orientation.x,
				    openHead.pose.orientation.y,
				    openHead.pose.orientation.z,
				    openHead.pose.orientation.w);

				// Apply hardcoded per-menu quad profile on menu open.
				// Each menu has its own calibrated quad size/position so
				// the laser interaction area matches the Scaleform extent.
				OCMenuTransform mxOpen = {};
				bool readOk = ReadMenuTransform(mxOpen);
				OOVR_LOGF("MENU-OPEN: ReadMenuTransform=%s menuName='%s'",
				    readOk ? "OK" : "FAIL", readOk ? mxOpen.menuName : "(n/a)");
				if (readOk && mxOpen.menuName[0] != '\0') {
					float pDist, pW, pH, pYOff, pXOff;
					int pOpacity;
					GetMenuProfile(mxOpen.menuName, pDist, pW, pH, pYOff, pXOff, pOpacity);
					s_profileActive = true; // Lock quad dims — file watcher won't override
					s_mqDist = pDist; s_mqWidthScale = pW; s_mqHeightScale = pH;
					s_mqYOffset = pYOff; s_mqXOffset = pXOff;
					s_mqYawOffset = 0; s_mqPitchOffset = 0; s_mqRollOffset = 0;
					s_mqOpacity = pOpacity;
				}
			}

			if (menuLaser) {
				// Detect menu name changes (e.g., TweenMenu → MagicMenu) and reload profile
				static char s_lastMenuName[64] = {};
				OCMenuTransform mxCheck = {};
				if (ReadMenuTransform(mxCheck) && mxCheck.menuName[0] != '\0') {
					bool nameChanged = (strcmp(s_lastMenuName, mxCheck.menuName) != 0);
					if (nameChanged) {
						strncpy(s_lastMenuName, mxCheck.menuName, sizeof(s_lastMenuName) - 1);
						s_lastMenuName[sizeof(s_lastMenuName) - 1] = '\0';
						OOVR_LOGF("Menu changed to '%s' — applying hardcoded profile", s_lastMenuName);

						// Apply hardcoded profile for the new menu.
						// Always applies — quad automatically resizes per menu.
						{
							float pDist, pW, pH, pYOff, pXOff;
							int pOpacity;
							GetMenuProfile(mxCheck.menuName, pDist, pW, pH, pYOff, pXOff, pOpacity);
							s_profileActive = true;
							s_mqDist = pDist; s_mqWidthScale = pW; s_mqHeightScale = pH;
							s_mqYOffset = pYOff; s_mqXOffset = pXOff;
							s_mqYawOffset = 0; s_mqPitchOffset = 0; s_mqRollOffset = 0;
							s_mqOpacity = pOpacity;
						}
					}
				}

				// Compute menu quad from head position + settings
				XrSpaceLocation headLoc = { XR_TYPE_SPACE_LOCATION };
				xrLocateSpace(xr_gbl->viewSpace, xr_gbl->floorSpace,
				    xr_gbl->nextPredictedFrameTime, &headLoc);

				XrVector3f headPos = headLoc.pose.position;
				XrVector3f headFwd, headRight, headUp;
				rotate_vector_by_quaternion({ 0, 0, -1 }, headLoc.pose.orientation, headFwd);
				rotate_vector_by_quaternion({ 1, 0, 0 }, headLoc.pose.orientation, headRight);
				rotate_vector_by_quaternion({ 0, 1, 0 }, headLoc.pose.orientation, headUp);

				// Anchor on first frame (or when head-locked mode changes)
				if (!s_mqHasAnchor || s_mqHeadLocked) {
					s_mqAnchorHeadPos = headPos;
					s_mqAnchorHeadFwd = headFwd;
					s_mqAnchorYaw = atan2f(headFwd.x, headFwd.z);
					s_mqHasAnchor = true;
				}

				// Use anchored or live head depending on mode
				XrVector3f usePos = s_mqHeadLocked ? headPos : s_mqAnchorHeadPos;
				XrVector3f useFwd = s_mqHeadLocked ? headFwd : s_mqAnchorHeadFwd;
				XrVector3f useRight, useUp;
				if (s_mqHeadLocked) {
					useRight = headRight;
					useUp = headUp;
				} else {
					// Reconstruct right/up from anchored forward (level ground)
					float yaw = s_mqAnchorYaw;
					useFwd = { sinf(yaw), 0, cosf(yaw) };
					useRight = { -cosf(yaw), 0, sinf(yaw) };
					useUp = { 0, 1, 0 };
				}

				// Quad center: distance forward + offsets
				XrVector3f quadCenter = {
					usePos.x + useFwd.x * s_mqDist + useRight.x * s_mqXOffset + useUp.x * s_mqYOffset,
					usePos.y + useFwd.y * s_mqDist + useRight.y * s_mqXOffset + useUp.y * s_mqYOffset,
					usePos.z + useFwd.z * s_mqDist + useRight.z * s_mqXOffset + useUp.z * s_mqYOffset
				};

				// Build orientation: face the player, then apply yaw/pitch/roll offsets
				// Base orientation: quad faces -Z (toward player), with Y up
				float yaw = atan2f(-useFwd.x, -useFwd.z) + s_mqYawOffset;
				float pitch = s_mqPitchOffset;
				float roll = s_mqRollOffset;

				// Euler to quaternion (YXZ order: yaw, pitch, roll)
				float cy = cosf(yaw * 0.5f), sy = sinf(yaw * 0.5f);
				float cp = cosf(pitch * 0.5f), sp = sinf(pitch * 0.5f);
				float cr = cosf(roll * 0.5f), sr = sinf(roll * 0.5f);
				XrQuaternionf quadOrient = {
					cy * sp * cr + sy * cp * sr,  // x
					sy * cp * cr - cy * sp * sr,  // y
					cy * cp * sr - sy * sp * cr,  // z
					cy * cp * cr + sy * sp * sr   // w
				};

				XrPosef quadPose;
				quadPose.position = quadCenter;
				quadPose.orientation = quadOrient;

				XrExtent2Df quadSize = { s_mqWidthScale, s_mqHeightScale };

				menuLaser->SetMenuQuad(quadPose, quadSize);
				// Quad visibility depends on mode checkboxes from Calibrator app:
				// - show_profile_quad=1: pink profile quads (analysis mode)
				// - show_calibration_quad=1: green calibration quad (adjustment mode)
				// - both off: no quad (mouse calibration mode)
				// Profile quad takes priority — if both are on, show profile (pink).
				bool showAnyQuad = s_mqShowCalibQuad || s_mqShowProfileQuad;
				menuLaser->SetShowDebugQuad(showAnyQuad);
				if (s_mqShowProfileQuad) {
					menuLaser->SetDebugQuadColor(180, 50, 180); // pink/magenta
				} else {
					menuLaser->SetDebugQuadColor(30, 100, 30);  // green (default)
				}
				menuLaser->SetDebugQuadOpacity(s_mqOpacity);

				// Write feedback file once per menu open (head pos, menu name, quad placement)
				if (!s_feedbackWritten) {
					s_feedbackWritten = true;
					char fbBuf[MAX_PATH];
					GetModuleFileNameA(nullptr, fbBuf, MAX_PATH);
					std::string fbPath(fbBuf);
					fbPath = fbPath.substr(0, fbPath.find_last_of("\\/")) + "\\menu_quad_feedback.txt";

					// Try to get menu name from shared memory
					char menuNameStr[64] = "Unknown";
					OCMenuTransform mxform = {};
					if (ReadMenuTransform(mxform) && mxform.menuName[0] != '\0') {
						strncpy(menuNameStr, mxform.menuName, sizeof(menuNameStr) - 1);
						menuNameStr[sizeof(menuNameStr) - 1] = '\0';
					}

					float headYawDeg = s_mqAnchorYaw * 180.0f / 3.14159265f;

					FILE* fbf = fopen(fbPath.c_str(), "w");
					if (fbf) {
						fprintf(fbf,
						    "[feedback]\n"
						    "menu_name=%s\n"
						    "head_x=%.4f\n"
						    "head_y=%.4f\n"
						    "head_z=%.4f\n"
						    "head_yaw=%.1f\n"
						    "quad_x=%.4f\n"
						    "quad_y=%.4f\n"
						    "quad_z=%.4f\n"
						    "quad_width=%.2f\n"
						    "quad_height=%.2f\n"
						    "distance=%.2f\n"
						    "x_offset=%.2f\n"
						    "y_offset=%.2f\n",
						    menuNameStr,
						    s_mqAnchorHeadPos.x, s_mqAnchorHeadPos.y, s_mqAnchorHeadPos.z,
						    headYawDeg,
						    quadCenter.x, quadCenter.y, quadCenter.z,
						    s_mqWidthScale, s_mqHeightScale,
						    s_mqDist, s_mqXOffset, s_mqYOffset);
						fclose(fbf);
						OOVR_LOGF("Feedback: menu=%s head(%.3f,%.3f,%.3f) yaw=%.1f quad(%.3f,%.3f,%.3f)",
						    menuNameStr,
						    s_mqAnchorHeadPos.x, s_mqAnchorHeadPos.y, s_mqAnchorHeadPos.z,
						    headYawDeg,
						    quadCenter.x, quadCenter.y, quadCenter.z);
					}
				}

				// Suppress laser beams/dots for menus that have their own pointer
				// or 3D perspective content (Sovngarde bug)
				bool suppressLaser = (strcmp(s_lastMenuName, "MapMenu") == 0)
				    || (strcmp(s_lastMenuName, "StatsMenu") == 0)
				    || (strcmp(s_lastMenuName, "Loading Menu") == 0)
				    || (strcmp(s_lastMenuName, "Main Menu") == 0)
				    || (strcmp(s_lastMenuName, "Mist Menu") == 0);

				bool kbHit[2] = { g_kbLaserConsumesTrigger[0], g_kbLaserConsumesTrigger[1] };
				if (!suppressLaser) {
					const auto& menuLayers = menuLaser->Update(xr_gbl->nextPredictedFrameTime, kbHit);
					for (auto* l : menuLayers)
						layerHeaders.push_back(l);
				}

				// Set g_menuLaserActive if either hand is hitting the quad (but not for suppressed menus)
				g_menuLaserActive = !suppressLaser && (menuLaser->IsHit(0) || menuLaser->IsHit(1));

				// ── In-VR Quad Adjustment (thumbstick click toggles) ──
				// Left thumbstick click toggles adjustment mode.
				// In adjustment mode:
				//   Left stick Y = distance, Left stick X = x offset
				//   Right stick Y = y offset, Right stick X = width/height/opacity (X btn cycles)
				static bool s_adjustModeLocal = false; // toggled by thumbstick click in VR
				static int  s_rightStickParam = 0; // 0=width, 1=height, 2=opacity
				static bool s_adjustDirty = false;
				static ULONGLONG s_adjustLastSave = 0;

				// Left thumbstick click toggles local adjustment mode
				if (menuLaser->IsThumbstickPressed(0)) {
					s_adjustModeLocal = !s_adjustModeLocal;
					OOVR_LOGF("Menu quad adjustment mode: %s", s_adjustModeLocal ? "ON" : "OFF");
				}

				// Active if either local toggle OR ini toggle is on
				bool s_adjustMode = s_adjustModeLocal || s_mqThumbstickAdjust;

				// X button cycles right-stick parameter
				if (s_adjustMode && menuLaser->IsXButtonPressed(0)) {
					s_rightStickParam = (s_rightStickParam + 1) % 3;
					const char* names[] = { "Width", "Height", "Opacity" };
					OOVR_LOGF("Right stick adjusts: %s", names[s_rightStickParam]);
				}

				if (s_adjustMode) {
					constexpr float DEADZONE = 0.15f;
					constexpr float SPEED = 0.0005f; // meters per frame at full tilt
					constexpr float SCALE_SPEED = 0.0003f;

					float lx = menuLaser->GetThumbstickX(0);
					float ly = menuLaser->GetThumbstickY(0);
					float rx = menuLaser->GetThumbstickX(1);
					float ry = menuLaser->GetThumbstickY(1);

					// Left stick: distance (Y) and x offset (X)
					if (fabsf(ly) > DEADZONE) {
						s_mqDist += ly * SPEED;
						if (s_mqDist < 0.1f) s_mqDist = 0.1f;
						if (s_mqDist > 10.0f) s_mqDist = 10.0f;
						s_adjustDirty = true;
					}
					if (fabsf(lx) > DEADZONE) {
						s_mqXOffset += lx * SPEED;
						if (s_mqXOffset < -5.0f) s_mqXOffset = -5.0f;
						if (s_mqXOffset > 5.0f) s_mqXOffset = 5.0f;
						s_adjustDirty = true;
					}

					// Right stick Y: y offset
					if (fabsf(ry) > DEADZONE) {
						s_mqYOffset += ry * SPEED;
						if (s_mqYOffset < -5.0f) s_mqYOffset = -5.0f;
						if (s_mqYOffset > 5.0f) s_mqYOffset = 5.0f;
						s_adjustDirty = true;
					}

					// Right stick X: width, height, or opacity depending on mode
					if (fabsf(rx) > DEADZONE) {
						if (s_rightStickParam == 0) {
							s_mqWidthScale += rx * SCALE_SPEED;
							if (s_mqWidthScale < 0.1f) s_mqWidthScale = 0.1f;
							if (s_mqWidthScale > 5.0f) s_mqWidthScale = 5.0f;
						} else if (s_rightStickParam == 1) {
							s_mqHeightScale += rx * SCALE_SPEED;
							if (s_mqHeightScale < 0.1f) s_mqHeightScale = 0.1f;
							if (s_mqHeightScale > 5.0f) s_mqHeightScale = 5.0f;
						} else {
							s_mqOpacity += (int)(rx * 0.5f);
							if (s_mqOpacity < 1) s_mqOpacity = 1;
							if (s_mqOpacity > 100) s_mqOpacity = 100;
						}
						s_adjustDirty = true;
					}

					// Auto-save to ini every 2 seconds if dirty
					ULONGLONG nowMs = GetTickCount64();
					if (s_adjustDirty && (nowMs - s_adjustLastSave) > 2000) {
						s_adjustDirty = false;
						s_adjustLastSave = nowMs;

						char mqBuf[MAX_PATH];
						GetModuleFileNameA(nullptr, mqBuf, MAX_PATH);
						std::string mqSavePath(mqBuf);
						mqSavePath = mqSavePath.substr(0, mqSavePath.find_last_of("\\/")) + "\\menu_quad_settings.ini";
						FILE* sf = fopen(mqSavePath.c_str(), "w");
						if (sf) {
							fprintf(sf, "[menu_quad]\ndistance=%.2f\nwidth_scale=%.2f\nheight_scale=%.2f\n"
							    "y_offset=%.2f\nx_offset=%.2f\nyaw_degrees=%d\npitch_degrees=%d\n"
							    "roll_degrees=%d\nopacity=%d\nshow_debug=%d\nhead_locked=%d\nthumbstick_adjust=%d\n"
							    "mouse_offset_x=%.3f\nmouse_offset_y=%.3f\nmouse_scale_x=%.3f\nmouse_scale_y=%.3f\n"
							    "show_calibration_quad=%d\nshow_profile_quad=%d\n",
							    s_mqDist, s_mqWidthScale, s_mqHeightScale,
							    s_mqYOffset, s_mqXOffset,
							    (int)(s_mqYawOffset * 180.0f / 3.14159265f),
							    (int)(s_mqPitchOffset * 180.0f / 3.14159265f),
							    (int)(s_mqRollOffset * 180.0f / 3.14159265f),
							    s_mqOpacity, s_mqShowDebug ? 1 : 0, s_mqHeadLocked ? 1 : 0, s_mqThumbstickAdjust ? 1 : 0,
							    s_mqMouseOffsetX, s_mqMouseOffsetY, s_mqMouseScaleX, s_mqMouseScaleY,
							    s_mqShowCalibQuad ? 1 : 0, s_mqShowProfileQuad ? 1 : 0);
							fclose(sf);
							OOVR_LOGF("Saved quad settings: dist=%.2f w=%.2f h=%.2f yOff=%.2f xOff=%.2f opacity=%d",
							    s_mqDist, s_mqWidthScale, s_mqHeightScale, s_mqYOffset, s_mqXOffset, s_mqOpacity);
						}
					}
				}

				// Send laser UV hits to SKSE plugin as WM_OC_LASER messages.
				// The plugin injects GFxMouseEvent directly into Scaleform,
				// bypassing Windows mouse entirely.
				if (!suppressLaser && cachedHwnd)
				for (int side = 0; side < 2; side++) {
					if (!menuLaser->IsHit(side)) continue;

					float u = menuLaser->GetHitU(side);
					float v = menuLaser->GetHitV(side);

					// Apply calibration offsets
					float adjU = u * s_mqMouseScaleX + s_mqMouseOffsetX;
					float adjV = v * s_mqMouseScaleY + s_mqMouseOffsetY;

					// Pack UV into WPARAM: low 16 = u*10000, high 16 = v*10000
					WORD uPacked = (WORD)(adjU * 10000.0f);
					WORD vPacked = (WORD)(adjV * 10000.0f);
					WPARAM packedUV = MAKEWPARAM(uPacked, vPacked);

					// LPARAM encoding: bits 0-7 = action, bit 8 = show_sf_cursor
					LPARAM cursorBit = s_mqShowSfCursor ? 0x100 : 0;

					// Always send mouse move
					PostMessage(cachedHwnd, WM_OC_LASER, packedUV, 0 | cursorBit);

					// Send press/release on trigger edges
					if (menuLaser->IsTriggerPressed(side))
						PostMessage(cachedHwnd, WM_OC_LASER, packedUV, 1 | cursorBit);
					if (menuLaser->IsTriggerReleased(side))
						PostMessage(cachedHwnd, WM_OC_LASER, packedUV, 2 | cursorBit);

					break; // Only one hand controls the mouse at a time
				}
			}
		} else {
			// Menu closed — destroy laser system, unlock profile
			if (menuLaser)
				menuLaser.reset();
			g_menuLaserActive = false;
			s_profileActive = false; // Allow file watcher to update quad dims again
		}
	}
#endif // _WIN32 (inside #if 0 block)
#endif // [EXPERIMENTAL — DISABLED] Menu laser system

	if (!oovr_global_configuration.EnableLayers()) {
		goto done;
	}

	for (const auto& kv : overlays) {
		if (kv.second) {
			OverlayData& overlay = *kv.second;

			// Skip hiddden overlays, and those without a valid texture (eg, after calling ClearOverlayTexture).
			if (!overlay.visible || overlay.texture.handle == nullptr)
				continue;

			// Quick hack to get around Boneworks creating overlays and setting them to an opacity of
			// zero to hide them. Leave 1% of margin in case of weird float issues.
			// if (overlay.colour.a < 0.01)
			//	continue;

			if ((uint64_t)overlay.layerQuad.subImage.swapchain == 0) {
				continue;
			}

			const XrRect2Di& srcSize = overlay.layerQuad.subImage.imageRect;
			if (srcSize.extent.height <= 8 && srcSize.extent.width <= 8) {
				// Hack for F1 22 which creates a low res texture to fade between scenes
				// but ends up just leaving a black square that takes up half the screen.
				continue;
			}

			// Calculate the texture's aspect ratio
			const float aspect = srcSize.extent.height > 0 ? (float)srcSize.extent.width / (float)srcSize.extent.height : 1.0f;
			// ... and use that to set the size of the overlay, as it will appear to the user
			// Note we shouldn't do this when setting the texture, as the user may change the width of
			//  the overlay without changing the texture.
			overlay.layerQuad.size.width = overlay.widthMeters * overlay.overlayTransform[0][0];
			overlay.layerQuad.size.height = overlay.widthMeters * overlay.overlayTransform[1][1] / aspect;

			// Finally, add it to the list of layers to be sent to LibOVR
			overlay.layerQuad.pose = { { 0.f, 0.f, 0.f, 1.f },
				{ overlay.overlayTransform[0][3], overlay.overlayTransform[1][3], overlay.overlayTransform[2][3] } };

			layerHeaders.push_back((XrCompositionLayerBaseHeader*)&overlay.layerQuad);
		}
	}

done:
	layers = layerHeaders.data();
	return static_cast<int>(layerHeaders.size());
}

EVROverlayError BaseOverlay::FindOverlay(const char* pchOverlayKey, VROverlayHandle_t* pOverlayHandle)
{
	if (overlays.count(pchOverlayKey)) {
		OVL = overlays[pchOverlayKey];
		return VROverlayError_None;
	}

	// TODO is this the correct return value
	return VROverlayError_InvalidParameter;
}
EVROverlayError BaseOverlay::CreateOverlay(const char* pchOverlayKey, const char* pchOverlayName, VROverlayHandle_t* pOverlayHandle)
{
	if (overlays.count(pchOverlayKey)) {
		return VROverlayError_KeyInUse;
	}

	OverlayData* data = new OverlayData(pchOverlayKey, pchOverlayName);
	OVL = data;

	overlays[pchOverlayKey] = data;
	validOverlays.insert(data);

	data->layerQuad.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
	data->layerQuad.next = NULL;
	data->layerQuad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
	data->layerQuad.space = xr_space_from_ref_space_type(GetUnsafeBaseSystem()->currentSpace);
	data->layerQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
	data->layerQuad.pose = { { 0.f, 0.f, 0.f, 1.f },
		{ 0.0f, 0.0f, -0.65f } };

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::DestroyOverlay(VROverlayHandle_t ulOverlayHandle)
{
	USEH();

	if (highQualityOverlay == ulOverlayHandle)
		highQualityOverlay = vr::k_ulOverlayHandleInvalid;

	overlays.erase(overlay->key);
	validOverlays.erase(overlay);
	delete overlay;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetHighQualityOverlay(VROverlayHandle_t ulOverlayHandle)
{
	USEH();

	highQualityOverlay = ulOverlayHandle;

	return VROverlayError_None;
}
VROverlayHandle_t BaseOverlay::GetHighQualityOverlay()
{
	if (!highQualityOverlay)
		return k_ulOverlayHandleInvalid;

	return highQualityOverlay;
}
uint32_t BaseOverlay::GetOverlayKey(VROverlayHandle_t ulOverlayHandle, char* pchValue, uint32_t unBufferSize, EVROverlayError* pError)
{
	OverlayData* overlay = (OverlayData*)ulOverlayHandle;
	if (!overlays.count(overlay->key)) {
		if (pError)
			*pError = VROverlayError_InvalidHandle;
		if (unBufferSize != 0)
			pchValue = 0;
		return 0;
	}

	const char* key = overlay->key.c_str();
	strncpy_s(pchValue, unBufferSize, key, unBufferSize);

	if (strlen(key) >= unBufferSize && unBufferSize != 0) {
		pchValue[unBufferSize - 1] = 0;
	}

	if (pError)
		*pError = VROverlayError_None;

	// Is this supposed to include the NULL or not?
	// TODO test, this could cause some very nasty bugs
	return static_cast<uint32_t>(strlen(pchValue) + 1);
}
uint32_t BaseOverlay::GetOverlayName(VROverlayHandle_t ulOverlayHandle, VR_OUT_STRING() char* pchValue, uint32_t unBufferSize, EVROverlayError* pError)
{
	if (pError)
		*pError = VROverlayError_None;

	OverlayData* overlay = (OverlayData*)ulOverlayHandle;
	if (!overlays.count(overlay->key)) {
		if (pError)
			*pError = VROverlayError_InvalidHandle;
		if (unBufferSize != 0)
			pchValue[0] = 0;
		return 0;
	}

	const char* name = overlay->name.c_str();
	strncpy_s(pchValue, unBufferSize, name, unBufferSize);

	if (strlen(name) >= unBufferSize && unBufferSize != 0) {
		pchValue[unBufferSize - 1] = 0;
	}

	// Is this supposed to include the NULL or not?
	// TODO test, this could cause some very nasty bugs
	return static_cast<uint32_t>(strlen(pchValue) + 1);
}
EVROverlayError BaseOverlay::SetOverlayName(VROverlayHandle_t ulOverlayHandle, const char* pchName)
{
	USEH();

	overlay->name = pchName;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayImageData(VROverlayHandle_t ulOverlayHandle, void* pvBuffer, uint32_t unBufferSize, uint32_t* punWidth, uint32_t* punHeight)
{
	STUBBED();
}
const char* BaseOverlay::GetOverlayErrorNameFromEnum(EVROverlayError error)
{
#define ERR_CASE(name)          \
	case VROverlayError_##name: \
		return #name;
	switch (error) {
		ERR_CASE(None);
		ERR_CASE(UnknownOverlay);
		ERR_CASE(InvalidHandle);
		ERR_CASE(PermissionDenied);
		ERR_CASE(OverlayLimitExceeded);
		ERR_CASE(WrongVisibilityType);
		ERR_CASE(KeyTooLong);
		ERR_CASE(NameTooLong);
		ERR_CASE(KeyInUse);
		ERR_CASE(WrongTransformType);
		ERR_CASE(InvalidTrackedDevice);
		ERR_CASE(InvalidParameter);
		ERR_CASE(ThumbnailCantBeDestroyed);
		ERR_CASE(ArrayTooSmall);
		ERR_CASE(RequestFailed);
		ERR_CASE(InvalidTexture);
		ERR_CASE(UnableToLoadFile);
		ERR_CASE(KeyboardAlreadyInUse);
		ERR_CASE(NoNeighbor);
		ERR_CASE(TooManyMaskPrimitives);
		ERR_CASE(BadMaskPrimitive);
	}
#undef ERR_CASE

	string msg = "Unknown overlay error code: " + to_string(error);
	OOVR_LOG(msg.c_str());

	STUBBED();
}
EVROverlayError BaseOverlay::SetOverlayRenderingPid(VROverlayHandle_t ulOverlayHandle, uint32_t unPID)
{
	STUBBED();
}
uint32_t BaseOverlay::GetOverlayRenderingPid(VROverlayHandle_t ulOverlayHandle)
{
	STUBBED();
}
EVROverlayError BaseOverlay::SetOverlayFlag(VROverlayHandle_t ulOverlayHandle, VROverlayFlags eOverlayFlag, bool bEnabled)
{
	USEH();

	if (bEnabled) {
		overlay->flags |= 1uLL << eOverlayFlag;
	} else {
		overlay->flags &= ~(1uLL << eOverlayFlag);
	}

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayFlag(VROverlayHandle_t ulOverlayHandle, VROverlayFlags eOverlayFlag, bool* pbEnabled)
{
	USEH();

	*pbEnabled = (overlay->flags & (1uLL << eOverlayFlag)) != 0uLL;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayColor(VROverlayHandle_t ulOverlayHandle, float fRed, float fGreen, float fBlue)
{
	USEH();

	overlay->colour.r = fRed;
	overlay->colour.g = fGreen;
	overlay->colour.b = fBlue;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayColor(VROverlayHandle_t ulOverlayHandle, float* pfRed, float* pfGreen, float* pfBlue)
{
	USEH();

	*pfRed = overlay->colour.r;
	*pfGreen = overlay->colour.g;
	*pfBlue = overlay->colour.b;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayAlpha(VROverlayHandle_t ulOverlayHandle, float fAlpha)
{
	USEH();

	overlay->colour.a = fAlpha;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayAlpha(VROverlayHandle_t ulOverlayHandle, float* pfAlpha)
{
	USEH();

	*pfAlpha = overlay->colour.a;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayTexelAspect(VROverlayHandle_t ulOverlayHandle, float fTexelAspect)
{
	USEH();

	overlay->texelAspect = fTexelAspect;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayTexelAspect(VROverlayHandle_t ulOverlayHandle, float* pfTexelAspect)
{
	USEH();

	if (!pfTexelAspect)
		OOVR_ABORT("pfTexelAspect == nullptr");

	*pfTexelAspect = overlay->texelAspect;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlaySortOrder(VROverlayHandle_t ulOverlayHandle, uint32_t unSortOrder)
{
	// TODO
	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlaySortOrder(VROverlayHandle_t ulOverlayHandle, uint32_t* punSortOrder)
{
	STUBBED();
}
EVROverlayError BaseOverlay::SetOverlayWidthInMeters(VROverlayHandle_t ulOverlayHandle, float fWidthInMeters)
{
	USEH();

	overlay->widthMeters = fWidthInMeters;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayWidthInMeters(VROverlayHandle_t ulOverlayHandle, float* pfWidthInMeters)
{
	USEH();

	*pfWidthInMeters = overlay->widthMeters;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayCurvature(VROverlayHandle_t ulOverlayHandle, float fCurvature)
{
	STUBBED();
}
EVROverlayError BaseOverlay::GetOverlayCurvature(VROverlayHandle_t ulOverlayHandle, float* pfCurvature)
{
	STUBBED();
}
EVROverlayError BaseOverlay::SetOverlayAutoCurveDistanceRangeInMeters(VROverlayHandle_t ulOverlayHandle, float fMinDistanceInMeters, float fMaxDistanceInMeters)
{
	USEH();

	overlay->autoCurveDistanceRangeMin = fMinDistanceInMeters;
	overlay->autoCurveDistanceRangeMax = fMaxDistanceInMeters;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayAutoCurveDistanceRangeInMeters(VROverlayHandle_t ulOverlayHandle, float* pfMinDistanceInMeters, float* pfMaxDistanceInMeters)
{
	USEH();

	*pfMinDistanceInMeters = overlay->autoCurveDistanceRangeMin;
	*pfMaxDistanceInMeters = overlay->autoCurveDistanceRangeMax;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayTextureColorSpace(VROverlayHandle_t ulOverlayHandle, EColorSpace eTextureColorSpace)
{
	USEH();

	overlay->colourSpace = eTextureColorSpace;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayTextureColorSpace(VROverlayHandle_t ulOverlayHandle, EColorSpace* peTextureColorSpace)
{
	USEH();

	*peTextureColorSpace = overlay->colourSpace;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayTextureBounds(VROverlayHandle_t ulOverlayHandle, const VRTextureBounds_t* pOverlayTextureBounds)
{
	USEH();

	if (pOverlayTextureBounds)
		overlay->textureBounds = *pOverlayTextureBounds;
	else
		overlay->textureBounds = { 0, 0, 1, 1 };

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayTextureBounds(VROverlayHandle_t ulOverlayHandle, VRTextureBounds_t* pOverlayTextureBounds)
{
	USEH();

	*pOverlayTextureBounds = overlay->textureBounds;

	return VROverlayError_None;
}
uint32_t BaseOverlay::GetOverlayRenderModel(VROverlayHandle_t ulOverlayHandle, char* pchValue, uint32_t unBufferSize, HmdColor_t* pColor, EVROverlayError* pError)
{
	if (pError)
		*pError = VROverlayError_None;

	STUBBED();
}
EVROverlayError BaseOverlay::SetOverlayRenderModel(VROverlayHandle_t ulOverlayHandle, const char* pchRenderModel, const HmdColor_t* pColor)
{
	STUBBED();
}
EVROverlayError BaseOverlay::GetOverlayTransformType(VROverlayHandle_t ulOverlayHandle, VROverlayTransformType* peTransformType)
{
	USEH();

	*peTransformType = overlay->transformType;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayTransformAbsolute(VROverlayHandle_t ulOverlayHandle, ETrackingUniverseOrigin eTrackingOrigin, const HmdMatrix34_t* pmatTrackingOriginToOverlayTransform)
{
	USEH();

	// TODO account for the universe origin, and if it doesn't match that currently in use then add or
	//  subtract the floor position to match it. This shouldn't usually be an issue though, as I can't
	//  imagine many apps will use a different origin for their overlays.

	overlay->transformType = VROverlayTransform_Absolute;
	S2O_om44(*pmatTrackingOriginToOverlayTransform, overlay->overlayTransform);

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayTransformAbsolute(VROverlayHandle_t ulOverlayHandle, ETrackingUniverseOrigin* peTrackingOrigin, HmdMatrix34_t* pmatTrackingOriginToOverlayTransform)
{
	USEH();

	if (overlay->transformType != VROverlayTransform_Absolute)
		return VROverlayError_WrongTransformType;

	O2S_om34(overlay->overlayTransform, *pmatTrackingOriginToOverlayTransform);

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayTransformTrackedDeviceRelative(VROverlayHandle_t ulOverlayHandle, TrackedDeviceIndex_t unTrackedDevice, const HmdMatrix34_t* pmatTrackedDeviceToOverlayTransform)
{
	USEH();

	overlay->transformType = VROverlayTransform_TrackedDeviceRelative;
	overlay->transformData.deviceRelative.device = unTrackedDevice;
	overlay->transformData.deviceRelative.offset = *pmatTrackedDeviceToOverlayTransform;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayTransformTrackedDeviceRelative(VROverlayHandle_t ulOverlayHandle, TrackedDeviceIndex_t* punTrackedDevice, HmdMatrix34_t* pmatTrackedDeviceToOverlayTransform)
{
	STUBBED();
}
EVROverlayError BaseOverlay::SetOverlayTransformTrackedDeviceComponent(VROverlayHandle_t ulOverlayHandle, TrackedDeviceIndex_t unDeviceIndex, const char* pchComponentName)
{
	STUBBED();
}
EVROverlayError BaseOverlay::GetOverlayTransformTrackedDeviceComponent(VROverlayHandle_t ulOverlayHandle, TrackedDeviceIndex_t* punDeviceIndex, char* pchComponentName, uint32_t unComponentNameSize)
{
	STUBBED();
}
EVROverlayError BaseOverlay::GetOverlayTransformOverlayRelative(VROverlayHandle_t ulOverlayHandle, VROverlayHandle_t* ulOverlayHandleParent, HmdMatrix34_t* pmatParentOverlayToOverlayTransform)
{
	STUBBED();
}
EVROverlayError BaseOverlay::SetOverlayTransformOverlayRelative(VROverlayHandle_t ulOverlayHandle, VROverlayHandle_t ulOverlayHandleParent, const HmdMatrix34_t* pmatParentOverlayToOverlayTransform)
{
	// TODO
	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayTransformCursor(VROverlayHandle_t ulCursorOverlayHandle, const HmdVector2_t* pvHotspot)
{
	STUBBED();
}
EVROverlayError BaseOverlay::GetOverlayTransformCursor(VROverlayHandle_t ulOverlayHandle, HmdVector2_t* pvHotspot)
{
	STUBBED();
}
EVROverlayError BaseOverlay::SetOverlayTransformProjection(VROverlayHandle_t ulOverlayHandle,
    ETrackingUniverseOrigin eTrackingOrigin, const HmdMatrix34_t* pmatTrackingOriginToOverlayTransform,
    const OOVR_VROverlayProjection_t* pProjection, EVREye eEye)
{
	STUBBED();
}
EVROverlayError BaseOverlay::ShowOverlay(VROverlayHandle_t ulOverlayHandle)
{
	USEH();
	overlay->visible = true;
	return VROverlayError_None;
}
EVROverlayError BaseOverlay::HideOverlay(VROverlayHandle_t ulOverlayHandle)
{
	USEH();
	overlay->visible = false;
	return VROverlayError_None;
}
bool BaseOverlay::IsOverlayVisible(VROverlayHandle_t ulOverlayHandle)
{
	USEHB();
	return overlay->visible;
}
EVROverlayError BaseOverlay::GetTransformForOverlayCoordinates(VROverlayHandle_t ulOverlayHandle, ETrackingUniverseOrigin eTrackingOrigin, HmdVector2_t coordinatesInOverlay, HmdMatrix34_t* pmatTransform)
{
	STUBBED();
}
bool BaseOverlay::PollNextOverlayEvent(VROverlayHandle_t ulOverlayHandle, VREvent_t* pEvent, uint32_t eventSize)
{
	USEHB();

	memset(pEvent, 0, eventSize);

	std::lock_guard<std::mutex> lock(overlay->eventMutex);
	if (overlay->eventQueue.empty())
		return false;

	VREvent_t e = overlay->eventQueue.front();
	overlay->eventQueue.pop();

	memcpy(pEvent, &e, std::min((uint32_t)sizeof(e), eventSize));

	return true;
}
EVROverlayError BaseOverlay::GetOverlayInputMethod(VROverlayHandle_t ulOverlayHandle, VROverlayInputMethod* peInputMethod)
{
	USEH();

	if (peInputMethod)
		*peInputMethod = overlay->inputMethod;

	return VROverlayError_None;
}

EVROverlayError BaseOverlay::SetOverlayInputMethod(VROverlayHandle_t ulOverlayHandle, VROverlayInputMethod eInputMethod)
{
	USEH();

	overlay->inputMethod = eInputMethod;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlayMouseScale(VROverlayHandle_t ulOverlayHandle, HmdVector2_t* pvecMouseScale)
{
	USEH();

	*pvecMouseScale = overlay->mouseScale;

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayMouseScale(VROverlayHandle_t ulOverlayHandle, const HmdVector2_t* pvecMouseScale)
{
	USEH();

	if (pvecMouseScale)
		overlay->mouseScale = *pvecMouseScale;
	else
		overlay->mouseScale = HmdVector2_t{ 1.0f, 1.0f };

	return VROverlayError_None;
}
bool BaseOverlay::ComputeOverlayIntersection(VROverlayHandle_t ulOverlayHandle, const OOVR_VROverlayIntersectionParams_t* pParams, OOVR_VROverlayIntersectionResults_t* pResults)
{
	STUBBED();
}
bool BaseOverlay::HandleControllerOverlayInteractionAsMouse(VROverlayHandle_t ulOverlayHandle, TrackedDeviceIndex_t unControllerDeviceIndex)
{
	STUBBED();
}
bool BaseOverlay::IsHoverTargetOverlay(VROverlayHandle_t ulOverlayHandle)
{
	STUBBED();
}
VROverlayHandle_t BaseOverlay::GetGamepadFocusOverlay()
{
	STUBBED();
}
EVROverlayError BaseOverlay::SetGamepadFocusOverlay(VROverlayHandle_t ulNewFocusOverlay)
{
	STUBBED();
}
EVROverlayError BaseOverlay::SetOverlayNeighbor(EOverlayDirection eDirection, VROverlayHandle_t ulFrom, VROverlayHandle_t ulTo)
{
	STUBBED();
}
EVROverlayError BaseOverlay::MoveGamepadFocusToNeighbor(EOverlayDirection eDirection, VROverlayHandle_t ulFrom)
{
	STUBBED();
}
EVROverlayError BaseOverlay::SetOverlayDualAnalogTransform(VROverlayHandle_t ulOverlay, EDualAnalogWhich eWhich, const HmdVector2_t& vCenter, float fRadius)
{
	STUBBED();
}
EVROverlayError BaseOverlay::GetOverlayDualAnalogTransform(VROverlayHandle_t ulOverlay, EDualAnalogWhich eWhich, HmdVector2_t* pvCenter, float* pfRadius)
{
	STUBBED();
}
EVROverlayError BaseOverlay::SetOverlayDualAnalogTransform(VROverlayHandle_t ulOverlay, EDualAnalogWhich eWhich, const HmdVector2_t* pvCenter, float fRadius)
{
	STUBBED();
}
EVROverlayError BaseOverlay::TriggerLaserMouseHapticVibration(VROverlayHandle_t ulOverlayHandle, float fDurationSeconds, float fFrequency, float fAmplitude)
{
	STUBBED();
}
EVROverlayError BaseOverlay::SetOverlayCursor(VROverlayHandle_t ulOverlayHandle, VROverlayHandle_t ulCursorHandle)
{
	STUBBED();
}
EVROverlayError BaseOverlay::SetOverlayCursorPositionOverride(VROverlayHandle_t ulOverlayHandle, const HmdVector2_t* pvCursor)
{
	STUBBED();
}
EVROverlayError BaseOverlay::ClearOverlayCursorPositionOverride(VROverlayHandle_t ulOverlayHandle)
{
	STUBBED();
}
EVROverlayError BaseOverlay::SetOverlayTexture(VROverlayHandle_t ulOverlayHandle, const Texture_t* pTexture)
{
	USEH();
	overlay->texture = *pTexture;

	BackendManager::Instance().OnOverlayTexture(pTexture);

	if (!oovr_global_configuration.EnableLayers() || !BackendManager::Instance().IsGraphicsConfigured())
		return VROverlayError_None;

	if (!overlay->compositor) {
		overlay->compositor.reset(GetUnsafeBaseCompositor()->CreateCompositorAPI(pTexture));
	}

	overlay->compositor->LoadSubmitContext();
	auto revertToCallerContext = MakeScopeGuard([&]() {
		overlay->compositor->ResetSubmitContext();
	});

	overlay->compositor->Invoke(&overlay->texture, nullptr);

	overlay->layerQuad.space = xr_space_from_ref_space_type(GetUnsafeBaseSystem()->currentSpace);
	overlay->layerQuad.subImage = {
		overlay->compositor->GetSwapChain(),
		{ { 0, 0 },
		    { (int32_t)overlay->compositor->GetSrcSize().width,
		        (int32_t)overlay->compositor->GetSrcSize().height } },
		0
	};

	return VROverlayError_None;
}
EVROverlayError BaseOverlay::ClearOverlayTexture(VROverlayHandle_t ulOverlayHandle)
{
	USEH();
	overlay->texture = {};

	overlay->compositor.reset();
	return VROverlayError_None;
}
EVROverlayError BaseOverlay::SetOverlayRaw(VROverlayHandle_t ulOverlayHandle, void* pvBuffer, uint32_t unWidth, uint32_t unHeight, uint32_t unDepth)
{
	STUBBED();
}
EVROverlayError BaseOverlay::SetOverlayFromFile(VROverlayHandle_t ulOverlayHandle, const char* pchFilePath)
{
	STUBBED();
}
EVROverlayError BaseOverlay::GetOverlayTexture(VROverlayHandle_t ulOverlayHandle, void** pNativeTextureHandle, void* pNativeTextureRef, uint32_t* pWidth, uint32_t* pHeight, uint32_t* pNativeFormat, ETextureType* pAPIType, EColorSpace* pColorSpace, VRTextureBounds_t* pTextureBounds)
{
	STUBBED();
}
EVROverlayError BaseOverlay::ReleaseNativeOverlayHandle(VROverlayHandle_t ulOverlayHandle, void* pNativeTextureHandle)
{
	STUBBED();
}
EVROverlayError BaseOverlay::GetOverlayTextureSize(VROverlayHandle_t ulOverlayHandle, uint32_t* pWidth, uint32_t* pHeight)
{
	STUBBED();
}
EVROverlayError BaseOverlay::CreateDashboardOverlay(const char* pchOverlayKey, const char* pchOverlayFriendlyName, VROverlayHandle_t* pMainHandle, VROverlayHandle_t* pThumbnailHandle)
{
	STUBBED();
}
bool BaseOverlay::IsDashboardVisible()
{
	// TODO should this be based of whether Dash is open?
	// Probably, but handling focus opens some other issues as it triggers under other conditions.
	return false;
}
bool BaseOverlay::IsActiveDashboardOverlay(VROverlayHandle_t ulOverlayHandle)
{
	STUBBED();
}
EVROverlayError BaseOverlay::SetDashboardOverlaySceneProcess(VROverlayHandle_t ulOverlayHandle, uint32_t unProcessId)
{
	STUBBED();
}
EVROverlayError BaseOverlay::GetDashboardOverlaySceneProcess(VROverlayHandle_t ulOverlayHandle, uint32_t* punProcessId)
{
	STUBBED();
}
void BaseOverlay::ShowDashboard(const char* pchOverlayToShow)
{
	STUBBED();
}
TrackedDeviceIndex_t BaseOverlay::GetPrimaryDashboardDevice()
{
	STUBBED();
}
EVROverlayError BaseOverlay::ShowKeyboardWithDispatch(EGamepadTextInputMode eInputMode, EGamepadTextInputLineMode eLineInputMode,
    const char* pchDescription, uint32_t unCharMax, const char* pchExistingText, bool bUseMinimalMode, uint64_t uUserValue,
    VRKeyboard::eventDispatch_t eventDispatch)
{
#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
	if (!BaseCompositor::dxcomp) {
		// Game hasn't submitted a frame yet — can't create keyboard without D3D11 device.
		// Fall back to placeholder.
		SubmitPlaceholderKeyboardEvent(VREvent_KeyboardDone, eventDispatch, uUserValue);
		keyboardCache = "Adventurer";
		return VROverlayError_None;
	}

	if (eLineInputMode != k_EGamepadTextInputLineModeSingleLine)
		OOVR_ABORTF("Only single-line keyboard entry mode is currently supported (as opposed to ID=%d)", eLineInputMode);

	keyboard = make_unique<VRKeyboard>(BaseCompositor::dxcomp->GetDevice(), uUserValue, unCharMax, bUseMinimalMode, eventDispatch,
	    (VRKeyboard::EGamepadTextInputMode)eInputMode);

	keyboard->contents(VRKeyboard::CHAR_CONV.from_bytes(pchExistingText));
#else
	// No DX11 support — fall back to placeholder
	SubmitPlaceholderKeyboardEvent(VREvent_KeyboardDone, eventDispatch, uUserValue);
	keyboardCache = "Adventurer";
#endif

	return VROverlayError_None;
}

/** Placeholder method for submitting a KeyboardDone event when asked to show the keyboard since it is not implemented yet. **/
void BaseOverlay::SubmitPlaceholderKeyboardEvent(vr::EVREventType ev, VRKeyboard::eventDispatch_t eventDispatch, uint64_t userValue)
{
	VREvent_Keyboard_t data = { 0 };
	data.uUserValue = userValue;

	VREvent_t evt = { 0 };
	evt.eventType = ev;
	evt.trackedDeviceIndex = 0;
	evt.data.keyboard = data;

	eventDispatch(evt);
}

EVROverlayError BaseOverlay::ShowKeyboard(EGamepadTextInputMode eInputMode, EGamepadTextInputLineMode eLineInputMode,
    const char* pchDescription, uint32_t unCharMax, const char* pchExistingText, bool bUseMinimalMode, uint64_t uUserValue)
{

	VRKeyboard::eventDispatch_t dispatch = [](VREvent_t ev) {
		BaseSystem* sys = GetUnsafeBaseSystem();
		if (sys) {
			sys->_EnqueueEvent(ev);
		}
	};

	return ShowKeyboardWithDispatch(eInputMode, eLineInputMode, pchDescription, unCharMax, pchExistingText, bUseMinimalMode, uUserValue, dispatch);
}
EVROverlayError BaseOverlay::ShowKeyboard(EGamepadTextInputMode eInputMode, EGamepadTextInputLineMode eLineInputMode, uint32_t unFlags,
    const char* pchDescription, uint32_t unCharMax, const char* pchExistingText, uint64_t uUserValue)
{
	STUBBED();
}
EVROverlayError BaseOverlay::ShowKeyboardForOverlay(VROverlayHandle_t ulOverlayHandle,
    EGamepadTextInputMode eInputMode, EGamepadTextInputLineMode eLineInputMode,
    const char* pchDescription, uint32_t unCharMax, const char* pchExistingText,
    bool bUseMinimalMode, uint64_t uUserValue)
{

	USEH();

	VRKeyboard::eventDispatch_t dispatch = [overlay](VREvent_t ev) {
		std::lock_guard<std::mutex> lock(overlay->eventMutex);
		overlay->eventQueue.push(ev);
	};

	return ShowKeyboardWithDispatch(eInputMode, eLineInputMode, pchDescription, unCharMax, pchExistingText, bUseMinimalMode, uUserValue, dispatch);
}
EVROverlayError BaseOverlay::ShowKeyboardForOverlay(VROverlayHandle_t ulOverlayHandle, EGamepadTextInputMode eInputMode,
    EGamepadTextInputLineMode eLineInputMode, uint32_t unFlags, const char* pchDescription, uint32_t unCharMax,
    const char* pchExistingText, uint64_t uUserValue)
{
	STUBBED();
}
uint32_t BaseOverlay::GetKeyboardText(char* pchText, uint32_t cchText)
{
	string str = keyboard ? VRKeyboard::CHAR_CONV.to_bytes(keyboard->contents()) : keyboardCache;

	strncpy_s(pchText, cchText, str.c_str(), cchText);
	pchText[cchText - 1] = 0;

	return (uint32_t)strlen(pchText);
}
void BaseOverlay::HideKeyboard()
{
	// First, if the keyboard is currently open, cache its contents
	if (keyboard) {
		OOVR_LOGF("HideKeyboard: caching contents (%zu chars)", keyboard->contents().size());
		keyboardCache = VRKeyboard::CHAR_CONV.to_bytes(keyboard->contents());
	} else {
		OOVR_LOG("HideKeyboard: keyboard already null");
	}

	// Delete the keyboard instance
	keyboard.reset();
	OOVR_LOG("HideKeyboard: keyboard instance destroyed");
}
void BaseOverlay::SetKeyboardTransformAbsolute(ETrackingUniverseOrigin eTrackingOrigin, const HmdMatrix34_t* pmatTrackingOriginToKeyboardTransform)
{
	if (!keyboard)
		OOVR_ABORT("Cannot set keyboard position when the keyboard is closed!");

	BaseCompositor* compositor = GetUnsafeBaseCompositor();
	if (compositor && eTrackingOrigin != compositor->GetTrackingSpace()) {
		OOVR_ABORTF("Origin mismatch - current %d, requested %d", compositor->GetTrackingSpace(), eTrackingOrigin);
	}

	keyboard->SetTransform(*pmatTrackingOriginToKeyboardTransform);
}
void BaseOverlay::SetKeyboardPositionForOverlay(VROverlayHandle_t ulOverlayHandle, HmdRect2_t avoidRect)
{
	STUBBED();
}
EVROverlayError BaseOverlay::SetOverlayIntersectionMask(VROverlayHandle_t ulOverlayHandle, OOVR_VROverlayIntersectionMaskPrimitive_t* pMaskPrimitives, uint32_t unNumMaskPrimitives, uint32_t unPrimitiveSize)
{
	STUBBED();
}
EVROverlayError BaseOverlay::GetOverlayFlags(VROverlayHandle_t ulOverlayHandle, uint32_t* pFlags)
{
	STUBBED();
}
BaseOverlay::VRMessageOverlayResponse BaseOverlay::ShowMessageOverlay(const char* pchText, const char* pchCaption, const char* pchButton0Text, const char* pchButton1Text, const char* pchButton2Text, const char* pchButton3Text)
{
	STUBBED();
}
void BaseOverlay::CloseMessageOverlay()
{
	STUBBED();
}

EVROverlayError BaseOverlay::SetOverlayPreCurvePitch(vr::VROverlayHandle_t ulOverlayHandle, float fRadians)
{
	STUBBED();
}

EVROverlayError BaseOverlay::GetOverlayPreCurvePitch(vr::VROverlayHandle_t ulOverlayHandle, float* pfRadians)
{
	STUBBED();
}

EVROverlayError BaseOverlay::WaitFrameSync(uint32_t nTimeoutMs)
{
	STUBBED();
}
