#include "stdafx.h"
#define BASE_IMPL
#include "BaseCompositor.h"
#include "BaseOverlay.h"
// VR laser→menu system v2 (Scaleform-free — see banner in Submit()).
// TODO: extract all laser code into a separate file for proper separation
// of concerns — it's currently interleaved in BaseOverlay::Submit().
#include "../Misc/Keyboard/VRMenuLaser.h"
#include <memory>
#include "BaseSystem.h"
#include "Compositor/compositor.h"
#include "Drivers/Backend.h"
#include "Misc/Config.h"
#include "Misc/CableTracking.h"
#include "Misc/ScopeGuard.h"
#include "convert.h"
#include "generated/static_bases.gen.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <direct.h>
#include <map>
#include <string>
#include <sstream>
#include <vector>

using glm::mat4;
using glm::vec3;

using namespace vr;

// Implemented by the D3D11 compositor bridge; sourced from Skyrim's real
// MenuOpenCloseEvent rather than a guessed keyboard toggle.
extern int OCBridge_ConsoleState();

// Global menu-laser activity flag retained for renderer/status plumbing.
// Actual trigger ownership is per hand below.
// Per-hand flag: keyboard laser is hitting the keyboard quad for this hand.
// When true, BaseSystem::GetControllerState() masks the trigger so the game
// doesn't see it — only the keyboard processes it. Index 0=left, 1=right.
// The keyboard itself uses GetUnmaskedControllerState() so it still sees triggers.
bool g_kbLaserConsumesTrigger[2] = { false, false };
// Console world lasers own the trigger even when they are not over a flat UI
// surface. Skyrim's native console pointer projects onto an invisible screen
// and otherwise selects a different reference than the visible 3D ray.
bool g_consoleLaserConsumesTrigger[2] = { false, false };
// Flat-menu laser owns the trigger only for the hand currently hitting the
// menu quad. The SKSE bridge injects the corresponding Scaleform mouse click;
// letting Skyrim also see the physical trigger double-activates the focused
// inventory row when the laser click was intended for a top tab.
bool g_menuLaserConsumesTrigger[2] = { false, false };
// Once a trigger press is accepted by the menu laser, keep that physical press
// hidden from Skyrim until the controller is actually released. A menu can
// synchronously replace itself while the same pull is still down (RaceMenu
// finishing into Alternate/Paradigm start prompts); handing the held trigger
// back during that transition makes the newly opened prompt accept and close
// immediately. The laser continues reading the unmasked state for drags.
std::atomic<bool> g_menuLaserSuppressUntilRelease[2]{ false, false };

bool g_menuLaserActive = false; // True while either menu laser hits the quad

// [EXPERIMENTAL — DISABLED] Custom Windows message for laser→Scaleform injection
// static constexpr UINT WM_OC_LASER = WM_APP + 0x4F44;

// ── Shared memory struct for menu transform (written by SKSE plugin) ──
// v2 adds the laser cursor bridge: SKSE exports the game's real UI plane
// (uiNode) and MenuCursor feedback; we write laser UV hits + trigger edges
// back. MUST match the copy in OpenCompositeInput/src/Main.cpp exactly.
#ifdef _WIN32
#pragma pack(push, 1)
struct OCMenuTransform {
	static constexpr uint32_t MAGIC = 0x54434D4F; // 'OCMT'
	static constexpr uint32_t VERSION = 6;

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

	// v2: UI plane ground truth (SKSE -> DLL), app tracking space, Y-up meters.
	// Quad convention matches VRMenuLaser: +X right, +Y up, +Z toward viewer.
	uint8_t  uiPlaneValid;
	float    uiPlanePos[3];
	float    uiPlaneQuat[4];
	float    uiPlaneWidth;
	float    uiPlaneHeight;

	// v2: cursor feedback (SKSE -> DLL)
	float    cursorPosX;
	float    cursorPosY;
	float    cursorRangeX;
	float    cursorRangeY;

	// v2: laser command (DLL -> SKSE). We are the only writer of these.
	// u,v in [0,1], Scaleform convention: (0,0) = top-left (VRMenuLaser
	// GetHitV already returns top-down V).
	uint8_t  laserActive;
	float    laserU;
	float    laserV;
	uint32_t laserPressSeq;
	uint32_t laserReleaseSeq;
	uint32_t laserFrameSeq;
	uint8_t  laserShowCursor; // 1 = SKSE may show the 2D arrow (diagnostic only;
	                          // default 0 — the laser dot IS the pointer)

	// v3: Skyrim HMD pose in the same RoomNode-local metric frame as uiPlane.
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

// Dedicated bidirectional bridge for console world-space picking. Keep this
// separate from OCMenuTransform: normal menu calibration is locked and has a
// different lifetime/ownership model. Runtime fields are written only by OC;
// hit fields are written only by the SKSE game-thread raycaster.
#pragma pack(push, 1)
struct OCConsoleLaserBridge {
	static constexpr uint32_t MAGIC = 0x524C434F; // 'OCLR'
	static constexpr uint32_t VERSION = 1;

	uint32_t magic;
	uint32_t version;
	uint32_t byteSize;

	uint32_t runtimeSequence;          // OC writer: odd while writing
	uint32_t frameSequence;
	uint32_t triggerPressSequence[2];
	uint8_t  rayValid[2];
	uint8_t  runtimePad[2];
	float    rayOriginFromHmd[2][3];   // OpenXR app-space delta, meters
	float    rayDirection[2][3];       // OpenXR app-space unit direction

	uint32_t gameSequence;             // SKSE writer: odd while writing
	uint32_t hitFormId[2];
	uint8_t  hitValid[2];              // Physics hit, including static terrain
	uint8_t  gamePad[2];
	float    hitDistanceMeters[2];
	uint32_t selectedFormId;
	uint8_t  reserved[12];
};
#pragma pack(pop)
static_assert(offsetof(OCConsoleLaserBridge, runtimeSequence) % 4 == 0);
static_assert(offsetof(OCConsoleLaserBridge, gameSequence) % 4 == 0);
static_assert(sizeof(OCConsoleLaserBridge) == 120);

static HANDLE           s_hMapFile = nullptr;
static OCMenuTransform* s_pTransform = nullptr;
static bool             s_sharedMemTried = false;
static HANDLE                s_hConsoleLaserMap = nullptr;
static OCConsoleLaserBridge* s_pConsoleLaser = nullptr;
static bool                  s_consoleLaserMapTried = false;

// Called from DLL_PROCESS_DETACH to release shared memory mapping
void CleanupOverlaySharedMemory()
{
	if (s_pTransform) {
		UnmapViewOfFile(s_pTransform);
		s_pTransform = nullptr;
	}
	if (s_hMapFile) {
		CloseHandle(s_hMapFile);
		s_hMapFile = nullptr;
	}
	if (s_pConsoleLaser) {
		UnmapViewOfFile(s_pConsoleLaser);
		s_pConsoleLaser = nullptr;
	}
	if (s_hConsoleLaserMap) {
		CloseHandle(s_hConsoleLaserMap);
		s_hConsoleLaserMap = nullptr;
	}
	s_sharedMemTried = false;
	s_consoleLaserMapTried = false;
}

static void OpenSharedMemory()
{
	if (s_pTransform) // Already connected
		return;

	// Retry every ~2 seconds (assuming ~90fps, every 180 frames)
	static int retryCounter = 0;
	if (s_sharedMemTried && (++retryCounter % 180) != 0)
		return;
	s_sharedMemTried = true;

	// Read+write: v2 writes laser UV hits and trigger edges back to SKSE
	s_hMapFile = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, L"Local\\OpenCompositeMenuTransform");
	if (!s_hMapFile)
		return; // SKSE plugin hasn't created it yet, retry later

	s_pTransform = static_cast<OCMenuTransform*>(
	    MapViewOfFile(s_hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(OCMenuTransform)));
	if (!s_pTransform) {
		CloseHandle(s_hMapFile);
		s_hMapFile = nullptr;
		return;
	}

	OOVR_LOG("Shared memory connected to SKSE plugin");
}

static void OpenConsoleLaserBridge()
{
	if (s_pConsoleLaser)
		return;

	static int retryCounter = 0;
	if (s_consoleLaserMapTried && (++retryCounter % 180) != 0)
		return;
	s_consoleLaserMapTried = true;

	s_hConsoleLaserMap = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE,
	    L"Local\\OpenCompositeConsoleLaser");
	if (!s_hConsoleLaserMap)
		return;

	s_pConsoleLaser = static_cast<OCConsoleLaserBridge*>(
	    MapViewOfFile(s_hConsoleLaserMap, FILE_MAP_ALL_ACCESS, 0, 0,
	        sizeof(OCConsoleLaserBridge)));
	if (!s_pConsoleLaser) {
		CloseHandle(s_hConsoleLaserMap);
		s_hConsoleLaserMap = nullptr;
		return;
	}

	if (s_pConsoleLaser->magic != OCConsoleLaserBridge::MAGIC ||
	    s_pConsoleLaser->version != OCConsoleLaserBridge::VERSION ||
	    s_pConsoleLaser->byteSize < sizeof(OCConsoleLaserBridge)) {
		OOVR_LOG("Console laser bridge rejected: incompatible SKSE plugin");
		UnmapViewOfFile(s_pConsoleLaser);
		s_pConsoleLaser = nullptr;
		CloseHandle(s_hConsoleLaserMap);
		s_hConsoleLaserMap = nullptr;
		return;
	}

	OOVR_LOG("Console laser bridge connected to SKSE plugin");
}

static bool ReadConsoleLaserHits(bool hitValid[2], float hitDistance[2], uint32_t hitFormId[2])
{
	if (!s_pConsoleLaser)
		return false;
	for (int attempt = 0; attempt < 3; ++attempt) {
		uint32_t seq1 = s_pConsoleLaser->gameSequence;
		if (seq1 & 1)
			continue;
		MemoryBarrier();
		for (int side = 0; side < 2; ++side) {
			hitValid[side] = s_pConsoleLaser->hitValid[side] != 0;
			hitDistance[side] = s_pConsoleLaser->hitDistanceMeters[side];
			hitFormId[side] = s_pConsoleLaser->hitFormId[side];
		}
		MemoryBarrier();
		uint32_t seq2 = s_pConsoleLaser->gameSequence;
		if (seq1 == seq2)
			return true;
	}
	return false;
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
		if (seq1 == seq2 && out.magic == OCMenuTransform::MAGIC &&
		    out.version == OCMenuTransform::VERSION)
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
// Per-menu quad profiles, hand-calibrated in-headset. v2 uses these only as
// a FALLBACK when the SKSE plugin isn't exporting the game's real uiNode
// plane (uiPlaneValid=0) — the ground-truth plane needs no per-menu tuning.
struct MenuQuadProfile {
	const char* menuName;
	float distance, widthScale, heightScale, yOffset, xOffset;
	int opacity;
};
static constexpr MenuQuadProfile kDefaultProfile = { "(default)", 0.85f, 0.80f, 0.42f, -0.13f, 0.00f, 20 };
static constexpr MenuQuadProfile kMenuProfiles[] = {
	{ "Journal Menu", 0.86f, 1.16f, 0.75f, -0.14f, 0.00f, 20 },
	{ "TweenMenu", 0.86f, 0.45f, 0.42f, -0.17f, 0.00f, 20 },
	{ "InventoryMenu", 0.86f, 0.73f, 0.89f, -0.14f, -0.42f, 20 },
	{ "MagicMenu", 0.86f, 0.73f, 0.89f, -0.14f, -0.42f, 20 },
	{ "FavoritesMenu", 0.86f, 0.43f, 0.58f, -0.23f, -0.28f, 20 },
	{ "CustomMenu", 0.86f, 1.17f, 0.64f, -0.11f, 0.00f, 20 },
	{ "ContainerMenu", 0.86f, 0.73f, 0.89f, -0.14f, -0.42f, 20 },
	{ "BarterMenu", 0.86f, 0.73f, 0.89f, -0.14f, -0.42f, 20 },
	{ "GiftMenu", 0.86f, 0.73f, 0.89f, -0.14f, -0.42f, 20 },
};

// Look up the calibrated quad profile for a menu; falls back to the default.
static bool GetMenuProfile(const char* menuName, float& dist, float& w, float& h,
    float& yOff, float& xOff, int& opacity)
{
	for (const auto& p : kMenuProfiles) {
		if (strcmp(p.menuName, menuName) == 0) {
			dist = p.distance;
			w = p.widthScale;
			h = p.heightScale;
			yOff = p.yOffset;
			xOff = p.xOffset;
			opacity = p.opacity;
			return true;
		}
	}
	dist = kDefaultProfile.distance;
	w = kDefaultProfile.widthScale;
	h = kDefaultProfile.heightScale;
	yOff = kDefaultProfile.yOffset;
	xOff = kDefaultProfile.xOffset;
	opacity = kDefaultProfile.opacity;
	return false;
}

// The menu laser renderer — created when a tracked menu opens, destroyed on close
static std::unique_ptr<VRMenuLaser> menuLaser;
// Independent from the calibrated menu renderer by design.
static std::unique_ptr<VRMenuLaser> consoleLaser;
#endif // _WIN32

// Reloadable keyboard shortcut settings (updated by file watcher)
static bool s_shortcutEnabled = true;
static std::string s_shortcutButton = "left_stick";
static std::string s_shortcutMode = "double_tap";
static int s_shortcutTiming = 500;
static std::string s_shortcutTrackpad = "none"; // none | swipe_up | swipe_down (Index trackpad)

// Initialize shortcut settings from global config
static void InitShortcutSettings()
{
	s_shortcutEnabled = oovr_global_configuration.KbShortcutEnabled();
	s_shortcutButton = oovr_global_configuration.KbShortcutButton();
	s_shortcutMode = oovr_global_configuration.KbShortcutMode();
	s_shortcutTiming = oovr_global_configuration.KbShortcutTiming();
	s_shortcutTrackpad = oovr_global_configuration.KbShortcutTrackpad();
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
		if (sscanf(line, "shortcutTrackpad=%255s", sval) == 1)
			s_shortcutTrackpad = sval;
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
	std::string mode;  // "press", "double_tap", "triple_tap", "quadruple_tap", "long_press", "hold"
	int timingMs;
	int scancode;

	// Per-combo detection state
	int tapCount;
	ULONGLONG tapTimes[4];
	bool wasAllPressed;
	ULONGLONG holdStart;
	bool firedThisPress;
	ULONGLONG pendingUpAt; // scheduled key-up for tap-style fires (0 = none)

	ComboBinding() : timingMs(500), scancode(0), tapCount(0),
		wasAllPressed(false), holdStart(0), firedThisPress(false), pendingUpAt(0) {
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

// Defined in the gesture section below
static HWND GetGameWindowLocal();

// Combo key delivery — same path the gesture system settled on after field
// testing: down/up through the SKSE plugin (WM_OC_KB wParam 4=down / 5=up ->
// BSInputEventQueue keyboard events), which reaches SKSE mod hotkey listeners
// (SkyrimNet, MCM hotkeys, ...) regardless of window focus. Raw SendInput is
// only the no-plugin fallback — injected scancodes never reach SKSE listeners
// when the game window is unfocused, which it usually is in VR.
static void SendComboKey(int scancode, bool up)
{
	if (ocu_cable::Shortcut(scancode, up)) return;
	constexpr UINT WM_OC_KB_COMBO = WM_APP + 0x4F43; // same channel as keyboard/gestures
	HWND hwnd = GetGameWindowLocal();
	if (hwnd) {
		PostMessageW(hwnd, WM_OC_KB_COMBO, up ? 5 : 4, (LPARAM)scancode);
	} else {
		INPUT in = {};
		in.type = INPUT_KEYBOARD;
		in.ki.wScan = (WORD)scancode;
		in.ki.dwFlags = KEYEVENTF_SCANCODE | (up ? KEYEVENTF_KEYUP : 0);
		SendInput(1, &in, sizeof(INPUT));
	}
}

// Tap-style fire: key down now, key up ~60ms later (SKSE listeners need the
// key visibly held across at least one frame to register it).
static void FireComboTap(ComboBinding& combo)
{
	SendComboKey(combo.scancode, false);
	combo.pendingUpAt = GetTickCount64() + 60;
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

		// Deliver a scheduled key-up from an earlier tap-style fire
		if (combo.pendingUpAt && GetTickCount64() >= combo.pendingUpAt) {
			SendComboKey(combo.scancode, true);
			combo.pendingUpAt = 0;
		}

		if (combo.mode == "press") {
			// Modifier combo: fire once when all held, reset when released
			if (allPressed && !combo.firedThisPress) {
				FireComboTap(combo);
				combo.firedThisPress = true;
				OOVR_DEBUG_LOGF("Combo fired (press): scancode 0x%02x", combo.scancode);
			}
			if (!allPressed)
				combo.firedThisPress = false;
		} else if (combo.mode == "hold") {
			// Push-to-talk style: the key goes DOWN when the combo is pressed
			// and stays down until the combo is released (SkyrimNet PTT etc.)
			if (allPressed && !combo.firedThisPress) {
				SendComboKey(combo.scancode, false);
				combo.firedThisPress = true;
				OOVR_DEBUG_LOGF("Combo hold: scancode 0x%02x DOWN", combo.scancode);
			} else if (!allPressed && combo.firedThisPress) {
				SendComboKey(combo.scancode, true);
				combo.firedThisPress = false;
				OOVR_DEBUG_LOGF("Combo hold: scancode 0x%02x UP", combo.scancode);
			}
		} else if (combo.mode == "long_press") {
			if (allPressed) {
				if (combo.holdStart == 0)
					combo.holdStart = GetTickCount64();
				else if (!combo.firedThisPress &&
					(GetTickCount64() - combo.holdStart) >= (ULONGLONG)combo.timingMs) {
					FireComboTap(combo);
					combo.firedThisPress = true;
					OOVR_DEBUG_LOGF("Combo fired (long_press): scancode 0x%02x", combo.scancode);
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
					FireComboTap(combo);
					combo.tapCount = 0;
					OOVR_DEBUG_LOGF("Combo fired (%s): scancode 0x%02x", combo.mode.c_str(), combo.scancode);
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
	bool destroyQueued = false;
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
	uint32_t sortOrder = 0; // Higher values render on top of lower values
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

// Helper — find the main visible window for our process (used by Prisma text-input detection).
#ifdef _WIN32
static BOOL CALLBACK FindVisibleWindowForPID(HWND hwnd, LPARAM lParam) {
	DWORD pid;
	GetWindowThreadProcessId(hwnd, &pid);
	if (pid == GetCurrentProcessId() && IsWindowVisible(hwnd)) {
		*reinterpret_cast<HWND*>(lParam) = hwnd;
		return FALSE;
	}
	return TRUE;
}

static HWND GetGameWindowLocal() {
	static HWND cached = nullptr;
	if (cached && IsWindow(cached))
		return cached;
	HWND found = nullptr;
	EnumWindows(FindVisibleWindowForPID, reinterpret_cast<LPARAM>(&found));
	cached = found;
	return cached;
}
#endif

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

// ============================================================================
// GESTURE RECOGNIZER
// Fires a keyboard scancode when a drawn controller motion matches a gesture
// saved by the configurator (<game>\Gestures\*.json). Capture is gated on the
// gesture's hold button; matching runs when the button releases. Templates
// and live paths are resampled to 64 points, centered, and uniformly scaled,
// then compared point-to-point ($1-recognizer style, no rotation invariance:
// a Z and an N must stay different).
// ============================================================================
#include <algorithm>
#include <fstream>
#include <json/json.h>
#include <mmsystem.h>

// Trackpad state exported from BaseInput (updated by GetControllerState)
extern float g_ocuTrackpadY[2];
extern bool g_ocuTrackpadTouch[2];
extern bool g_ocuTrackpadClick[2];

namespace gestures {

struct Pt {
	float x, y;
};

struct GestureDef {
	std::string name;
	std::string hold; // l_trigger, r_stick, r_trackpad, ...
	int scancode = 0;
	int vk = 0;
	int action = 0; // 0 = press key, 1 = cast spell instantly, 2 = equip left hand, 3 = equip right hand
	std::string spellPlugin;   // source plugin file (load-order independent)
	uint32_t spellFormId = 0;  // LOCAL form id within that plugin
	bool concentration = false; // spell streams while held (Flames-style) — channels instead of one-shot
	float trailRGB[3] = { 0.2f, 0.78f, 1.0f };
	float runeRGB[3] = { 0.2f, 0.78f, 1.0f }; // pulse-flash color once the shape completes (defaults to trail color)
	float trailWidth = 1.0f; // stroke thickness multiplier (0.6 thin … 2.2 massive)
	uint8_t trailStyle = 8; // bit flags: 1 transparent, 2 smoky, 4 wispy, 8 glowing
	std::vector<Pt> hand[2]; // concatenated template strokes per hand (0=L, 1=R)
};

// Handed by pointer to the SKSE plugin (same process). Layout must match
// OCGestureCastRequest in the plugin's Main.cpp.
struct OCGestureCastRequest {
	char plugin[128];
	uint32_t formId;
	int mode; // 0 = cast instantly, 1 = equip left, 2 = equip right, 3 = start held stream, 4 = stop held stream
	int hand; // casting hand: 0 = left, 1 = right (the hand that drew the gesture)
};

static void TrailColorFromName(const std::string& name, float rgb[3])
{
	struct NamedColor { const char* n; float r, g, b; };
	static const NamedColor kColors[] = {
		{ "cyan", 0.20f, 0.78f, 1.00f }, { "blue", 0.25f, 0.42f, 1.00f },
		{ "purple", 0.62f, 0.32f, 1.00f }, { "green", 0.30f, 1.00f, 0.42f },
		{ "orange", 1.00f, 0.55f, 0.15f }, { "red", 1.00f, 0.25f, 0.20f },
		{ "pink", 1.00f, 0.42f, 0.75f }, { "white", 1.00f, 1.00f, 1.00f },
		{ "gold", 1.00f, 0.84f, 0.30f }, { "black", 0.06f, 0.05f, 0.10f },
	};
	for (const auto& c : kColors) {
		if (name == c.n) {
			rgb[0] = c.r;
			rgb[1] = c.g;
			rgb[2] = c.b;
			return;
		}
	}
	rgb[0] = 0.20f;
	rgb[1] = 0.78f;
	rgb[2] = 1.00f;
}

static std::vector<GestureDef> s_defs;
static ULONGLONG s_nextScan = 0;
static ULONGLONG s_dirStamp = 0; // combined file count + newest write time
static ULONGLONG s_cooldownUntil = 0;

static std::wstring DirPath()
{
	wchar_t exePath[MAX_PATH];
	GetModuleFileNameW(nullptr, exePath, MAX_PATH);
	std::wstring p(exePath);
	size_t pos = p.find_last_of(L"\\/");
	if (pos != std::wstring::npos)
		p = p.substr(0, pos + 1);
	return p + L"Gestures";
}

static void ParseStrokes(const Json::Value& arr, std::vector<Pt>& out)
{
	if (!arr.isArray())
		return;
	for (const auto& stroke : arr) {
		if (!stroke.isArray())
			continue;
		for (const auto& p : stroke) {
			if (p.isArray() && p.size() >= 2)
				out.push_back({ p[0].asFloat(), p[1].asFloat() });
		}
	}
}

static void LoadGestures()
{
	s_defs.clear();
	std::wstring dir = DirPath();
	WIN32_FIND_DATAW fd;
	HANDLE find = FindFirstFileW((dir + L"\\*.json").c_str(), &fd);
	if (find == INVALID_HANDLE_VALUE)
		return;

	do {
		std::wstring path = dir + L"\\" + fd.cFileName;
		std::ifstream file(path);
		if (!file.is_open())
			continue;

		Json::Value root;
		Json::CharReaderBuilder builder;
		std::string errs;
		if (!Json::parseFromStream(builder, file, &root, &errs))
			continue;

		GestureDef def;
		def.name = root.get("Name", "").asString();
		def.hold = root.get("HoldButton", "").asString();
		def.scancode = root.get("Scancode", 0).asInt();
		def.vk = root.get("Vk", 0).asInt();
		std::string action = root.get("Action", "key").asString();
		def.action = action == "spell" ? 1 : action == "equip_left" ? 2 : action == "equip_right" ? 3 : 0;
		def.spellPlugin = root.get("SpellPlugin", "").asString();
		std::string formStr = root.get("SpellFormId", "").asString();
		if (!formStr.empty())
			def.spellFormId = (uint32_t)strtoul(formStr.c_str(), nullptr, 0); // handles 0x prefix
		if (def.action != 0 && (def.spellPlugin.empty() || def.spellFormId == 0)) {
			OOVR_LOGF("Gestures: '%s' is a spell gesture without a valid spell — treating as key", def.name.c_str());
			def.action = 0;
		}
		def.concentration = root.get("Concentration", false).asBool();
		def.trailWidth = root.get("TrailWidth", 1.0f).asFloat();
		if (def.trailWidth < 0.3f || def.trailWidth > 4.0f)
			def.trailWidth = 1.0f;
		TrailColorFromName(root.get("TrailColor", "cyan").asString(), def.trailRGB);
		std::string runeColor = root.get("RuneColor", "").asString();
		if (!runeColor.empty())
			TrailColorFromName(runeColor, def.runeRGB);
		else
			memcpy(def.runeRGB, def.trailRGB, sizeof(def.runeRGB));
		def.trailStyle = 0;
		if (root["TrailStyle"].isArray()) {
			for (const auto& s : root["TrailStyle"]) {
				std::string v = s.asString();
				if (v == "transparent") def.trailStyle |= 1;
				else if (v == "smoky") def.trailStyle |= 2;
				else if (v == "wispy") def.trailStyle |= 4;
				else if (v == "glowing") def.trailStyle |= 8;
			}
		}
		if (def.trailStyle == 0)
			def.trailStyle = 8;
		ParseStrokes(root["Left"], def.hand[0]);
		ParseStrokes(root["Right"], def.hand[1]);

		// Migrate pre-split hold ids (hand-agnostic) to the drawing hand
		if (!def.hold.empty() && def.hold[0] != 'l' && def.hold[0] != 'r') {
			bool leftOnly = !def.hand[0].empty() && def.hand[1].empty();
			def.hold = (leftOnly ? std::string("l_") : std::string("r_")) + def.hold;
		}

		// v1: hold-gated gestures only. Always-listening needs continuous
		// matching and a much stricter threshold; not worth the misfires yet.
		if (def.hold.empty()) {
			OOVR_LOGF("Gestures: '%s' has no hold button — skipped (not supported yet)", def.name.c_str());
			continue;
		}
		if (def.hand[0].size() < 2 && def.hand[1].size() < 2)
			continue;

		s_defs.push_back(std::move(def));
	} while (FindNextFileW(find, &fd));
	FindClose(find);

	OOVR_DEBUG_LOGF("Gestures: loaded %zu gesture(s) from Gestures folder", s_defs.size());
	for (const auto& d : s_defs)
		OOVR_LOGF("Gestures:   '%s' hold=%s action=%d key=0x%02X spell=%s/0x%X",
		    d.name.c_str(), d.hold.c_str(), d.action, d.scancode,
		    d.spellPlugin.c_str(), d.spellFormId);
}

// Cheap change detection: file count + newest write time, checked every 3s.
// Lets the configurator hot-add gestures while the game runs.
static void MaybeReload()
{
	ULONGLONG now = GetTickCount64();
	if (now < s_nextScan)
		return;
	s_nextScan = now + 3000;

	ULONGLONG stamp = 0;
	WIN32_FIND_DATAW fd;
	HANDLE find = FindFirstFileW((DirPath() + L"\\*.json").c_str(), &fd);
	if (find != INVALID_HANDLE_VALUE) {
		do {
			ULARGE_INTEGER t;
			t.LowPart = fd.ftLastWriteTime.dwLowDateTime;
			t.HighPart = fd.ftLastWriteTime.dwHighDateTime;
			stamp += t.QuadPart / 10000000ULL; // seconds granularity
			stamp += 1; // count contribution
		} while (FindNextFileW(find, &fd));
		FindClose(find);
	}

	if (stamp != s_dirStamp) {
		s_dirStamp = stamp;
		LoadGestures();
	}
}

static bool HoldHeld(const std::string& hold, VRControllerState_t st[2], const bool valid[2])
{
	if (hold.size() < 3)
		return false;
	int h = (hold[0] == 'l') ? 0 : 1;
	if (!valid[h])
		return false;
	const char* btn = hold.c_str() + 2;
	if (!strcmp(btn, "trigger"))
		return (st[h].ulButtonPressed & ButtonMaskFromId(k_EButton_Axis1)) != 0 || st[h].rAxis[1].x >= 0.5f;
	if (!strcmp(btn, "grip"))
		return (st[h].ulButtonPressed & ButtonMaskFromId(k_EButton_Grip)) != 0 || st[h].rAxis[2].x >= 0.5f;
	if (!strcmp(btn, "a"))
		return (st[h].ulButtonPressed & ButtonMaskFromId(k_EButton_A)) != 0;
	if (!strcmp(btn, "b"))
		return (st[h].ulButtonPressed & ButtonMaskFromId(k_EButton_ApplicationMenu)) != 0;
	if (!strcmp(btn, "stick"))
		return (st[h].ulButtonPressed & ButtonMaskFromId(k_EButton_SteamVR_Touchpad)) != 0;
	if (!strcmp(btn, "trackpad"))
		return g_ocuTrackpadClick[h];
	return false;
}

// Resample to N points by arc length, center on centroid, scale uniformly so
// the larger extent is 1. Output is comparable point-to-point.
static constexpr int kSamples = 64;

static bool NormalizePath(const std::vector<Pt>& in, Pt out[kSamples])
{
	if (in.size() < 2)
		return false;

	float total = 0;
	for (size_t i = 1; i < in.size(); i++)
		total += hypotf(in[i].x - in[i - 1].x, in[i].y - in[i - 1].y);
	if (total < 1e-5f)
		return false;

	float step = total / (kSamples - 1);
	out[0] = in[0];
	int outIdx = 1;
	float acc = 0;
	for (size_t i = 1; i < in.size() && outIdx < kSamples; i++) {
		Pt a = in[i - 1], b = in[i];
		float seg = hypotf(b.x - a.x, b.y - a.y);
		while (acc + seg >= step && outIdx < kSamples) {
			float t = (step - acc) / seg;
			Pt np = { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t };
			out[outIdx++] = np;
			seg -= (step - acc);
			a = np;
			acc = 0;
		}
		acc += seg;
	}
	while (outIdx < kSamples)
		out[outIdx++] = in.back();

	float cx = 0, cy = 0;
	for (int i = 0; i < kSamples; i++) {
		cx += out[i].x;
		cy += out[i].y;
	}
	cx /= kSamples;
	cy /= kSamples;

	float minX = 1e9f, maxX = -1e9f, minY = 1e9f, maxY = -1e9f;
	for (int i = 0; i < kSamples; i++) {
		out[i].x -= cx;
		out[i].y -= cy;
		minX = std::min(minX, out[i].x);
		maxX = std::max(maxX, out[i].x);
		minY = std::min(minY, out[i].y);
		maxY = std::max(maxY, out[i].y);
	}
	float extent = std::max(maxX - minX, maxY - minY);
	if (extent < 1e-5f)
		return false;
	for (int i = 0; i < kSamples; i++) {
		out[i].x /= extent;
		out[i].y /= extent;
	}
	return true;
}

// 0..1, higher is better. 0.707 = half-diagonal of the unit box.
static float MatchScore(const Pt a[kSamples], const Pt b[kSamples])
{
	float sum = 0;
	for (int i = 0; i < kSamples; i++)
		sum += hypotf(a[i].x - b[i].x, a[i].y - b[i].y);
	return 1.0f - (sum / kSamples) / 0.707f;
}

// Gesture key press with real duration. Preferred delivery: through the SKSE
// plugin into Skyrim's own input event queue (WM_OC_KB wParam 4 = down,
// 5 = up, lParam = scancode). SendInput proved unreliable here: injected
// keystrokes never reached SKSE hotkey listeners even with focus forced and
// a 60ms hold. SendInput remains only as a no-plugin fallback.
static constexpr UINT WM_OC_KB_GESTURE = WM_APP + 0x4F43;
static int s_pendingUpScancode = 0;
static ULONGLONG s_pendingUpAt = 0;
static bool s_pendingUpViaPlugin = false;

static void SendScancodeEvent(int scancode, bool up)
{
	INPUT in = {};
	in.type = INPUT_KEYBOARD;
	in.ki.wScan = (WORD)scancode;
	in.ki.dwFlags = KEYEVENTF_SCANCODE | (up ? KEYEVENTF_KEYUP : 0);
	SendInput(1, &in, sizeof(INPUT));
}

// Prisma direct delivery: panels only hear keys in their JS when the view has
// focus, so injected (and even physical) keys can be silently eaten. This
// export delivers straight to the panel regardless of focus. Same channel
// the VR keyboard uses; resolved lazily, null when PrismaUI is not loaded.
typedef void (*PrismaVR_DeliverVKeyFn)(int);
static PrismaVR_DeliverVKeyFn GetPrismaDeliverVKey()
{
	static PrismaVR_DeliverVKeyFn cached = nullptr;
	static bool resolved = false;
	if (!resolved) {
		resolved = true;
		if (HMODULE prisma = GetModuleHandleW(L"PrismaUI.dll"))
			cached = reinterpret_cast<PrismaVR_DeliverVKeyFn>(GetProcAddress(prisma, "PrismaVR_DeliverVKey"));
	}
	return cached;
}

static void FireGestureKey(int scancode, int vk)
{
	HWND hwnd = GetGameWindowLocal();
	if (hwnd) {
		PostMessageW(hwnd, WM_OC_KB_GESTURE, 4, (LPARAM)scancode);
		s_pendingUpViaPlugin = true;
	} else {
		SendScancodeEvent(scancode, false);
		s_pendingUpViaPlugin = false;
	}
	s_pendingUpScancode = scancode;
	s_pendingUpAt = GetTickCount64() + 60;

	// Prisma JS listeners get the key directly (game listeners never see this)
	if (vk) {
		if (auto deliver = GetPrismaDeliverVKey()) {
			deliver(vk);
			OOVR_DEBUG_LOGF("Gesture key: also delivered vk 0x%02X directly to Prisma", vk);
		}
	}
}

// ─── Cast sounds ─────────────────────────────────────────────────────────
// Magictrace.wav loops while the trail traces (starts once the hand actually
// moves, so arming alone stays silent); on release a successful cast plays
// the configured finish sound. Files live in the Gestures folder beside the
// gesture JSONs so Root Builder deploys them with everything else.
#pragma comment(lib, "winmm.lib")

static bool s_traceSoundOn = false;
static float s_capArc[2] = { 0, 0 };

static std::wstring GestureSoundPath(const wchar_t* file)
{
	return DirPath() + L"\\" + file;
}

static void StartTraceSound()
{
	if (s_traceSoundOn || !oovr_global_configuration.KbGestureSounds())
		return;
	PlaySoundW(GestureSoundPath(L"Magictrace.wav").c_str(), nullptr,
	    SND_FILENAME | SND_ASYNC | SND_LOOP | SND_NODEFAULT);
	s_traceSoundOn = true;
}

static void StopTraceSound(bool playFinish)
{
	if (s_traceSoundOn) {
		PlaySoundW(nullptr, nullptr, 0);
		s_traceSoundOn = false;
	}
	if (playFinish && oovr_global_configuration.KbGestureSounds()) {
		const wchar_t* file = (oovr_global_configuration.KbGestureFinishSound() == "dark")
		    ? L"Dark.wav"
		    : L"Impact.wav";
		PlaySoundW(GestureSoundPath(file).c_str(), nullptr,
		    SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
	}
}

// ─── Trail overlay state (rendering functions live below Update) ────────
static constexpr int kTrailTex = 512;
static constexpr float kTrailQuadSize = 1.6f; // meters; hand offsets map 1:1 onto it
static constexpr float kTrailDistance = 1.0f; // meters in front of the capture-time head

enum class TrailPhase { Idle,
	Drawing,
	Fade };
static TrailPhase s_trailPhase = TrailPhase::Idle;
static bool s_trailSuccess = false;
static ULONGLONG s_trailT0 = 0;
static float s_trailRGB[3] = { 0.2f, 0.78f, 1.0f };
static float s_trailWidth = 1.0f;
static uint8_t s_trailStyle = 8;
static std::vector<Pt> s_trailPath[2];
static XrPosef s_trailPose = {};
static XrSwapchain s_trailChain = XR_NULL_HANDLE;
static std::vector<XrSwapchainImageD3D11KHR> s_trailImages;
static XrCompositionLayerQuad s_trailLayer;
static uint32_t* s_trailPixels = nullptr;

static XrQuaternionf QuatFromHmdMatrix(const HmdMatrix34_t& m)
{
	XrQuaternionf q;
	float trace = m.m[0][0] + m.m[1][1] + m.m[2][2];
	if (trace > 0.0f) {
		float s = sqrtf(trace + 1.0f) * 2.0f;
		q.w = 0.25f * s;
		q.x = (m.m[2][1] - m.m[1][2]) / s;
		q.y = (m.m[0][2] - m.m[2][0]) / s;
		q.z = (m.m[1][0] - m.m[0][1]) / s;
	} else if (m.m[0][0] > m.m[1][1] && m.m[0][0] > m.m[2][2]) {
		float s = sqrtf(1.0f + m.m[0][0] - m.m[1][1] - m.m[2][2]) * 2.0f;
		q.w = (m.m[2][1] - m.m[1][2]) / s;
		q.x = 0.25f * s;
		q.y = (m.m[0][1] + m.m[1][0]) / s;
		q.z = (m.m[0][2] + m.m[2][0]) / s;
	} else if (m.m[1][1] > m.m[2][2]) {
		float s = sqrtf(1.0f + m.m[1][1] - m.m[0][0] - m.m[2][2]) * 2.0f;
		q.w = (m.m[0][2] - m.m[2][0]) / s;
		q.x = (m.m[0][1] + m.m[1][0]) / s;
		q.y = 0.25f * s;
		q.z = (m.m[1][2] + m.m[2][1]) / s;
	} else {
		float s = sqrtf(1.0f + m.m[2][2] - m.m[0][0] - m.m[1][1]) * 2.0f;
		q.w = (m.m[1][0] - m.m[0][1]) / s;
		q.x = (m.m[0][2] + m.m[2][0]) / s;
		q.y = (m.m[1][2] + m.m[2][1]) / s;
		q.z = 0.25f * s;
	}
	return q;
}

// Capture session: hand paths in the HMD-local plane captured at hold start.
// The frame is frozen at capture start so turning your head mid-gesture does
// not warp the drawing.
struct Capture {
	bool active = false;
	std::string hold;
	float origin[3];
	float right[3], up[3];
	std::vector<Pt> path[2];
	ULONGLONG start = 0;
};
static Capture s_cap;

// Stream state for concentration-spell gestures (Flames-style): releasing
// the hold secures the shape and starts a guaranteed 2s burst; holding the
// casting hand's trigger extends the stream for as long as it stays down
// ("continuance"). We must stop the caster explicitly — an unstopped
// concentration stream flows and drains magicka until empty.
static bool s_streaming = false;
static char s_streamPlugin[128] = {};
static uint32_t s_streamFormId = 0;
static int s_streamHand = 1;
static ULONGLONG s_streamStartedAt = 0;

// Score the captured drawing against the templates on this hold button.
// concentrationOnly restricts candidates to channel-capable spell gestures
// (the mid-hold early match); quiet suppresses the per-candidate log lines.
static const GestureDef* MatchCapture(const Capture& cap, const float arc[2], bool concentrationOnly, bool quiet, float& bestScore)
{
	Pt liveNorm[2][kSamples];
	bool liveOk[2] = { false, false };
	for (int h = 0; h < 2; h++)
		if (arc[h] > 0.12f && cap.path[h].size() >= 8)
			liveOk[h] = NormalizePath(cap.path[h], liveNorm[h]);

	bestScore = 0;
	if (!liveOk[0] && !liveOk[1])
		return nullptr;

	const GestureDef* best = nullptr;
	for (const auto& def : s_defs) {
		if (def.hold != cap.hold)
			continue;
		if (concentrationOnly && !(def.concentration && def.action == 1))
			continue;
		float score = 0;
		int hands = 0;
		bool ok = true;
		for (int h = 0; h < 2; h++) {
			if (def.hand[h].size() < 2)
				continue;
			Pt tmplNorm[kSamples];
			if (!liveOk[h] || !NormalizePath(def.hand[h], tmplNorm)) {
				ok = false;
				break;
			}
			score += MatchScore(liveNorm[h], tmplNorm);
			hands++;
		}
		if (!ok) {
			if (!quiet)
				OOVR_LOGF("Gesture: '%s' needs a hand you did not draw with — skipped", def.name.c_str());
			continue;
		}
		if (hands == 0)
			continue;
		score /= hands;
		if (!quiet)
			OOVR_DEBUG_LOGF("Gesture: candidate '%s' score %.2f", def.name.c_str(), score);
		if (score > bestScore) {
			bestScore = score;
			best = &def;
		}
	}
	return best;
}

static void GetPos(const HmdMatrix34_t& m, float out[3])
{
	out[0] = m.m[0][3];
	out[1] = m.m[1][3];
	out[2] = m.m[2][3];
}

static void Update(BaseSystem* sys, bool keyboardOpen)
{
	// Release a pending gesture keypress even if gestures get disabled mid-hold
	if (s_pendingUpScancode && GetTickCount64() >= s_pendingUpAt) {
		HWND hwnd = s_pendingUpViaPlugin ? GetGameWindowLocal() : nullptr;
		if (s_pendingUpViaPlugin && hwnd)
			PostMessageW(hwnd, WM_OC_KB_GESTURE, 5, (LPARAM)s_pendingUpScancode);
		else
			SendScancodeEvent(s_pendingUpScancode, true);
		s_pendingUpScancode = 0;
	}

	if (!oovr_global_configuration.KbGesturesEnabled())
		return;
	MaybeReload();
	if (s_defs.empty())
		return;

	ULONGLONG now = GetTickCount64();
	if (keyboardOpen || now < s_cooldownUntil) {
		// Gestures pause while the VR keyboard is open — say so, once per episode
		static bool s_loggedKbPause = false;
		if (keyboardOpen && !s_loggedKbPause) {
			s_loggedKbPause = true;
			OOVR_DEBUG_LOG("Gesture: paused while VR keyboard is open (close it to cast)");
		}
		if (!keyboardOpen)
			s_loggedKbPause = false;
		// A capture interrupted mid-draw fades its trail out quietly
		if (s_cap.active && s_trailPhase == TrailPhase::Drawing) {
			s_trailPhase = TrailPhase::Fade;
			s_trailSuccess = false;
			s_trailT0 = now;
			StopTraceSound(false);
		}
		s_cap.active = false;
		return;
	}

	// Controller states (also refreshes the exported trackpad values)
	VRControllerState_t st[2] = {};
	bool valid[2] = { false, false };
	TrackedDeviceIndex_t handIdx[2] = {
		sys->GetTrackedDeviceIndexForControllerRole(TrackedControllerRole_LeftHand),
		sys->GetTrackedDeviceIndexForControllerRole(TrackedControllerRole_RightHand)
	};
	for (int h = 0; h < 2; h++)
		if (handIdx[h] != k_unTrackedDeviceIndexInvalid)
			valid[h] = sys->GetControllerState(handIdx[h], &st[h], sizeof(st[h]));

	// Active stream: the engine keeps it flowing (and drains magicka) until
	// we stop it. Guaranteed 2s burst, then it survives only while the
	// casting hand's trigger is held — the same button that sustains a
	// normal cast. 30s safety cap in case a trigger reads stuck.
	if (s_streaming) {
		bool burstDone = now - s_streamStartedAt >= 2000;
		bool continuing = HoldHeld(s_streamHand == 0 ? "l_trigger" : "r_trigger", st, valid);
		bool timedOut = now - s_streamStartedAt > 30000;
		if (timedOut || (burstDone && !continuing)) {
			s_streaming = false;
			s_cooldownUntil = now + 600;
			HWND hwnd = GetGameWindowLocal();
			if (hwnd) {
				static OCGestureCastRequest s_stopRequest;
				memset(s_stopRequest.plugin, 0, sizeof(s_stopRequest.plugin));
				strncpy_s(s_stopRequest.plugin, s_streamPlugin, _TRUNCATE);
				s_stopRequest.formId = s_streamFormId;
				s_stopRequest.mode = 4; // stop stream
				s_stopRequest.hand = s_streamHand;
				PostMessageW(hwnd, WM_OC_KB_GESTURE, 6, (LPARAM)&s_stopRequest);
			}
			OOVR_DEBUG_LOGF("Gesture: stream ends (%s after %.1fs)",
			    timedOut ? "30s safety cap" : "burst done, trigger not held",
			    (now - s_streamStartedAt) / 1000.0f);
		}
		return;
	}

	// Rising-edge tracking for hold buttons: arming requires PRESSING the
	// button while a hand is already raised. A button that was already down
	// when the hand came up (grabbing something low and lifting it) must
	// never start a cast.
	static std::map<std::string, bool> s_holdWasDown;
	std::map<std::string, bool> holdPrev;
	std::swap(holdPrev, s_holdWasDown);
	for (const auto& def : s_defs)
		if (!s_holdWasDown.count(def.hold))
			s_holdWasDown[def.hold] = HoldHeld(def.hold, st, valid);

	if (!s_cap.active) {
		// Start a capture on a FRESH press of a referenced hold button while
		// a hand is raised high (above head + armHeight). The height gate is
		// what keeps grips during normal play from arming casts (and
		// spamming the trace sound).
		for (const auto& def : s_defs) {
			if (s_holdWasDown[def.hold] && !holdPrev[def.hold]) {
				TrackedDevicePose_t poses[k_unMaxTrackedDeviceCount];
				sys->GetDeviceToAbsoluteTrackingPose(TrackingUniverseStanding, 0, poses, k_unMaxTrackedDeviceCount);
				if (!poses[k_unTrackedDeviceIndex_Hmd].bPoseIsValid) {
					OOVR_LOG("Gesture: hold detected but HMD pose invalid — capture not started");
					return;
				}

				// Arm gate: EITHER hand raised high (default: forehead level)
				// arms the cast — drawing hand or holding hand, whichever the
				// player instinctively lifts. Blocked attempts log (throttled)
				// so a too-low hand is never an invisible failure.
				{
					float headY = poses[k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking.m[1][3];
					float armY = headY + oovr_global_configuration.KbGestureArmHeight();
					bool raised = false;
					float bestY = -100.0f;
					for (int h = 0; h < 2; h++) {
						if (handIdx[h] == k_unTrackedDeviceIndexInvalid || !poses[handIdx[h]].bPoseIsValid)
							continue;
						float hy = poses[handIdx[h]].mDeviceToAbsoluteTracking.m[1][3];
						bestY = std::max(bestY, hy);
						if (hy >= armY)
							raised = true;
					}
					if (!raised) {
						static ULONGLONG s_lastGateLog = 0;
						if (now - s_lastGateLog > 2000) {
							s_lastGateLog = now;
							OOVR_DEBUG_LOGF("Gesture: hold held but no hand raised (best %.2fm below arm height) — raise a hand to %.2fm above head to arm",
							    armY - bestY, oovr_global_configuration.KbGestureArmHeight());
						}
						continue; // hold held, but hands too low — stay disarmed
					}
				}

				const auto& hm = poses[k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking;
				s_cap = Capture{};
				s_cap.active = true;
				s_cap.hold = def.hold;
				s_cap.start = now;
				GetPos(hm, s_cap.origin);
				// HMD basis columns: 0 = right, 1 = up
				for (int r = 0; r < 3; r++) {
					s_cap.right[r] = hm.m[r][0];
					s_cap.up[r] = hm.m[r][1];
				}

				// Trail overlay: frozen quad at the capture plane, styled by
				// the (first) gesture on this hold button
				s_trailRGB[0] = def.trailRGB[0];
				s_trailRGB[1] = def.trailRGB[1];
				s_trailRGB[2] = def.trailRGB[2];
				s_trailWidth = def.trailWidth;
				s_trailStyle = def.trailStyle;
				s_trailPath[0].clear();
				s_trailPath[1].clear();
				s_trailPose.position = {
					s_cap.origin[0] - hm.m[0][2] * kTrailDistance,
					s_cap.origin[1] - hm.m[1][2] * kTrailDistance,
					s_cap.origin[2] - hm.m[2][2] * kTrailDistance
				};
				s_trailPose.orientation = QuatFromHmdMatrix(hm);
				s_trailPhase = TrailPhase::Drawing;
				s_trailSuccess = false;
				s_trailT0 = now;
				s_capArc[0] = s_capArc[1] = 0;

				OOVR_DEBUG_LOGF("Gesture: capture started (hold=%s, hand raised)", s_cap.hold.c_str());
				break;
			}
		}
		return;
	}

	// Active capture: bail out if it runs absurdly long
	if (now - s_cap.start > 10000) {
		OOVR_LOG("Gesture: capture timed out after 10s — cancelled");
		s_cap.active = false;
		s_trailPhase = TrailPhase::Fade;
		s_trailSuccess = false;
		s_trailT0 = now;
		StopTraceSound(false);
		return;
	}

	bool stillHeld = HoldHeld(s_cap.hold, st, valid);

	// Append current hand positions projected into the frozen HMD plane
	{
		TrackedDevicePose_t poses[k_unMaxTrackedDeviceCount];
		sys->GetDeviceToAbsoluteTrackingPose(TrackingUniverseStanding, 0, poses, k_unMaxTrackedDeviceCount);
		for (int h = 0; h < 2; h++) {
			if (handIdx[h] == k_unTrackedDeviceIndexInvalid || !poses[handIdx[h]].bPoseIsValid)
				continue;
			float p[3];
			GetPos(poses[handIdx[h]].mDeviceToAbsoluteTracking, p);
			float d[3] = { p[0] - s_cap.origin[0], p[1] - s_cap.origin[1], p[2] - s_cap.origin[2] };
			// Canvas convention: x right, y DOWN (matches the configurator)
			Pt pt = {
				d[0] * s_cap.right[0] + d[1] * s_cap.right[1] + d[2] * s_cap.right[2],
				-(d[0] * s_cap.up[0] + d[1] * s_cap.up[1] + d[2] * s_cap.up[2])
			};
			auto& path = s_cap.path[h];
			if (path.empty() || hypotf(pt.x - path.back().x, pt.y - path.back().y) > 0.005f) {
				if (!path.empty())
					s_capArc[h] += hypotf(pt.x - path.back().x, pt.y - path.back().y);
				path.push_back(pt);
				s_trailPath[h].push_back(pt); // live mirror for the trail overlay
			}
		}

		// Trace sound starts once the hand genuinely moves, not on arm alone
		if (!s_traceSoundOn && (s_capArc[0] + s_capArc[1]) > 0.05f)
			StartTraceSound();
	}

	if (stillHeld)
		return;

	// Hold released: match against gestures with this hold button
	Capture cap = std::move(s_cap);
	s_cap.active = false;

	// Per-hand arc length (deliberate motion filter: > 12cm)
	float arc[2] = { 0, 0 };
	for (int h = 0; h < 2; h++)
		for (size_t i = 1; i < cap.path[h].size(); i++)
			arc[h] += hypotf(cap.path[h][i].x - cap.path[h][i - 1].x, cap.path[h][i].y - cap.path[h][i - 1].y);

	OOVR_DEBUG_LOGF("Gesture: capture ended (hold=%s) — left %.2fm/%zu pts, right %.2fm/%zu pts",
	    cap.hold.c_str(), arc[0], cap.path[0].size(), arc[1], cap.path[1].size());

	if ((arc[0] <= 0.12f || cap.path[0].size() < 8) && (arc[1] <= 0.12f || cap.path[1].size() < 8)) {
		OOVR_DEBUG_LOG("Gesture: no deliberate drawing motion (need > 0.12m of hand travel) — ignored");
		s_trailPhase = TrailPhase::Fade;
		s_trailSuccess = false;
		s_trailT0 = now;
		StopTraceSound(false);
		return;
	}

	float bestScore = 0;
	const GestureDef* best = MatchCapture(cap, arc, false, false, bestScore);

	float threshold = oovr_global_configuration.KbGestureThreshold();
	bool cast = false;
	if (best && bestScore >= threshold) {
		if (best->action != 0) {
			// Spell action: hand the request to the SKSE plugin (same process)
			static OCGestureCastRequest s_castRequest;
			HWND hwnd = GetGameWindowLocal();
			if (hwnd) {
				memset(s_castRequest.plugin, 0, sizeof(s_castRequest.plugin));
				strncpy_s(s_castRequest.plugin, best->spellPlugin.c_str(), _TRUNCATE);
				s_castRequest.formId = best->spellFormId;
				s_castRequest.mode = best->action - 1;
				s_castRequest.hand = arc[0] >= arc[1] ? 0 : 1; // cast from the hand that drew
				if (best->action == 1 && best->concentration) {
					// Stream spell: start it (mode 3) and take over stop duty —
					// guaranteed 2s burst, extended while that hand's trigger
					// is held, stopped by the s_streaming block above.
					s_castRequest.mode = 3;
					memset(s_streamPlugin, 0, sizeof(s_streamPlugin));
					strncpy_s(s_streamPlugin, best->spellPlugin.c_str(), _TRUNCATE);
					s_streamFormId = best->spellFormId;
					s_streamHand = s_castRequest.hand;
					s_streamStartedAt = now;
					s_streaming = true;
				}
				PostMessageW(hwnd, WM_OC_KB_GESTURE, 6, (LPARAM)&s_castRequest);
				s_cooldownUntil = now + 600;
				cast = true;
				OOVR_LOGF("Gesture '%s' matched (score %.2f) — %s spell 0x%X from '%s'",
				    best->name.c_str(), bestScore,
				    best->action == 1 ? "casting" : "equipping",
				    best->spellFormId, best->spellPlugin.c_str());
			} else {
				OOVR_LOGF("Gesture '%s' matched but no game window for spell cast", best->name.c_str());
			}
		} else {
			int sc = best->scancode;
			if (!sc && best->vk)
				sc = (int)MapVirtualKeyW(best->vk, MAPVK_VK_TO_VSC);
			if (sc) {
				int vk = best->vk ? best->vk : (int)MapVirtualKeyW(sc, MAPVK_VSC_TO_VK);
				FireGestureKey(sc, vk);
				s_cooldownUntil = now + 600;
				cast = true;
				OOVR_LOGF("Gesture '%s' matched (score %.2f) — fired scancode 0x%02X (60ms hold)",
				    best->name.c_str(), bestScore, sc);
			} else {
				OOVR_LOGF("Gesture '%s' matched (score %.2f) but has no key bound", best->name.c_str(), bestScore);
			}
		}
	} else if (best) {
		OOVR_DEBUG_LOGF("Gesture: best candidate '%s' below threshold (%.2f < %.2f)",
		    best->name.c_str(), bestScore, threshold);
	}

	// Trail outcome: matched gesture breathes and dissolves, miss fades fast.
	// Use the matched gesture's own color/style for the success flourish.
	if (cast && best) {
		// The completed rune flashes in its own color (falls back to the trail color)
		s_trailRGB[0] = best->runeRGB[0];
		s_trailRGB[1] = best->runeRGB[1];
		s_trailRGB[2] = best->runeRGB[2];
		s_trailWidth = best->trailWidth;
		s_trailStyle = best->trailStyle;
	}
	s_trailPhase = TrailPhase::Fade;
	s_trailSuccess = cast;
	s_trailT0 = now;

	// Trace loop ends with the hold; a successful cast gets its finish sound
	StopTraceSound(cast);
}

// ─── Trail overlay rendering ─────────────────────────────────────────────
// Draws the gesture as a glowing stroke on a compositor quad frozen at the
// capture plane. Builds live under the hand; on success the finished shape
// breathes (two glow swells) and dissolves, on a miss it fades out quietly.

// Additive stamp with radial falloff into the RGBA8 buffer
static void TrailStamp(float cx, float cy, float radius, const float rgb[3], float alpha)
{
	int x0 = std::max(0, (int)(cx - radius));
	int x1 = std::min(kTrailTex - 1, (int)(cx + radius));
	int y0 = std::max(0, (int)(cy - radius));
	int y1 = std::min(kTrailTex - 1, (int)(cy + radius));
	float r2 = radius * radius;
	for (int y = y0; y <= y1; y++) {
		for (int x = x0; x <= x1; x++) {
			float dx = x - cx, dy = y - cy;
			float d2 = dx * dx + dy * dy;
			if (d2 > r2)
				continue;
			float fall = 1.0f - sqrtf(d2) / radius;
			float a = alpha * fall * fall;
			uint32_t& px = s_trailPixels[y * kTrailTex + x];
			uint8_t* c = (uint8_t*)&px;
			// RGBA8: additive, saturating
			c[0] = (uint8_t)std::min(255.0f, c[0] + rgb[0] * a * 255.0f);
			c[1] = (uint8_t)std::min(255.0f, c[1] + rgb[1] * a * 255.0f);
			c[2] = (uint8_t)std::min(255.0f, c[2] + rgb[2] * a * 255.0f);
			c[3] = (uint8_t)std::min(255.0f, c[3] + a * 255.0f);
		}
	}
}

static void RenderTrailTexture(float envelope)
{
	if (!s_trailPixels)
		s_trailPixels = new uint32_t[kTrailTex * kTrailTex];
	memset(s_trailPixels, 0, kTrailTex * kTrailTex * sizeof(uint32_t));

	float master = envelope;
	if (s_trailStyle & 1) // transparent
		master *= 0.5f;
	master = std::min(master, 1.9f); // headroom for the success pulse-flash
	if (master <= 0.0f)
		return;

	const bool glow = (s_trailStyle & 8) != 0;
	const bool smoky = (s_trailStyle & 2) != 0;
	const bool wispy = (s_trailStyle & 4) != 0;

	for (int h = 0; h < 2; h++) {
		const auto& path = s_trailPath[h];
		if (path.size() < 2)
			continue;

		int stamp = 0;
		for (size_t i = 1; i < path.size(); i++) {
			// Meters → texture pixels (quad center = capture origin)
			float ax = (path[i - 1].x / kTrailQuadSize + 0.5f) * kTrailTex;
			float ay = (path[i - 1].y / kTrailQuadSize + 0.5f) * kTrailTex;
			float bx = (path[i].x / kTrailQuadSize + 0.5f) * kTrailTex;
			float by = (path[i].y / kTrailQuadSize + 0.5f) * kTrailTex;
			// Per-gesture thickness multiplier; spacing scales with it so the
			// stamps keep the same overlap (CPU cost grows only linearly)
			const float w = s_trailWidth;
			float seg = hypotf(bx - ax, by - ay);
			int steps = std::max(1, (int)(seg / (4.0f * w)));
			for (int s = 0; s < steps; s++, stamp++) {
				float t = (float)s / steps;
				float px = ax + (bx - ax) * t;
				float py = ay + (by - ay) * t;

				if (glow) {
					TrailStamp(px, py, 26.0f * w, s_trailRGB, 0.05f * master);
					TrailStamp(px, py, 14.0f * w, s_trailRGB, 0.16f * master);
					TrailStamp(px, py, 6.0f * w, s_trailRGB, 0.75f * master);
				} else {
					TrailStamp(px, py, 8.0f * w, s_trailRGB, 0.8f * master);
				}
				if (smoky) {
					// Soft billow with deterministic jitter
					uint32_t hsh = (uint32_t)(stamp * 2654435761u);
					float jx = ((hsh & 0xFF) / 255.0f - 0.5f) * 18.0f * w;
					float jy = (((hsh >> 8) & 0xFF) / 255.0f - 0.5f) * 18.0f * w;
					TrailStamp(px + jx, py + jy, 26.0f * w, s_trailRGB, 0.030f * master);
				}
				if (wispy) {
					// Thin strands weaving around the stroke
					float wave = sinf(stamp * 0.35f) * 9.0f * w;
					float dx = by - ay, dy = -(bx - ax);
					float len = hypotf(dx, dy);
					if (len > 0.01f) {
						dx /= len;
						dy /= len;
						TrailStamp(px + dx * wave, py + dy * wave, 3.2f * w, s_trailRGB, 0.45f * master);
						TrailStamp(px - dx * wave * 0.6f, py - dy * wave * 0.6f, 2.4f * w, s_trailRGB, 0.30f * master);
					}
				}
			}
		}

		// Bright leading tip while drawing
		if (s_trailPhase == TrailPhase::Drawing) {
			float tx = (path.back().x / kTrailQuadSize + 0.5f) * kTrailTex;
			float ty = (path.back().y / kTrailQuadSize + 0.5f) * kTrailTex;
			const float white[3] = { 1.0f, 1.0f, 1.0f };
			TrailStamp(tx, ty, 9.0f * s_trailWidth, white, 0.9f * std::min(1.0f, master));
		}
	}
}

static bool EnsureTrailResources()
{
	if (s_trailChain != XR_NULL_HANDLE)
		return true;

	XrSwapchainCreateInfo sci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
	sci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
	sci.format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	sci.sampleCount = 1;
	sci.width = kTrailTex;
	sci.height = kTrailTex;
	sci.faceCount = 1;
	sci.arraySize = 1;
	sci.mipCount = 1;
	if (XR_FAILED(xrCreateSwapchain(xr_session.get(), &sci, &s_trailChain))) {
		s_trailChain = XR_NULL_HANDLE;
		return false;
	}

	uint32_t count = 0;
	xrEnumerateSwapchainImages(s_trailChain, 0, &count, nullptr);
	s_trailImages.resize(count, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
	xrEnumerateSwapchainImages(s_trailChain, count, &count, (XrSwapchainImageBaseHeader*)s_trailImages.data());

	memset(&s_trailLayer, 0, sizeof(s_trailLayer));
	s_trailLayer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
	s_trailLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
	s_trailLayer.space = xr_gbl->floorSpace;
	s_trailLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
	s_trailLayer.subImage.swapchain = s_trailChain;
	s_trailLayer.subImage.imageRect.offset = { 0, 0 };
	s_trailLayer.subImage.imageRect.extent = { kTrailTex, kTrailTex };
	s_trailLayer.subImage.imageArrayIndex = 0;
	s_trailLayer.size.width = kTrailQuadSize;
	s_trailLayer.size.height = kTrailQuadSize;
	OOVR_DEBUG_LOG("Gesture trail: overlay swapchain created");
	return true;
}

// Called from _BuildLayers: renders the trail (when active) and appends its
// quad to the layer list.
static void AppendTrailLayer(std::vector<XrCompositionLayerBaseHeader*>& headers)
{
	if (s_trailPhase == TrailPhase::Idle)
		return;

	ULONGLONG now = GetTickCount64();
	float envelope = 0.0f;

	if (s_trailPhase == TrailPhase::Drawing) {
		envelope = std::min(1.0f, (now - s_trailT0) / 150.0f); // quick fade-in
	} else { // Fade
		if (s_trailSuccess) {
			// Pulse-flash: two hard swells over 1.6s while dissolving, timed
			// to land with the finish sound ("woo... woo"). Each peak pushes
			// the additive stamps into saturation so the stroke flashes white.
			float t = (now - s_trailT0) / 1600.0f;
			if (t >= 1.0f) {
				s_trailPhase = TrailPhase::Idle;
				return;
			}
			float pulse = 1.0f + 0.9f * sinf(t * 6.2831853f * 2.0f);
			envelope = (1.0f - t * t) * pulse;
		} else {
			float t = (now - s_trailT0) / 280.0f;
			if (t >= 1.0f) {
				s_trailPhase = TrailPhase::Idle;
				return;
			}
			envelope = 1.0f - t;
		}
	}

	if (!BaseCompositor::dxcomp)
		return;
	ID3D11Device* dev = BaseCompositor::dxcomp->GetDevice();
	if (!dev || reinterpret_cast<uintptr_t>(dev) <= 0xFFFF)
		return;
	if (!EnsureTrailResources())
		return;

	RenderTrailTexture(envelope);

	// Upload: staging texture with init data, then copy into the acquired image
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = kTrailTex;
	td.Height = kTrailTex;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	td.SampleDesc = { 1, 0 };
	td.Usage = D3D11_USAGE_DEFAULT;
	D3D11_SUBRESOURCE_DATA init = { s_trailPixels, sizeof(uint32_t) * kTrailTex, 0 };
	ID3D11Texture2D* tex = nullptr;
	if (FAILED(dev->CreateTexture2D(&td, &init, &tex)))
		return;

	ID3D11DeviceContext* ctx = nullptr;
	dev->GetImmediateContext(&ctx);
	if (ctx) {
		XrSwapchainImageAcquireInfo acq = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
		uint32_t idx = 0;
		if (XR_SUCCEEDED(xrAcquireSwapchainImage(s_trailChain, &acq, &idx))) {
			XrSwapchainImageWaitInfo wait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
			wait.timeout = 500000000;
			if (XR_SUCCEEDED(xrWaitSwapchainImage(s_trailChain, &wait))) {
				if (idx < s_trailImages.size())
					ctx->CopyResource(s_trailImages[idx].texture, tex);
			}
			XrSwapchainImageReleaseInfo rel = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
			xrReleaseSwapchainImage(s_trailChain, &rel);

			s_trailLayer.pose = s_trailPose;
			headers.push_back((XrCompositionLayerBaseHeader*)&s_trailLayer);
		}
		ctx->Release();
	}
	tex->Release();
}

} // namespace gestures
#endif

int BaseOverlay::_BuildLayers(XrCompositionLayerBaseHeader* sceneLayer, XrCompositionLayerBaseHeader const* const*& layers)
{
	// Note that at least on MSVC, this shouldn't be doing any memory allocations
	//  unless the list is expanding from new layers.
	layerHeaders.clear();
	if (sceneLayer)
		layerHeaders.push_back(sceneLayer);

	// [KB-DIAG] Periodic device health check — only log once when corruption is detected
#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
	{
		static bool s_deviceCorruptionLogged = false;
		if (BaseCompositor::dxcomp && !s_deviceCorruptionLogged) {
			ID3D11Device* checkDev = BaseCompositor::dxcomp->GetDevice();
			if (reinterpret_cast<uintptr_t>(checkDev) <= 0xFFFF) {
				OOVR_LOGF("[KB-DIAG] *** DEVICE CORRUPTION DETECTED *** dxcomp=0x%llX GetDevice()=0x%llX frame=%llu",
				    (unsigned long long)(uintptr_t)BaseCompositor::dxcomp,
				    (unsigned long long)(uintptr_t)checkDev,
				    (unsigned long long)GetTickCount64());
				s_deviceCorruptionLogged = true;
			}
		}
	}

	// Skyrim, ENB, Prisma, and OCU share the game's immediate D3D11 context.
	// Do not call ClearState here: this runs every frame after scene submission and
	// can erase the pipeline state Skyrim later uses for its desktop companion draw.
	// OCU render paths must bind the state they need and restore any application
	// state they temporarily replace.
#endif

	// A client can destroy the keyboard's temporary owner overlay from its polling
	// worker as soon as it receives KeyboardDone. Perform that teardown before any
	// keyboard update, on this compositor thread, so its swapchain cannot be freed
	// concurrently with VRKeyboard::Update().
	ProcessPendingOverlayDestroys();

	// Game/SkyUI keyboard requests can arrive on arbitrary OpenVR client threads.
	// Consume them only here, on the compositor thread, after the frame's normal
	// D3D11 state handoff. This prevents concurrent immediate-context access in
	// nvwgf2umx.dll while Skyrim and Streamline are submitting work.
	ProcessPendingKeyboardRequest();

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
						OOVR_DEBUG_LOGF("Shortcut settings reloaded: enabled=%d button=%s mode=%s timing=%d",
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
			const std::string& modeName = s_shortcutMode;
			int timing = s_shortcutTiming;

			// Cached parsed button combo — only re-parse when s_shortcutButton changes
			struct BtnReq { int ctrl; uint64_t mask; };
			static std::vector<BtnReq> requirements;
			static std::string s_cachedBtnName;
			if (s_cachedBtnName != s_shortcutButton) {
				s_cachedBtnName = s_shortcutButton;
				requirements.clear();
				std::istringstream ss(s_shortcutButton);
				std::string token;
				while (std::getline(ss, token, '+')) {
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
					else if (token == "left_grip" || token == "right_grip" || token == "both_grips") { continue; }
					else if (token == "left_trigger" || token == "right_trigger") { continue; }
					if (bm != 0)
						requirements.push_back({ ci, bm });
				}
			}

			if (!requirements.empty() || s_shortcutTrackpad != "none") {
				// Get controller states using proper hand assignments (not hardcoded indices)
				VRControllerState_t ctrlState[2] = {};
				bool ctrlValid[2] = { false, false };
				TrackedDeviceIndex_t leftIdx = sys->GetTrackedDeviceIndexForControllerRole(TrackedControllerRole_LeftHand);
				TrackedDeviceIndex_t rightIdx = sys->GetTrackedDeviceIndexForControllerRole(TrackedControllerRole_RightHand);
				if (leftIdx != k_unTrackedDeviceIndexInvalid)
					ctrlValid[0] = sys->GetControllerState(leftIdx, &ctrlState[0], sizeof(ctrlState[0]));
				if (rightIdx != k_unTrackedDeviceIndexInvalid)
					ctrlValid[1] = sys->GetControllerState(rightIdx, &ctrlState[1], sizeof(ctrlState[1]));

				// Check ALL required buttons are pressed simultaneously.
				// Empty requirements (trackpad-only shortcut) must never count
				// as pressed or the tap detector would fire on its own.
				bool btnPressed = !requirements.empty();
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

				// Index trackpad swipe: thumb lands, slides most of the pad
				// height within the time window, either hand. GetControllerState
				// above refreshed the exported trackpad values this frame.
				// Fires once per touch so holding the thumb cannot re-trigger.
				if (!activate && s_shortcutTrackpad != "none") {
					extern float g_ocuTrackpadY[2];
					extern bool g_ocuTrackpadTouch[2];
					static bool swipeTouchWas[2] = { false, false };
					static bool swipeFired[2] = { false, false };
					static float swipeStartY[2] = { 0, 0 };
					static ULONGLONG swipeStartT[2] = { 0, 0 };
					const bool wantUp = (s_shortcutTrackpad == "swipe_up");
					for (int h = 0; h < 2; h++) {
						bool touch = g_ocuTrackpadTouch[h];
						float ty = g_ocuTrackpadY[h];
						if (touch && !swipeTouchWas[h]) {
							swipeStartY[h] = ty;
							swipeStartT[h] = GetTickCount64();
							swipeFired[h] = false;
						} else if (touch && !swipeFired[h]) {
							float dy = ty - swipeStartY[h];
							if ((ULONGLONG)(GetTickCount64() - swipeStartT[h]) <= 450) {
								if ((wantUp && dy >= 0.8f) || (!wantUp && dy <= -0.8f)) {
									activate = true;
									swipeFired[h] = true;
									OOVR_DEBUG_LOGF("Keyboard shortcut: trackpad swipe %s (hand=%d dy=%.2f)",
									    wantUp ? "up" : "down", h, dy);
								}
							}
						}
						swipeTouchWas[h] = touch;
					}
				}

				if (activate) {
					// Clear any dirty D3D11 pipeline state left by overlay rendering (PrismaUI etc.)
					if (BaseCompositor::dxcomp) {
						ID3D11Device* clearDev = BaseCompositor::dxcomp->GetDevice();
						if (clearDev && reinterpret_cast<uintptr_t>(clearDev) > 0xFFFF) {
							ID3D11DeviceContext* clearCtx = nullptr;
							clearDev->GetImmediateContext(&clearCtx);
							if (clearCtx) {
								clearCtx->ClearState();
								clearCtx->Flush();
								clearCtx->Release();
								OOVR_DEBUG_LOG("[KB-DIAG] ClearState()+Flush() before shortcut keyboard");
							}
						}
					}
					ID3D11Device* kbDev = BaseCompositor::dxcomp ? BaseCompositor::dxcomp->GetDevice() : nullptr;
					OOVR_DEBUG_LOGF("[KB-DIAG] shortcut: dxcomp=0x%llX GetDevice()=0x%llX",
					    (unsigned long long)(uintptr_t)BaseCompositor::dxcomp,
					    (unsigned long long)(uintptr_t)kbDev);
					if (kbDev && reinterpret_cast<uintptr_t>(kbDev) > 0xFFFF) {
						try {
							VRKeyboard::eventDispatch_t dispatch = [](VREvent_t ev) {
								BaseSystem* sys = GetUnsafeBaseSystem();
								if (sys) {
									sys->_EnqueueEvent(ev);
								}
							};
							keyboard = make_unique<VRKeyboard>(
							    kbDev, 0, 256, false, dispatch,
							    VRKeyboard::EGamepadTextInputMode::k_EGamepadTextInputModeNormal);
							keyboard->SetSendInputOnly(true);
						} catch (const std::exception& e) {
							OOVR_LOGF("Keyboard creation failed (shortcut): %s", e.what());
							keyboard.reset();
	keyboardOwner = nullptr;
						}
					} else {
						OOVR_LOG("Keyboard activation skipped - D3D device unavailable");
					}
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
				OOVR_DEBUG_LOGF("TextInput auto-detect: SkyrimVR.exe base = 0x%llX", (unsigned long long)gameBase);
			else
				OOVR_LOG("TextInput auto-detect: SkyrimVR.exe not found, disabled");
		}

		if (gameBase && BaseCompositor::dxcomp) {
			// Safe memory read helper (SEH can't be in functions with C++ destructors)
			int8_t textEntryCount = ReadTextEntryCount(gameBase);
			bool textInputActive = textEntryCount > 0;
			bool prismaTextInput = false;

			// Prisma UI VR bridge: check if a Prisma text input has focus.
			// PrismaVR sets this window property when document.activeElement
			// is an <input>/<textarea>/contentEditable in a Prisma HTML panel.
			if (!textInputActive) {
				HWND hwnd = GetGameWindowLocal();
				if (hwnd && GetPropW(hwnd, L"OC_PRISMA_TEXT")) {
					textInputActive = true;
					prismaTextInput = true;
				}
			}

			// Transition: text input just became active — auto-open keyboard
			if (textInputActive && !textInputWasActive && !keyboard) {
				OOVR_DEBUG_LOGF("TextInput auto-detect: textEntryCount=%d, opening VR keyboard", textEntryCount);
				// Clear any dirty D3D11 pipeline state left by overlay rendering (PrismaUI etc.)
				{
					ID3D11Device* clearDev2 = BaseCompositor::dxcomp->GetDevice();
					if (clearDev2 && reinterpret_cast<uintptr_t>(clearDev2) > 0xFFFF) {
						ID3D11DeviceContext* clearCtx2 = nullptr;
						clearDev2->GetImmediateContext(&clearCtx2);
						if (clearCtx2) {
							clearCtx2->ClearState();
							clearCtx2->Flush();
							clearCtx2->Release();
							OOVR_DEBUG_LOG("[KB-DIAG] ClearState()+Flush() before auto-detect keyboard");
						}
					}
				}
				VRKeyboard::eventDispatch_t dispatch = [](VREvent_t ev) {
					BaseSystem* sys = GetUnsafeBaseSystem();
					if (sys) {
						sys->_EnqueueEvent(ev);
					}
				};
				ID3D11Device* kbDev2 = BaseCompositor::dxcomp->GetDevice();
				OOVR_DEBUG_LOGF("[KB-DIAG] auto-detect: dxcomp=0x%llX GetDevice()=0x%llX",
				    (unsigned long long)(uintptr_t)BaseCompositor::dxcomp,
				    (unsigned long long)(uintptr_t)kbDev2);
				if (kbDev2 && reinterpret_cast<uintptr_t>(kbDev2) > 0xFFFF) {
					try {
						keyboard = make_unique<VRKeyboard>(
						    kbDev2, 0, 256, false, dispatch,
						    VRKeyboard::EGamepadTextInputMode::k_EGamepadTextInputModeNormal);
						// Prisma text inputs → VR MODE (no scancodes, no game hotkeys)
						// Game text inputs   → PC MODE (scancodes for DirectInput/MCM)
						keyboard->SetSendInputOnly(!prismaTextInput);
						autoOpenedKeyboard = true;
					} catch (const std::exception& e) {
						OOVR_LOGF("Keyboard creation failed (auto-detect): %s", e.what());
						keyboard.reset();
	keyboardOwner = nullptr;
					}
				} else {
					OOVR_LOG("Auto keyboard skipped - D3D device unavailable");
				}
			}

			// Transition: text input ended while we auto-opened — close keyboard
			if (!textInputActive && textInputWasActive && keyboard && autoOpenedKeyboard) {
				OOVR_DEBUG_LOG("TextInput auto-detect: text input ended, closing VR keyboard");
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

	// Gesture recognizer: capture hand motion while a gesture's hold button is
	// down, match and fire the bound key on release. Paused while typing.
	{
		BaseSystem* gsys = GetUnsafeBaseSystem();
		if (gsys)
			gestures::Update(gsys, keyboard != nullptr);
	}
#endif

	if (keyboard) {
		const auto& kbLayers = keyboard->Update();

		if (keyboard->IsClosed()) {
			OOVR_DEBUG_LOG("Keyboard closed, destroying before layer submission");
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

#ifdef _WIN32
	// Gesture trail overlay: renders while drawing and through the
	// breathe-and-dissolve after release
	gestures::AppendTrailLayer(layerHeaders);

	// Console world-space laser path. This is intentionally outside the menu
	// quad block below: the console's native cursor lives on an invisible flat
	// screen, while these rays remain valid throughout the full 3D scene.
	{
		static bool s_consoleWasOpen = false;
		const bool consoleOpen = OCBridge_ConsoleState() == 1 &&
		    oovr_global_configuration.MenuLaserEnabled();
		if (consoleOpen) {
			OpenConsoleLaserBridge();
			ID3D11Device* laserDev = BaseCompositor::dxcomp ?
			    BaseCompositor::dxcomp->GetDevice() : nullptr;
			if (!consoleLaser && laserDev && reinterpret_cast<uintptr_t>(laserDev) > 0xFFFF) {
				consoleLaser = std::make_unique<VRMenuLaser>(laserDev);
				consoleLaser->SetShowDebugQuad(false);
				OOVR_DEBUG_LOG("Console world lasers created (two-hand, Havok hit feedback)");
			}

			if (consoleLaser) {
				bool hitValid[2] = { false, false };
				float hitDistance[2] = { 0.0f, 0.0f };
				uint32_t hitFormId[2] = { 0, 0 };
				ReadConsoleLaserHits(hitValid, hitDistance, hitFormId);
				bool keyboardHit[2] = {
					g_kbLaserConsumesTrigger[0],
					g_kbLaserConsumesTrigger[1]
				};
				const auto& worldLayers = consoleLaser->UpdateWorld(
				    xr_gbl->nextPredictedFrameTime, keyboardHit,
				    hitValid, hitDistance);
				for (auto* layer : worldLayers)
					layerHeaders.push_back(layer);

				// Publish HMD-relative OpenXR rays. SKSE reconstructs them through
				// RoomNode into game-world coordinates, so stick rotation and
				// locomotion are both preserved without any flat-screen mapping.
				if (s_pConsoleLaser) {
					XrSpace appSpace = xr_space_from_ref_space_type(
					    GetUnsafeBaseSystem()->currentSpace);
					XrSpaceLocation headLoc = { XR_TYPE_SPACE_LOCATION };
					XrResult headResult = xrLocateSpace(xr_gbl->viewSpace, appSpace,
					    xr_gbl->nextPredictedFrameTime, &headLoc);
					bool headValid = XR_SUCCEEDED(headResult) &&
					    (headLoc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
					    (headLoc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);

					InterlockedIncrement(reinterpret_cast<volatile LONG*>(
					    &s_pConsoleLaser->runtimeSequence));
					MemoryBarrier();
					s_pConsoleLaser->frameSequence++;
					for (int side = 0; side < 2; ++side) {
						bool valid = headValid && consoleLaser->IsRayValid(side) &&
						    !keyboardHit[side];
						s_pConsoleLaser->rayValid[side] = valid ? 1 : 0;
						if (!valid)
							continue;
						XrVector3f origin = consoleLaser->GetRayOrigin(side);
						XrVector3f direction = consoleLaser->GetRayDir(side);
						s_pConsoleLaser->rayOriginFromHmd[side][0] = origin.x - headLoc.pose.position.x;
						s_pConsoleLaser->rayOriginFromHmd[side][1] = origin.y - headLoc.pose.position.y;
						s_pConsoleLaser->rayOriginFromHmd[side][2] = origin.z - headLoc.pose.position.z;
						s_pConsoleLaser->rayDirection[side][0] = direction.x;
						s_pConsoleLaser->rayDirection[side][1] = direction.y;
						s_pConsoleLaser->rayDirection[side][2] = direction.z;
						if (consoleLaser->IsTriggerPressed(side))
							s_pConsoleLaser->triggerPressSequence[side]++;
					}
					MemoryBarrier();
					InterlockedIncrement(reinterpret_cast<volatile LONG*>(
					    &s_pConsoleLaser->runtimeSequence));
				}
			}

			const bool ownsConsoleInput = consoleLaser && s_pConsoleLaser;
			g_consoleLaserConsumesTrigger[0] = ownsConsoleInput;
			g_consoleLaserConsumesTrigger[1] = ownsConsoleInput;
			if (!s_consoleWasOpen)
				OOVR_DEBUG_LOG("Console world-laser mode entered; native console trigger selection masked");
			s_consoleWasOpen = true;
		} else {
			g_consoleLaserConsumesTrigger[0] = false;
			g_consoleLaserConsumesTrigger[1] = false;
			if (s_pConsoleLaser && s_consoleWasOpen) {
				InterlockedIncrement(reinterpret_cast<volatile LONG*>(
				    &s_pConsoleLaser->runtimeSequence));
				MemoryBarrier();
				s_pConsoleLaser->rayValid[0] = 0;
				s_pConsoleLaser->rayValid[1] = 0;
				s_pConsoleLaser->frameSequence++;
				MemoryBarrier();
				InterlockedIncrement(reinterpret_cast<volatile LONG*>(
				    &s_pConsoleLaser->runtimeSequence));
			}
			if (consoleLaser) {
				consoleLaser.reset();
				OOVR_DEBUG_LOG("Console world lasers destroyed");
			}
			s_consoleWasOpen = false;
		}
	}
#endif

// =========================================================================
// MCM Menu Laser Pointer System v2 (Scaleform-free)
// =========================================================================
// VR laser pointer for Skyrim's flat menus. Re-enabled 2026-07-10 with the
// architecture that removes both original failure modes:
//
// 1. SOVNGARDE BUG (fixed by construction): v1 posted WM_OC_LASER to the
//    SKSE plugin which injected GFxMouseEvent/NotifyMouseState into a menu's
//    Scaleform movie from the WndProc thread. Out-of-band Scaleform calls
//    while StatsMenu's constellation scene owned the shared renderer state
//    permanently corrupted VR rendering. v2 never touches Scaleform on
//    either side: we write laser UV + trigger edges into shared memory and
//    the SKSE plugin drives the game's own MenuCursor through
//    BSInputEventQueue on the game thread.
//
// 2. EXPONENTIAL DRIFT (fixed by ground truth): v1 raycast against a
//    hand-calibrated head-anchored quad that never matched the plane the
//    game actually renders menus on; the mismatch grew toward the edges.
//    v2 raycasts against the game's own uiNode plane (exported by SKSE via
//    shared memory, uiPlaneValid). Head-anchored profiles remain only as a
//    fallback while the plane export is being validated.
//
// Runtime gate: enable_laser=1 in menu_quad_settings.ini (default OFF).
// The VR keyboard overlay (above this block) is COMPLETELY SEPARATE.
// =========================================================================
#if 1 // Menu laser system v2
	// ── MCM Menu Laser Pointer System ──
	// Menu quad parameters — overrideable via menu_quad_settings.ini
	// File is watched every ~1 second (same pattern as keyboard_settings.ini).
	static bool  s_mqEnableLaser = false; // master gate: enable_laser=1 in ini
	static bool  s_mqAlwaysShow = false; // calibration: quad stays up at ALL times (always_show_quad=1)
	// Live trim applied to the ADOPTED game plane (meters, hot-reload ~1s).
	// Locked in-headset calibration, 2026-07-28, after RoomNode-HMD to
	// OpenXR-Stage origin correction: 2.5mm down and 20mm behind the raw mesh.
	// plane_scale multiplies quad width+height.
	static float s_mqPlaneShiftDown = 0.0025f;
	static float s_mqPlaneShiftBack = 0.020f;
	static float s_mqPlaneShiftRight = 0.0f;
	static float s_mqPlaneScale = 1.0f;
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
	// Locked 2026-07-28 after full near/far/left/right in-headset validation.
	static float s_mqMouseOffsetX = -0.00333f; // fraction of screen width (positive = shift cursor right)
	static float s_mqMouseOffsetY = -0.0065f;  // fraction of screen height (positive = shift cursor down)
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
	// Per-menu-open diagnostic budgets: adoption/fallback state is re-logged on
	// every VRMenuLaser creation, not once per process (the one-shot logs made
	// the 2026-07-25 "small quad on reopen" session undiagnosable).
	static int   s_planeAdoptLogsLeft = 0;
	static int   s_planeFallbackLogsLeft = 0;

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
							if (sscanf(line, "enable_laser=%d", &iv) == 1) s_mqEnableLaser = (iv != 0);
							else if (sscanf(line, "always_show_quad=%d", &iv) == 1) s_mqAlwaysShow = (iv != 0);
							else if (sscanf(line, "plane_shift_down=%f", &fv) == 1) s_mqPlaneShiftDown = fv;
							else if (sscanf(line, "plane_shift_back=%f", &fv) == 1) s_mqPlaneShiftBack = fv;
							else if (sscanf(line, "plane_shift_right=%f", &fv) == 1) s_mqPlaneShiftRight = fv;
							else if (sscanf(line, "plane_scale=%f", &fv) == 1) s_mqPlaneScale = fv;
							else if (sscanf(line, "show_debug=%d", &iv) == 1) s_mqShowDebug = (iv != 0);
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
						OOVR_DEBUG_LOGF("Menu quad settings: dist=%.3f wScale=%.3f hScale=%.3f yOff=%.3f xOff=%.3f yawDeg=%.1f pitchDeg=%.1f rollDeg=%.1f opacity=%d debug=%d headLock=%d",
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
		bool menuSignalActive = false;
		static HWND cachedHwnd = nullptr;
		if (!cachedHwnd || !IsWindow(cachedHwnd)) {
			cachedHwnd = FindWindowW(L"Skyrim Special Edition", nullptr);
			if (!cachedHwnd)
				cachedHwnd = FindWindowW(nullptr, L"Skyrim VR");
		}
		if (cachedHwnd)
			menuSignalActive = (intptr_t)GetPropW(cachedHwnd, L"OC_MENU_ACTIVE") != 0;
		menuActive = menuSignalActive;

		// Master gate — laser stays fully dormant unless enable_laser=1
		const bool menuLaserMasterEnabled = s_mqEnableLaser &&
		    oovr_global_configuration.MenuLaserEnabled();
		if (!menuLaserMasterEnabled)
			menuActive = false;

		// CALIBRATION OVERRIDE (2026-07-25, user request): always_show_quad=1
		// keeps the quad up at ALL times — menus come and go, the quad stays.
		// Skips every menu/plane gate below; the quad renders head-anchored at
		// the fallback pose until a menu opens and exports the real plane.
		bool alwaysShow = menuLaserMasterEnabled && s_mqAlwaysShow;
		if (alwaysShow)
			menuActive = true;

		// LOADING-SCREEN GUARD (2026-07-25): the bridge's OC_MENU_ACTIVE flag
		// includes gamePaused (so WASD passthrough works in text boxes), and
		// loading screens pause the game — the quad was arming on every
		// loading screen with no menu present. Require an actual tracked
		// menu name from shared memory: never CREATE without one, and tear
		// down after ~15 frames without one (tolerates transient seqlock
		// read misses mid-menu).
		static int s_noMenuNameFrames = 0;
		static bool s_mapNativeOnlyLatched = false;
		bool mapNativeOnly = false;
		if (menuActive) {
			OpenSharedMemory();
			OCMenuTransform mxGate = {};
			bool readOk = ReadMenuTransform(mxGate);
			bool named = readOk && mxGate.menuName[0] != '\0';
			if (!menuSignalActive) {
				s_mapNativeOnlyLatched = false;
			} else if (named) {
				s_mapNativeOnlyLatched = strcmp(mxGate.menuName, "MapMenu") == 0;
			}
			mapNativeOnly = menuSignalActive && s_mapNativeOnlyLatched;

			// MapMenu is only a lifecycle-name sentinel here. Keep any existing
			// flat-menu laser allocation idle and never enter its renderer, publish
			// pointer input, or submit a compositor layer while Skyrim owns the map.
			if (mapNativeOnly) {
				g_menuLaserActive = false;
				for (int side = 0; side < 2; ++side) {
					g_menuLaserConsumesTrigger[side] = false;
					g_menuLaserSuppressUntilRelease[side].store(false, std::memory_order_release);
				}
				if (s_pTransform) {
					s_pTransform->laserActive = 0;
					s_pTransform->laserHand = 0xFF;
					s_pTransform->laserTriggerHeld = 0;
				}
			}

			if (!alwaysShow && !mapNativeOnly) {
			// Pump present (v2 bridge) but refusing to export a plane = the SKSE
			// side is deliberately dormant (StatsMenu-on-top-of-TweenMenu left a
			// stale fallback quad floating in Sovngarde, 2026-07-25). Never show
			// the fallback in that state: delay creation until the first valid
			// plane, and tear down if the export starves mid-menu. Transient
			// seqlock read misses reset nothing (readOk=false = no information).
			if (named)
				s_noMenuNameFrames = 0;
			else if (readOk)
				s_noMenuNameFrames++;
			if (!menuLaser) {
				if (!named)
					menuActive = false; // loading screen / plain pause / StatsMenu — stay dormant
			} else if (s_noMenuNameFrames > 15) {
				menuActive = false; // menu closed — tear down
			}
			}
		}

		if (menuActive && !mapNativeOnly) {
			OOVR_LOG_ONCE("MCM menu detected active via OC_MENU_ACTIVE property");

			// Try to open shared memory from SKSE plugin (once)
			OpenSharedMemory();

			// Create menu laser system on first detection
			static bool s_feedbackWritten = false;

			ID3D11Device* laserDev = BaseCompositor::dxcomp ? BaseCompositor::dxcomp->GetDevice() : nullptr;
			if (!menuLaser && laserDev && reinterpret_cast<uintptr_t>(laserDev) > 0xFFFF) {
				OOVR_LOG("Creating VRMenuLaser system");
				menuLaser = std::make_unique<VRMenuLaser>(laserDev);
				s_mqHasAnchor = false; // Re-anchor from current head on menu reopen
				s_feedbackWritten = false; // Write feedback on first anchored frame
				s_planeAdoptLogsLeft = 2; // Re-log plane adoption state for THIS menu open
				s_planeFallbackLogsLeft = 2;

				// Log head pose at menu open
				XrSpaceLocation openHead = { XR_TYPE_SPACE_LOCATION };
				xrLocateSpace(xr_gbl->viewSpace, xr_space_from_ref_space_type(GetUnsafeBaseSystem()->currentSpace),
				    xr_gbl->nextPredictedFrameTime, &openHead);
				OOVR_DEBUG_LOGF("CAL MENU-OPEN head(%.4f, %.4f, %.4f) orient(%.4f, %.4f, %.4f, %.4f)",
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
				OOVR_DEBUG_LOGF("MENU-OPEN: ReadMenuTransform=%s menuName='%s'",
				    readOk ? "OK" : "FAIL", readOk ? mxOpen.menuName : "(n/a)");
				if (readOk && mxOpen.menuName[0] != '\0') {
					float pDist, pW, pH, pYOff, pXOff;
					int pOpacity;
					GetMenuProfile(mxOpen.menuName, pDist, pW, pH, pYOff, pXOff, pOpacity);
					s_profileActive = true; // Lock quad dims — file watcher won't override
					s_mqDist = pDist; s_mqWidthScale = pW; s_mqHeightScale = pH;
					s_mqYOffset = pYOff; s_mqXOffset = pXOff;
					s_mqYawOffset = 0; s_mqPitchOffset = 0; s_mqRollOffset = 0;
					// Profiles carry a faint default (20). During calibration the ini
					// opacity should win when higher — a 15-20% grid is invisible over
					// lit menu content (2026-07-25 "quad not showing in menus").
					s_mqOpacity = (pOpacity > s_mqOpacity) ? pOpacity : s_mqOpacity;
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
						OOVR_DEBUG_LOGF("Menu changed to '%s' — applying hardcoded profile", s_lastMenuName);

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
							// Profiles carry a faint default (20). During calibration the ini
					// opacity should win when higher — a 15-20% grid is invisible over
					// lit menu content (2026-07-25 "quad not showing in menus").
					s_mqOpacity = (pOpacity > s_mqOpacity) ? pOpacity : s_mqOpacity;
						}
					}
				}

				// Compute menu quad from head position + settings
				XrSpaceLocation headLoc = { XR_TYPE_SPACE_LOCATION };
				xrLocateSpace(xr_gbl->viewSpace, xr_space_from_ref_space_type(GetUnsafeBaseSystem()->currentSpace),
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
				const bool physicalBookMode = strcmp(s_lastMenuName, "Book Menu") == 0;
				bool liveSharedPlaneAdopted = false;

				// v2 GROUND TRUTH: if the SKSE plugin is exporting the game's
				// real uiNode plane, use it instead of the head-anchored guess.
				// This is what kills the off-center drift — the raycast quad IS
				// the plane the menu is rendered on.
				//
				// HARD VALIDATION before anything reaches the compositor: a
				// non-unit layer quat makes xrEndFrame fail with
				// XR_ERROR_POSE_INVALID on EVERY frame = frozen display
				// (happened live 2026-07-10). Reject and fall back instead.
				{
					OCMenuTransform mxPlane = {};
					bool readOk = ReadMenuTransform(mxPlane);
					if (readOk && mxPlane.version >= 2 && mxPlane.uiPlaneValid &&
					    mxPlane.uiPlaneWidth > 0.01f && mxPlane.uiPlaneWidth < 20.0f &&
					    mxPlane.uiPlaneHeight > 0.01f && mxPlane.uiPlaneHeight < 20.0f) {
						XrVector3f p = { mxPlane.uiPlanePos[0], mxPlane.uiPlanePos[1], mxPlane.uiPlanePos[2] };
						XrQuaternionf q = { mxPlane.uiPlaneQuat[0], mxPlane.uiPlaneQuat[1],
						    mxPlane.uiPlaneQuat[2], mxPlane.uiPlaneQuat[3] };
						float qn = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
						bool posSane = std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) &&
						    fabsf(p.x) < 15.0f && p.y > -2.0f && p.y < 8.0f && fabsf(p.z) < 15.0f;
						bool quatSane = std::isfinite(qn) && fabsf(qn - 1.0f) < 0.05f;
						if (posSane && quatSane) {
							liveSharedPlaneAdopted = true;
							// Renormalize anyway — belt and suspenders
							q.x /= qn; q.y /= qn; q.z /= qn; q.w /= qn;

							// v3: map Skyrim RoomNode space into the live OpenXR Stage
							// origin through the HMD pose common to both frames.
							if (mxPlane.version >= 3 && mxPlane.roomHmdValid) {
								XrVector3f roomHmd = { mxPlane.roomHmdPos[0], mxPlane.roomHmdPos[1], mxPlane.roomHmdPos[2] };
								XrQuaternionf roomHmdQ = { mxPlane.roomHmdQuat[0], mxPlane.roomHmdQuat[1],
								    mxPlane.roomHmdQuat[2], mxPlane.roomHmdQuat[3] };
								float hqn = sqrtf(roomHmdQ.x * roomHmdQ.x + roomHmdQ.y * roomHmdQ.y +
								    roomHmdQ.z * roomHmdQ.z + roomHmdQ.w * roomHmdQ.w);
								bool hmdSane = std::isfinite(roomHmd.x) && std::isfinite(roomHmd.y) && std::isfinite(roomHmd.z) &&
								    fabsf(roomHmd.x) < 20.0f && roomHmd.y > -5.0f && roomHmd.y < 10.0f &&
								    fabsf(roomHmd.z) < 20.0f && std::isfinite(hqn) && hqn > 0.95f && hqn < 1.05f;
								if (hmdSane) {
									roomHmdQ.x /= hqn; roomHmdQ.y /= hqn; roomHmdQ.z /= hqn; roomHmdQ.w /= hqn;
									XrVector3f roomFwd;
									rotate_vector_by_quaternion({ 0, 0, -1 }, roomHmdQ, roomFwd);
									// RoomNode and OpenXR Stage axes are already parallel. The
									// UprightHmdNode yaw is frozen/stale in Skyrim while the OpenXR
									// HMD yaw is live; subtracting them makes the quad orbit in the
									// opposite direction on every head turn. Correct origin only.
									float yawDelta = 0.0f;
									XrQuaternionf roomToStage = { 0.0f, sinf(yawDelta * 0.5f), 0.0f, cosf(yawDelta * 0.5f) };
									XrVector3f roomRelative = { p.x - roomHmd.x, p.y - roomHmd.y, p.z - roomHmd.z };
									XrVector3f stageRelative;
									rotate_vector_by_quaternion(roomRelative, roomToStage, stageRelative);
									XrVector3f rawRoomPlane = p;
									p = { headPos.x + stageRelative.x, headPos.y + stageRelative.y, headPos.z + stageRelative.z };

									auto quatMul = [](const XrQuaternionf& a, const XrQuaternionf& b) -> XrQuaternionf {
										return {
										    a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
										    a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
										    a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
										    a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z
										};
									};
									q = quatMul(roomToStage, q);
									if (s_planeAdoptLogsLeft > 0) {
										OOVR_DEBUG_LOGF("Menu plane Room->Stage: roomPlane=(%.3f,%.3f,%.3f) roomHmd=(%.3f,%.3f,%.3f) stageHmd=(%.3f,%.3f,%.3f) yawDelta=%.2fdeg mapped=(%.3f,%.3f,%.3f)",
										    rawRoomPlane.x, rawRoomPlane.y, rawRoomPlane.z,
										    roomHmd.x, roomHmd.y, roomHmd.z, headPos.x, headPos.y, headPos.z,
										    yawDelta * 180.0f / 3.14159265f, p.x, p.y, p.z);
									}
								}
							}

							// FACE-THE-VIEWER GUARD (2026-07-25): the game's uiNode
							// rotation contains a reflection; the rebuilt basis can leave
							// the quad's +Z face pointing away from the player, and quad
							// layers can render one-sided — a perfectly placed quad that
							// faces away is simply invisible (the fallback quads face the
							// player by construction, which is why only THOSE were ever
							// seen). If the face normal points away from the HMD, spin
							// 180° about local Y. (This mirrors U relative to the game's
							// menu — the mouse mapping calibration absorbs it.)
							XrVector3f faceN;
							rotate_vector_by_quaternion({ 0, 0, 1 }, q, faceN);
							XrVector3f toHead = { headPos.x - p.x, headPos.y - p.y, headPos.z - p.z };
							float facing = faceN.x * toHead.x + faceN.y * toHead.y + faceN.z * toHead.z;
							if (facing < 0.0f) {
								XrQuaternionf qf = { -q.z, q.w, q.x, -q.y }; // q ⊗ (180° about Y)
								q = qf;
								rotate_vector_by_quaternion({ 0, 0, 1 }, q, faceN);
								if (s_planeAdoptLogsLeft > 0)
									OOVR_DEBUG_LOG("Menu laser: shared plane faced AWAY from viewer — flipped 180");
							}
							// Nudge 2cm toward the viewer so the quad never z-fights the
							// menu surface the game draws at this exact plane.
							p.x += faceN.x * 0.02f;
							p.y += faceN.y * 0.02f;
							p.z += faceN.z * 0.02f;

							// Live calibration trim (ini, hot-reload): shift along the
							// quad's own axes and scale its extent. The visible menu
							// image doesn't sit exactly on the uiNode geometry, the
							// user dials these in-headset against the numbered grid.
							if (!physicalBookMode) {
								XrVector3f upN, rightN;
								rotate_vector_by_quaternion({ 0, 1, 0 }, q, upN);
								rotate_vector_by_quaternion({ 1, 0, 0 }, q, rightN);
								p.x += -upN.x * s_mqPlaneShiftDown - faceN.x * s_mqPlaneShiftBack + rightN.x * s_mqPlaneShiftRight;
								p.y += -upN.y * s_mqPlaneShiftDown - faceN.y * s_mqPlaneShiftBack + rightN.y * s_mqPlaneShiftRight;
								p.z += -upN.z * s_mqPlaneShiftDown - faceN.z * s_mqPlaneShiftBack + rightN.z * s_mqPlaneShiftRight;
							}

							quadPose.position = p;
							quadPose.orientation = q;
							float ps = physicalBookMode ? 1.0f :
							    ((s_mqPlaneScale > 0.1f && s_mqPlaneScale < 10.0f) ? s_mqPlaneScale : 1.0f);
							quadSize = { mxPlane.uiPlaneWidth * ps, mxPlane.uiPlaneHeight * ps };
							if (s_planeAdoptLogsLeft > 0) {
								s_planeAdoptLogsLeft--;
								OOVR_DEBUG_LOGF("Menu laser: using game uiNode plane pos(%.3f,%.3f,%.3f) quat(%.3f,%.3f,%.3f,%.3f) size %.3fx%.3fm",
								    p.x, p.y, p.z, q.x, q.y, q.z, q.w, quadSize.width, quadSize.height);
							}
						} else {
							if (s_planeFallbackLogsLeft > 0) {
								s_planeFallbackLogsLeft--;
								OOVR_LOGF("Menu laser: REJECTED shared plane pos(%.3f,%.3f,%.3f) |q|=%.3f — using fallback profile quad",
								    p.x, p.y, p.z, qn);
							}
						}
					} else {
						// Shared plane unavailable — fallback profile quad is showing.
						// This is the "small quad" state; log WHY, per menu open.
						if (s_planeFallbackLogsLeft > 0) {
							s_planeFallbackLogsLeft--;
							OOVR_LOGF("Menu laser: shared plane unavailable (read=%d ver=%u valid=%u w=%.3f h=%.3f) — using fallback profile quad %.2fx%.2fm",
							    (int)readOk, readOk ? mxPlane.version : 0,
							    readOk ? (unsigned)mxPlane.uiPlaneValid : 0,
							    readOk ? mxPlane.uiPlaneWidth : 0.0f,
							    readOk ? mxPlane.uiPlaneHeight : 0.0f,
							    quadSize.width, quadSize.height);
						}
					}
				}

				menuLaser->SetMenuQuad(quadPose, quadSize);
				// Low-rate live transform trace. This makes physical translation
				// failures measurable: head, rendered quad, and their relative vector
				// are captured in the same OpenXR reference space every two seconds.
				static ULONGLONG s_nextMenuTrackLog = 0;
				ULONGLONG trackNow = oovr_debug_logging_enabled() ? GetTickCount64() : 0;
				if (oovr_debug_logging_enabled() && s_mqShowDebug && trackNow >= s_nextMenuTrackLog) {
					s_nextMenuTrackLog = trackNow + 2000;
					OOVR_DEBUG_LOGF("MENU TRACK LIVE menu=%s space=%d head=(%.4f,%.4f,%.4f) quad=(%.4f,%.4f,%.4f) rel=(%.4f,%.4f,%.4f) size=(%.4f,%.4f)",
					    s_lastMenuName, (int)GetUnsafeBaseSystem()->currentSpace,
					    headPos.x, headPos.y, headPos.z,
					    quadPose.position.x, quadPose.position.y, quadPose.position.z,
					    quadPose.position.x - headPos.x,
					    quadPose.position.y - headPos.y,
					    quadPose.position.z - headPos.z,
					    quadSize.width, quadSize.height);
				}
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
						OOVR_DEBUG_LOGF("Feedback: menu=%s head(%.3f,%.3f,%.3f) yaw=%.1f quad(%.3f,%.3f,%.3f)",
						    menuNameStr,
						    s_mqAnchorHeadPos.x, s_mqAnchorHeadPos.y, s_mqAnchorHeadPos.z,
						    headYawDeg,
						    quadCenter.x, quadCenter.y, quadCenter.z);
					}
				}

				bool hardSuppressLaser = (strcmp(s_lastMenuName, "StatsMenu") == 0)
				    || (strcmp(s_lastMenuName, "Loading Menu") == 0)
				    || (strcmp(s_lastMenuName, "Main Menu") == 0)
				    || (strcmp(s_lastMenuName, "Mist Menu") == 0)
				    || !oovr_global_configuration.MenuLaserEnabled();
				const bool physicalBookPending = physicalBookMode && !liveSharedPlaneAdopted;
				const bool suppressLaser = hardSuppressLaser || physicalBookPending;

				// Both hands render a menu beam, but Skyrim/Scaleform still has one
				// cursor. Meaningful on-quad movement or a click transfers that cursor,
				// while both beams remain visually identical. Default owner: right hand.
				static int s_activeLaserHand = 1;
				static bool s_authorityAnchorValid[2] = {};
				static float s_authorityAnchorU[2] = {};
				static float s_authorityAnchorV[2] = {};
				static char s_authorityMenu[64] = {};
				if (strcmp(s_authorityMenu, s_lastMenuName) != 0) {
					s_authorityAnchorValid[0] = false;
					s_authorityAnchorValid[1] = false;
					snprintf(s_authorityMenu, sizeof(s_authorityMenu), "%s", s_lastMenuName);
				}
				menuLaser->SetActiveHand(s_activeLaserHand);
				menuLaser->SetRenderHand(0, true);
				menuLaser->SetRenderHand(1, true);

				bool kbHit[2] = { g_kbLaserConsumesTrigger[0], g_kbLaserConsumesTrigger[1] };
				if (!hardSuppressLaser) {
					const auto& menuLayers = menuLaser->Update(xr_gbl->nextPredictedFrameTime, kbHit);
					// Keep controller edge state warm while the physical book plane is
					// settling, but do not render or interact with the fallback quad.
					if (!physicalBookPending) {
						for (auto* l : menuLayers)
							layerHeaders.push_back(l);
					}
				}

				// Pointer authority follows the hand the user is actually aiming with.
				// Measure motion from a per-hand anchor instead of comparing consecutive
				// samples so slow deliberate movement eventually crosses the dead zone.
				// This prevents normal tracking noise from making two stationary hands
				// fight over Skyrim's single menu arrow.
				const bool ownerLocked = menuLaser->IsTriggerDown(s_activeLaserHand) ||
				    menuLaser->IsTriggerReleased(s_activeLaserHand);
				constexpr float kAuthorityMotionThreshold = 0.008f; // normalized quad units (~16 px at 2K)
				constexpr float kAuthorityMotionThresholdSq =
				    kAuthorityMotionThreshold * kAuthorityMotionThreshold;
				bool meaningfulMotion[2] = {};
				float motionDistanceSq[2] = {};
				for (int side = 0; side < 2; ++side) {
					if (suppressLaser || !menuLaser->IsHit(side)) {
						s_authorityAnchorValid[side] = false;
						continue;
					}

					const float u = menuLaser->GetHitU(side);
					const float v = menuLaser->GetHitV(side);
					if (!s_authorityAnchorValid[side]) {
						s_authorityAnchorU[side] = u;
						s_authorityAnchorV[side] = v;
						s_authorityAnchorValid[side] = true;
						continue;
					}

					const float du = u - s_authorityAnchorU[side];
					const float dv = v - s_authorityAnchorV[side];
					motionDistanceSq[side] = du * du + dv * dv;
					meaningfulMotion[side] = motionDistanceSq[side] >= kAuthorityMotionThresholdSq;
				}

				// A click is always an explicit ownership request. Preserve the trigger
				// mask even if another hand currently owns a drag, but do not transfer
				// the cursor until that drag's release edge has reached the SKSE bridge.
				int clickedHand = -1;
				for (int side = 0; !suppressLaser && side < 2; side++) {
					if (menuLaser->IsHit(side) && menuLaser->IsTriggerPressed(side)) {
						g_menuLaserSuppressUntilRelease[side].store(
						    true, std::memory_order_release);
						clickedHand = side;
					}
				}

				int authorityCandidate = -1;
				if (!ownerLocked && !suppressLaser) {
					if (clickedHand >= 0) {
						authorityCandidate = clickedHand;
					} else if (!menuLaser->IsHit(s_activeLaserHand) &&
					    menuLaser->IsHit(1 - s_activeLaserHand)) {
						// Do not strand the arrow on a hand that has left the quad.
						authorityCandidate = 1 - s_activeLaserHand;
					} else if (meaningfulMotion[0] || meaningfulMotion[1]) {
						if (meaningfulMotion[0] && meaningfulMotion[1])
							authorityCandidate = motionDistanceSq[1] > motionDistanceSq[0] ? 1 : 0;
						else
							authorityCandidate = meaningfulMotion[0] ? 0 : 1;
					}
				}

				const bool authorityEvent = authorityCandidate >= 0;
				if (authorityEvent)
					s_activeLaserHand = authorityCandidate;

				// Consume every threshold crossing, including motion rejected during a
				// drag. Reset both anchors after a real authority event so stale movement
				// from the other hand cannot steal the cursor on the following frame.
				for (int side = 0; side < 2; ++side) {
					if ((authorityEvent || meaningfulMotion[side]) && menuLaser->IsHit(side)) {
						s_authorityAnchorU[side] = menuLaser->GetHitU(side);
						s_authorityAnchorV[side] = menuLaser->GetHitV(side);
						s_authorityAnchorValid[side] = true;
					}
				}
				menuLaser->SetActiveHand(s_activeLaserHand);

				// Own the physical trigger only while the beam is
				// ON the quad, plus a short grace window after it leaves. The
				// grace covers the one-frame off-quad transition where Skyrim
				// could otherwise see the same trigger our Scaleform bridge
				// handled and activate the newly opened row underneath it. Off
				// the quad past the grace, the full legacy menu bindings
				// (trigger included) belong to the game again.
				g_menuLaserActive = !suppressLaser &&
				    (menuLaser->IsHit(0) || menuLaser->IsHit(1));
				static ULONGLONG s_lastQuadHitMs[2] = {};
				constexpr ULONGLONG kTriggerGraceMs = 250;
				for (int side = 0; side < 2; side++) {
					if (menuLaser->IsHit(side))
						s_lastQuadHitMs[side] = GetTickCount64();
					const bool recentHit = s_lastQuadHitMs[side] != 0
					    && GetTickCount64() - s_lastQuadHitMs[side] <= kTriggerGraceMs;
					g_menuLaserConsumesTrigger[side] = !suppressLaser &&
					    !kbHit[side] && menuLaser->IsRayValid(side) && recentHit;
				}

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
					OOVR_DEBUG_LOGF("Menu quad adjustment mode: %s", s_adjustModeLocal ? "ON" : "OFF");
				}

				// Active if either local toggle OR ini toggle is on
				bool s_adjustMode = s_adjustModeLocal || s_mqThumbstickAdjust;

				// X button cycles right-stick parameter
				if (s_adjustMode && menuLaser->IsXButtonPressed(0)) {
					s_rightStickParam = (s_rightStickParam + 1) % 3;
					const char* names[] = { "Width", "Height", "Opacity" };
					OOVR_DEBUG_LOGF("Right stick adjusts: %s", names[s_rightStickParam]);
				}

				// Explicit four-click mouse calibration. The LEFT controller's X
				// button is the only confirmation button; the right controller's A
				// button is deliberately never used. Sequence:
				//   1 right laser at intended target, 2 right laser at visible mouse,
				//   3 left laser at intended target,  4 left laser at visible mouse.
				// A held button records once because IsXButtonPressed is edge based.
				// We log the reverse correction instead of changing the live mapping
				// so both hands can be compared before committing an offset/scale fix.
				static int s_mouseCalStep = 0;
				static char s_mouseCalMenu[64] = {};
				static float s_mouseCalTargetU[2] = {};
				static float s_mouseCalTargetV[2] = {};
				if (!s_adjustMode && menuLaser->IsXButtonPressed(0)) {
					if (strcmp(s_mouseCalMenu, s_lastMenuName) != 0) {
						s_mouseCalStep = 0;
						snprintf(s_mouseCalMenu, sizeof(s_mouseCalMenu), "%s", s_lastMenuName);
						s_activeLaserHand = 1;
						OOVR_DEBUG_LOGF("MOUSE CAL: new menu '%s'; sequence reset, RIGHT TARGET expected", s_lastMenuName);
					}

					const int calSide = (s_mouseCalStep < 2) ? 1 : 0;
					const bool targetStep = ((s_mouseCalStep & 1) == 0);
					const char* handName = calSide == 0 ? "LEFT" : "RIGHT";
					if (suppressLaser || !menuLaser->IsHit(calSide)) {
						OOVR_DEBUG_LOGF("MOUSE CAL %d/4 %s %s: X IGNORED -- that laser is not hitting the menu quad",
						    s_mouseCalStep + 1, handName, targetStep ? "TARGET" : "MOUSE");
					} else {
						const float rawU = menuLaser->GetHitU(calSide);
						const float rawV = menuLaser->GetHitV(calSide);
						const float adjU = rawU * s_mqMouseScaleX + s_mqMouseOffsetX;
						const float adjV = rawV * s_mqMouseScaleY + s_mqMouseOffsetY;
						const XrVector3f rayO = menuLaser->GetRayOrigin(calSide);
						const XrVector3f rayD = menuLaser->GetRayDir(calSide);

						if (targetStep) {
							s_mouseCalTargetU[calSide] = rawU;
							s_mouseCalTargetV[calSide] = rawV;
							OOVR_DEBUG_LOGF("MOUSE CAL %d/4 %s TARGET CAPTURED: menu=%s raw=(%.6f,%.6f) adjusted=(%.6f,%.6f) t=%.4f rayO=(%.4f,%.4f,%.4f) rayD=(%.4f,%.4f,%.4f)",
							    s_mouseCalStep + 1, handName, s_lastMenuName, rawU, rawV, adjU, adjV,
							    menuLaser->GetHitT(calSide), rayO.x, rayO.y, rayO.z, rayD.x, rayD.y, rayD.z);
						} else {
							const float deltaU = rawU - s_mouseCalTargetU[calSide];
							const float deltaV = rawV - s_mouseCalTargetV[calSide];
							// With the current normalized Scaleform convention, translating
							// the command opposite the observed visual error gives the
							// one-point reverse correction. Multiple pairs expose scale/pose
							// errors instead of incorrectly baking them into an offset.
							const float proposedOffsetX = s_mqMouseOffsetX - deltaU;
							const float proposedOffsetY = s_mqMouseOffsetY - deltaV;
							OOVR_DEBUG_LOGF("MOUSE CAL %d/4 %s MOUSE CAPTURED: menu=%s raw=(%.6f,%.6f) adjusted=(%.6f,%.6f) target=(%.6f,%.6f) visualDelta=(%+.6f,%+.6f) pixels2048=(%+.1f,%+.1f) REVERSE_OFFSETS=(%.6f,%.6f) current=(%.6f,%.6f)",
							    s_mouseCalStep + 1, handName, s_lastMenuName, rawU, rawV, adjU, adjV,
							    s_mouseCalTargetU[calSide], s_mouseCalTargetV[calSide], deltaU, deltaV,
							    deltaU * 2048.0f, deltaV * 2048.0f, proposedOffsetX, proposedOffsetY,
							    s_mqMouseOffsetX, s_mqMouseOffsetY);
						}

						s_mouseCalStep++;
						if (s_mouseCalStep == 2) {
							s_activeLaserHand = 0;
							OOVR_DEBUG_LOG("MOUSE CAL: RIGHT pair complete; visible laser switched to LEFT -- LEFT TARGET expected");
						} else if (s_mouseCalStep >= 4) {
							s_mouseCalStep = 0;
							s_activeLaserHand = 1;
							OOVR_DEBUG_LOG("MOUSE CAL: BOTH HANDS complete; sequence reset and visible laser switched to RIGHT");
						}
					}
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
							// MUST persist every key the watcher parses — a rewrite
							// that drops enable_laser/show_sf_cursor silently kills
							// the laser system on the next file reload.
							fprintf(sf, "[menu_quad]\nenable_laser=%d\nalways_show_quad=%d\ndistance=%.2f\nwidth_scale=%.2f\nheight_scale=%.2f\n"
							    "y_offset=%.2f\nx_offset=%.2f\nyaw_degrees=%d\npitch_degrees=%d\n"
							    "roll_degrees=%d\nopacity=%d\nshow_debug=%d\nhead_locked=%d\nthumbstick_adjust=%d\n"
							    "plane_shift_down=%.3f\nplane_shift_back=%.3f\nplane_shift_right=%.3f\nplane_scale=%.3f\n"
							    "mouse_offset_x=%.3f\nmouse_offset_y=%.3f\nmouse_scale_x=%.3f\nmouse_scale_y=%.3f\n"
							    "show_calibration_quad=%d\nshow_profile_quad=%d\nshow_sf_cursor=%d\n",
							    s_mqEnableLaser ? 1 : 0,
							    s_mqAlwaysShow ? 1 : 0,
							    s_mqDist, s_mqWidthScale, s_mqHeightScale,
							    s_mqYOffset, s_mqXOffset,
							    (int)(s_mqYawOffset * 180.0f / 3.14159265f),
							    (int)(s_mqPitchOffset * 180.0f / 3.14159265f),
							    (int)(s_mqRollOffset * 180.0f / 3.14159265f),
							    s_mqOpacity, s_mqShowDebug ? 1 : 0, s_mqHeadLocked ? 1 : 0, s_mqThumbstickAdjust ? 1 : 0,
							    s_mqPlaneShiftDown, s_mqPlaneShiftBack, s_mqPlaneShiftRight, s_mqPlaneScale,
							    s_mqMouseOffsetX, s_mqMouseOffsetY, s_mqMouseScaleX, s_mqMouseScaleY,
							    s_mqShowCalibQuad ? 1 : 0, s_mqShowProfileQuad ? 1 : 0, s_mqShowSfCursor ? 1 : 0);
							fclose(sf);
							OOVR_LOGF("Saved quad settings: dist=%.2f w=%.2f h=%.2f yOff=%.2f xOff=%.2f opacity=%d",
							    s_mqDist, s_mqWidthScale, s_mqHeightScale, s_mqYOffset, s_mqXOffset, s_mqOpacity);
						}
					}
				}

				// v2: publish laser state to the SKSE plugin via shared memory.
				// The plugin drives the game's own MenuCursor and mouse button
				// events through BSInputEventQueue on the game thread. No window
				// messages, no Scaleform, on either side. Only the owning hand
				// (last meaningfully moved or clicked on the quad) feeds the pointer.
				if (s_pTransform) {
					bool wroteHit = false;
					int side = s_activeLaserHand;
					s_pTransform->laserHand = static_cast<uint8_t>(side);
					s_pTransform->laserTriggerHeld = menuLaser->IsTriggerDown(side) ? 1 : 0;
					if (!suppressLaser && menuLaser->IsHit(side)) {
						// Calibration trims retained (default identity)
						float adjU = physicalBookMode ? menuLaser->GetHitU(side) :
						    menuLaser->GetHitU(side) * s_mqMouseScaleX + s_mqMouseOffsetX;
						float adjV = physicalBookMode ? menuLaser->GetHitV(side) :
						    menuLaser->GetHitV(side) * s_mqMouseScaleY + s_mqMouseOffsetY;
						adjU = adjU < 0.0f ? 0.0f : (adjU > 1.0f ? 1.0f : adjU);
						adjV = adjV < 0.0f ? 0.0f : (adjV > 1.0f ? 1.0f : adjV);
						s_pTransform->laserU = adjU;
						s_pTransform->laserV = adjV;
						s_pTransform->laserActive = 1;
						if (menuLaser->IsTriggerPressed(side))
							s_pTransform->laserPressSeq++;
						wroteHit = true;
					}
					// A held drag can leave the quad before the trigger comes up. Publish
					// that owning-hand release even without a current hit so SKSE can
					// always close the exact interaction that received DOWN.
					if (!suppressLaser && menuLaser->IsTriggerReleased(side))
						s_pTransform->laserReleaseSeq++;
					if (!wroteHit)
						s_pTransform->laserActive = 0;
					s_pTransform->laserShowCursor = s_mqShowSfCursor ? 1 : 0;
					s_pTransform->laserFrameSeq++;
				}
			}
		} else if (!menuActive) {
			// Menu closed — destroy laser system, unlock profile
			if (menuLaser) {
				OOVR_DEBUG_LOG("Menu laser: destroyed (menu-active flag went false)");
				menuLaser.reset();
			}
			g_menuLaserActive = false;
			g_menuLaserConsumesTrigger[0] = false;
			g_menuLaserConsumesTrigger[1] = false;
			s_profileActive = false; // Allow file watcher to update quad dims again
			if (s_pTransform && s_pTransform->laserActive)
				s_pTransform->laserActive = 0; // let SKSE release a held click
			if (s_pTransform)
				s_pTransform->laserHand = 0xFF;
			if (s_pTransform)
				s_pTransform->laserTriggerHeld = 0;
		}
	}
#endif // _WIN32 (menu laser system)
#endif // Menu laser system v2

	if (!oovr_global_configuration.EnableLayers()) {
		goto done;
	}

	{ // Scope block to avoid goto-past-initialization errors
		// Collect visible overlays, then sort by sortOrder so higher values render on top.
		std::vector<OverlayData*> sortedOverlays;
		for (const auto& kv : overlays) {
			if (kv.second) {
				OverlayData& overlay = *kv.second;
				if (!overlay.visible || overlay.texture.handle == nullptr)
					continue;
				if ((uint64_t)overlay.layerQuad.subImage.swapchain == 0)
					continue;
				const XrRect2Di& srcSize = overlay.layerQuad.subImage.imageRect;
				if (srcSize.extent.height <= 8 && srcSize.extent.width <= 8)
					continue;
				sortedOverlays.push_back(&overlay);
			}
		}
		std::sort(sortedOverlays.begin(), sortedOverlays.end(),
			[](const OverlayData* a, const OverlayData* b) { return a->sortOrder < b->sortOrder; });

		for (OverlayData* overlayPtr : sortedOverlays) {
			OverlayData& overlay = *overlayPtr;

			// Calculate the texture's aspect ratio
			const XrRect2Di& srcSize = overlay.layerQuad.subImage.imageRect;
			const float aspect = srcSize.extent.height > 0 ? (float)srcSize.extent.width / (float)srcSize.extent.height : 1.0f;
			overlay.layerQuad.size.width = overlay.widthMeters;
			overlay.layerQuad.size.height = overlay.widthMeters / aspect;

			// Extract position + rotation from the overlay transform.
			// overlayTransform is stored via S2O_om44 which copies HmdMatrix34_t
			// without transposing: overlayTransform[y][x] = hmd.m[y][x].
			// Since GLM is column-major, this means the data is transposed from
			// GLM's perspective. We extract values treating [y][x] as row y, col x.
			const auto& M = overlay.overlayTransform;
			overlay.layerQuad.pose.position = { M[0][3], M[1][3], M[2][3] };

			// Quaternion from rotation matrix (Shepperd method, row-major access)
			float trace = M[0][0] + M[1][1] + M[2][2];
			XrQuaternionf q;
			if (trace > 0.0f) {
				float s = sqrtf(trace + 1.0f) * 2.0f;
				q.w = 0.25f * s;
				q.x = (M[2][1] - M[1][2]) / s;
				q.y = (M[0][2] - M[2][0]) / s;
				q.z = (M[1][0] - M[0][1]) / s;
			} else if (M[0][0] > M[1][1] && M[0][0] > M[2][2]) {
				float s = sqrtf(1.0f + M[0][0] - M[1][1] - M[2][2]) * 2.0f;
				q.w = (M[2][1] - M[1][2]) / s;
				q.x = 0.25f * s;
				q.y = (M[0][1] + M[1][0]) / s;
				q.z = (M[0][2] + M[2][0]) / s;
			} else if (M[1][1] > M[2][2]) {
				float s = sqrtf(1.0f + M[1][1] - M[0][0] - M[2][2]) * 2.0f;
				q.w = (M[0][2] - M[2][0]) / s;
				q.x = (M[0][1] + M[1][0]) / s;
				q.y = 0.25f * s;
				q.z = (M[1][2] + M[2][1]) / s;
			} else {
				float s = sqrtf(1.0f + M[2][2] - M[0][0] - M[1][1]) * 2.0f;
				q.w = (M[1][0] - M[0][1]) / s;
				q.x = (M[0][2] + M[2][0]) / s;
				q.y = (M[1][2] + M[2][1]) / s;
				q.z = 0.25f * s;
			}
			overlay.layerQuad.pose.orientation = q;

			layerHeaders.push_back((XrCompositionLayerBaseHeader*)&overlay.layerQuad);
		}
	} // end sort-order scope

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
EVROverlayError BaseOverlay::CreateSubviewOverlay(VROverlayHandle_t parentOverlayHandle,
    const char* pchSubviewOverlayKey, const char* pchSubviewOverlayName,
    VROverlayHandle_t* pSubviewOverlayHandle)
{
	(void)parentOverlayHandle;
	(void)pchSubviewOverlayKey;
	(void)pchSubviewOverlayName;
	if (pSubviewOverlayHandle)
		*pSubviewOverlayHandle = vr::k_ulOverlayHandleInvalid;
	OOVR_LOG_ONCE("IVROverlay_028: subview overlays are not supported; returning RequestFailed");
	return VROverlayError_RequestFailed;
}
EVROverlayError BaseOverlay::DestroyOverlay(VROverlayHandle_t ulOverlayHandle)
{
	// SkyUI destroys its temporary keyboard overlay from a polling worker. Keep
	// both the overlay and keyboard alive until _BuildLayers can tear them down
	// on the compositor thread that owns their D3D/OpenXR resources.
	{
		std::lock_guard<std::mutex> lock(pendingOverlayDestroyMutex);
		OverlayData* queuedOverlay = (OverlayData*)ulOverlayHandle;
		if (!queuedOverlay || !validOverlays.count(queuedOverlay) ||
		    !overlays.count(queuedOverlay->key))
			return VROverlayError_InvalidHandle;

		if (!queuedOverlay->destroyQueued) {
			queuedOverlay->destroyQueued = true;
			pendingOverlayDestroys.push_back(queuedOverlay);
			OOVR_LOGF("DestroyOverlay queued for compositor thread: key='%s' overlay=0x%llX callerTid=%lu",
			    queuedOverlay->key.c_str(),
			    (unsigned long long)(uintptr_t)queuedOverlay,
			    (unsigned long)GetCurrentThreadId());
		}
	}
	return VROverlayError_None;

#if 0 // Historical worker-thread destruction retained for reference only.
	USEH();

	if (highQualityOverlay == ulOverlayHandle)
		highQualityOverlay = vr::k_ulOverlayHandleInvalid;

	// [KB-DIAG] Check dxcomp health BEFORE overlay destruction
#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
	{
		ID3D11Device* preDeviceCheck = BaseCompositor::dxcomp ? BaseCompositor::dxcomp->GetDevice() : nullptr;
		OOVR_LOGF("[KB-DIAG] DestroyOverlay BEFORE: key='%s' overlay=0x%llX compositor=0x%llX dxcomp=0x%llX dxcomp->dev=0x%llX",
		    overlay->key.c_str(),
		    (unsigned long long)(uintptr_t)overlay,
		    (unsigned long long)(uintptr_t)overlay->compositor.get(),
		    (unsigned long long)(uintptr_t)BaseCompositor::dxcomp,
		    (unsigned long long)(uintptr_t)preDeviceCheck);
	}
#endif

	// If the keyboard was opened for this overlay, its dispatch lambda holds a raw
	// pointer to the overlay's event queue. Close it before the queue is freed —
	// pressing Done afterwards pushed into deleted memory (field crash, b.2).
	if (keyboard && keyboardOwner == overlay) {
		OOVR_LOG("DestroyOverlay: closing keyboard bound to dying overlay");
		keyboard.reset();
	keyboardOwner = nullptr;
	}
	if (keyboardOwner == overlay)
		keyboardOwner = nullptr;

	overlays.erase(overlay->key);
	validOverlays.erase(overlay);
	delete overlay;

	// [KB-DIAG] Check dxcomp health AFTER overlay destruction
#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
	{
		ID3D11Device* postDeviceCheck = BaseCompositor::dxcomp ? BaseCompositor::dxcomp->GetDevice() : nullptr;
		OOVR_LOGF("[KB-DIAG] DestroyOverlay AFTER: dxcomp=0x%llX dxcomp->dev=0x%llX",
		    (unsigned long long)(uintptr_t)BaseCompositor::dxcomp,
		    (unsigned long long)(uintptr_t)postDeviceCheck);
		if (postDeviceCheck && reinterpret_cast<uintptr_t>(postDeviceCheck) <= 0xFFFF) {
			OOVR_LOGF("[KB-DIAG] *** CORRUPTION DETECTED IN DestroyOverlay *** dxcomp->dev=0x%llX",
			    (unsigned long long)(uintptr_t)postDeviceCheck);
		}
	}
#endif

	return VROverlayError_None;
#endif
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
	USEH();
	overlay->sortOrder = unSortOrder;
	return VROverlayError_None;
}
EVROverlayError BaseOverlay::GetOverlaySortOrder(VROverlayHandle_t ulOverlayHandle, uint32_t* punSortOrder)
{
	USEH();
	if (punSortOrder)
		*punSortOrder = overlay->sortOrder;
	return VROverlayError_None;
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
EVROverlayError BaseOverlay::SetSubviewPosition(VROverlayHandle_t ulOverlayHandle, float fX, float fY)
{
	(void)ulOverlayHandle;
	(void)fX;
	(void)fY;
	OOVR_LOG_ONCE("IVROverlay_028: subview positioning is not supported; returning RequestFailed");
	return VROverlayError_RequestFailed;
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
	USEHB();

	if (!pParams || !pResults)
		return false;

	// Extract overlay basis vectors and position from the transform matrix.
	// overlayTransform uses S2O_om44 convention (non-transposing copy from HmdMatrix34_t),
	// so overlayTransform[glmCol][glmRow] = HMD m[glmCol][glmRow].
	// In HmdMatrix34_t (row-major), the columns of the rotation part are basis vectors:
	//   HMD col 0 = right, col 1 = up, col 2 = normal, col 3 = translation.
	// To extract HMD column C: read overlayTransform[0][C], [1][C], [2][C].
	const MfMatrix4f& xform = overlay->overlayTransform;

	vec3 right(xform[0][0], xform[1][0], xform[2][0]);
	vec3 up(xform[0][1], xform[1][1], xform[2][1]);
	vec3 normal(xform[0][2], xform[1][2], xform[2][2]);
	vec3 overlayPos(xform[0][3], xform[1][3], xform[2][3]);

	// Normalize basis vectors (should already be unit length, but be safe)
	float rightLen = glm::length(right);
	float upLen = glm::length(up);
	if (rightLen < 1e-6f || upLen < 1e-6f)
		return false;
	vec3 rightNorm = right / rightLen;
	vec3 upNorm = up / upLen;
	vec3 normalNorm = glm::normalize(normal);

	// Ray parameters
	vec3 rayOrigin(pParams->vSource.v[0], pParams->vSource.v[1], pParams->vSource.v[2]);
	vec3 rayDir(pParams->vDirection.v[0], pParams->vDirection.v[1], pParams->vDirection.v[2]);

	// Ray-plane intersection: t = dot(P0 - O, N) / dot(D, N)
	float denom = glm::dot(rayDir, normalNorm);
	if (fabsf(denom) < 1e-6f)
		return false; // Ray parallel to overlay plane

	float t = glm::dot(overlayPos - rayOrigin, normalNorm) / denom;
	if (t < 0.0f)
		return false; // Intersection behind the ray origin

	// Hit point in world space
	vec3 hitPoint = rayOrigin + t * rayDir;

	// Project hit point into overlay local space (distance along each axis)
	vec3 localOffset = hitPoint - overlayPos;
	float localX = glm::dot(localOffset, rightNorm);
	float localY = glm::dot(localOffset, upNorm);

	// Overlay dimensions: width is set directly, height derived from aspect ratio.
	// mouseScale is typically set to texture dimensions (e.g. {1920, 1080}).
	float width = overlay->widthMeters;
	float aspectRatio = (overlay->mouseScale.v[0] > 0.0f)
		? overlay->mouseScale.v[1] / overlay->mouseScale.v[0]
		: 1.0f;
	float height = width * aspectRatio;

	// Convert to UV coordinates [0,1] x [0,1]
	// OpenVR convention: u=0 left edge, u=1 right edge
	//                    v=0 top edge (+Y), v=1 bottom edge (-Y)
	float u = (localX / width) + 0.5f;
	float v = 0.5f - (localY / height);

	// Check bounds
	if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f)
		return false;

	// Fill results
	pResults->vPoint.v[0] = hitPoint.x;
	pResults->vPoint.v[1] = hitPoint.y;
	pResults->vPoint.v[2] = hitPoint.z;

	pResults->vNormal.v[0] = normalNorm.x;
	pResults->vNormal.v[1] = normalNorm.y;
	pResults->vNormal.v[2] = normalNorm.z;

	pResults->vUVs.v[0] = u;
	pResults->vUVs.v[1] = v;

	pResults->fDistance = t;

	return true;
}
bool BaseOverlay::HandleControllerOverlayInteractionAsMouse(VROverlayHandle_t ulOverlayHandle, TrackedDeviceIndex_t unControllerDeviceIndex)
{
	USEHB();

	// Track previous trigger state per overlay+controller for edge detection
	struct InteractionState {
		bool wasTriggerPressed = false;
		float lastMouseX = 0.0f;
		float lastMouseY = 0.0f;
	};
	static std::map<std::pair<VROverlayHandle_t, TrackedDeviceIndex_t>, InteractionState> s_interactionState;

	auto system = GetBaseSystem();
	if (!system)
		return false;

	// Get controller pose
	TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
	system->GetDeviceToAbsoluteTrackingPose(TrackingUniverseStanding, 0.0f, poses, vr::k_unMaxTrackedDeviceCount);

	if (unControllerDeviceIndex >= vr::k_unMaxTrackedDeviceCount || !poses[unControllerDeviceIndex].bPoseIsValid)
		return false;

	const HmdMatrix34_t& poseMat = poses[unControllerDeviceIndex].mDeviceToAbsoluteTracking;

	// Extract controller position (column 3 of the row-major matrix)
	vec3 controllerPos(poseMat.m[0][3], poseMat.m[1][3], poseMat.m[2][3]);

	// Controller forward direction is -Z in controller local space
	vec3 controllerFwd(-poseMat.m[0][2], -poseMat.m[1][2], -poseMat.m[2][2]);
	controllerFwd = glm::normalize(controllerFwd);

	// Build intersection params
	OOVR_VROverlayIntersectionParams_t params;
	params.vSource.v[0] = controllerPos.x;
	params.vSource.v[1] = controllerPos.y;
	params.vSource.v[2] = controllerPos.z;
	params.vDirection.v[0] = controllerFwd.x;
	params.vDirection.v[1] = controllerFwd.y;
	params.vDirection.v[2] = controllerFwd.z;
	params.eOrigin = TrackingUniverseStanding;

	OOVR_VROverlayIntersectionResults_t results;
	bool hit = ComputeOverlayIntersection(ulOverlayHandle, &params, &results);

	auto stateKey = std::make_pair(ulOverlayHandle, unControllerDeviceIndex);
	auto& state = s_interactionState[stateKey];

	if (hit) {
		// Convert UV to mouse coordinates using overlay's mouseScale
		// Mouse events use GL convention: (0,0) = bottom-left
		float mouseX = results.vUVs.v[0] * overlay->mouseScale.v[0];
		float mouseY = (1.0f - results.vUVs.v[1]) * overlay->mouseScale.v[1];

		// Generate mouse move event
		VREvent_t moveEvent = {};
		moveEvent.eventType = VREvent_MouseMove;
		moveEvent.trackedDeviceIndex = unControllerDeviceIndex;
		moveEvent.data.mouse.x = mouseX;
		moveEvent.data.mouse.y = mouseY;
		moveEvent.data.mouse.button = 0;

		{
			std::lock_guard<std::mutex> lock(overlay->eventMutex);
			overlay->eventQueue.push(moveEvent);
		}

		// Check trigger state for button events
		VRControllerState_t controllerState;
		if (system->GetControllerState(unControllerDeviceIndex, &controllerState, sizeof(controllerState))) {
			bool triggerPressed = (controllerState.ulButtonPressed & ButtonMaskFromId(k_EButton_SteamVR_Trigger)) != 0;

			if (triggerPressed && !state.wasTriggerPressed) {
				// Trigger just pressed — mouse button down
				VREvent_t downEvent = {};
				downEvent.eventType = VREvent_MouseButtonDown;
				downEvent.trackedDeviceIndex = unControllerDeviceIndex;
				downEvent.data.mouse.x = mouseX;
				downEvent.data.mouse.y = mouseY;
				downEvent.data.mouse.button = VRMouseButton_Left;

				std::lock_guard<std::mutex> lock(overlay->eventMutex);
				overlay->eventQueue.push(downEvent);
			} else if (!triggerPressed && state.wasTriggerPressed) {
				// Trigger just released — mouse button up
				VREvent_t upEvent = {};
				upEvent.eventType = VREvent_MouseButtonUp;
				upEvent.trackedDeviceIndex = unControllerDeviceIndex;
				upEvent.data.mouse.x = mouseX;
				upEvent.data.mouse.y = mouseY;
				upEvent.data.mouse.button = VRMouseButton_Left;

				std::lock_guard<std::mutex> lock(overlay->eventMutex);
				overlay->eventQueue.push(upEvent);
			}

			state.wasTriggerPressed = triggerPressed;
		}

		state.lastMouseX = mouseX;
		state.lastMouseY = mouseY;
	} else {
		// Not hitting overlay — if trigger was pressed, send button up
		if (state.wasTriggerPressed) {
			VREvent_t upEvent = {};
			upEvent.eventType = VREvent_MouseButtonUp;
			upEvent.trackedDeviceIndex = unControllerDeviceIndex;
			upEvent.data.mouse.x = state.lastMouseX;
			upEvent.data.mouse.y = state.lastMouseY;
			upEvent.data.mouse.button = VRMouseButton_Left;

			std::lock_guard<std::mutex> lock(overlay->eventMutex);
			overlay->eventQueue.push(upEvent);
		}
		state.wasTriggerPressed = false;
	}

	return hit;
}
bool BaseOverlay::IsHoverTargetOverlay(VROverlayHandle_t ulOverlayHandle)
{
	USEHB();

	// Check if either controller is currently pointing at this overlay
	auto system = GetBaseSystem();
	if (!system)
		return false;

	TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
	system->GetDeviceToAbsoluteTrackingPose(TrackingUniverseStanding, 0.0f, poses, vr::k_unMaxTrackedDeviceCount);

	for (TrackedDeviceIndex_t i = 0; i < vr::k_unMaxTrackedDeviceCount; i++) {
		if (!poses[i].bPoseIsValid)
			continue;
		if (system->GetTrackedDeviceClass(i) != TrackedDeviceClass_Controller)
			continue;

		const HmdMatrix34_t& poseMat = poses[i].mDeviceToAbsoluteTracking;
		vec3 pos(poseMat.m[0][3], poseMat.m[1][3], poseMat.m[2][3]);
		vec3 fwd(-poseMat.m[0][2], -poseMat.m[1][2], -poseMat.m[2][2]);
		fwd = glm::normalize(fwd);

		OOVR_VROverlayIntersectionParams_t params;
		params.vSource = { pos.x, pos.y, pos.z };
		params.vDirection = { fwd.x, fwd.y, fwd.z };
		params.eOrigin = TrackingUniverseStanding;

		OOVR_VROverlayIntersectionResults_t results;
		if (ComputeOverlayIntersection(ulOverlayHandle, &params, &results))
			return true;
	}

	return false;
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

	bool creatingNew = !overlay->compositor;
	if (creatingNew) {
		overlay->compositor.reset(GetUnsafeBaseCompositor()->CreateCompositorAPI(pTexture));
		overlay->compositor->isOverlay = true;
	}

	// [KB-DIAG] Check dxcomp health after overlay compositor creation (only on first call per overlay)
#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
	if (creatingNew && BaseCompositor::dxcomp) {
		ID3D11Device* ovlDevCheck = BaseCompositor::dxcomp->GetDevice();
		OOVR_LOGF("[KB-DIAG] SetOverlayTexture NEW compositor: overlay=0x%llX ovl_comp=0x%llX dxcomp=0x%llX dxcomp->dev=0x%llX",
		    (unsigned long long)(uintptr_t)overlay,
		    (unsigned long long)(uintptr_t)overlay->compositor.get(),
		    (unsigned long long)(uintptr_t)BaseCompositor::dxcomp,
		    (unsigned long long)(uintptr_t)ovlDevCheck);
		if (ovlDevCheck && reinterpret_cast<uintptr_t>(ovlDevCheck) <= 0xFFFF) {
			OOVR_LOGF("[KB-DIAG] *** CORRUPTION DETECTED after overlay compositor creation *** dxcomp->dev=0x%llX",
			    (unsigned long long)(uintptr_t)ovlDevCheck);
		}
	}
#endif

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
	// ReShade 6 builds from before its OpenComposite detection fix request the
	// current IVROverlay interface and create a SteamVR dashboard overlay during
	// effect-runtime initialization. OCU has no SteamVR dashboard to display it
	// on, but aborting (or returning an error) prevents ReShade effects from
	// initializing at all. Give the client one ordinary, hidden overlay as a
	// compatibility handle and omit the thumbnail. Since the overlay is never
	// shown, this allocates no OpenXR swapchain or composition layer.
	if (pMainHandle)
		*pMainHandle = vr::k_ulOverlayHandleInvalid;
	if (pThumbnailHandle)
		*pThumbnailHandle = vr::k_ulOverlayHandleInvalid;

	if (!pchOverlayKey || !pMainHandle)
		return VROverlayError_InvalidParameter;

	const EVROverlayError result = CreateOverlay(
	    pchOverlayKey,
	    pchOverlayFriendlyName ? pchOverlayFriendlyName : pchOverlayKey,
	    pMainHandle);
	if (result == VROverlayError_None) {
		OOVR_LOG_ONCE("SteamVR dashboard overlays are unavailable under OCU; created a hidden compatibility overlay");
	}
	return result;
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
    VRKeyboard::eventDispatch_t eventDispatch, OverlayData* owner)
{
	// OpenVR clients may call this from any thread. SkyUI VR calls it from its
	// keyboard-polling worker, where touching the shared D3D11 immediate context
	// races Skyrim/Streamline and crashes inside nvwgf2umx.dll. Copy the request;
	// _BuildLayers performs every GPU operation on the compositor thread.
	auto queuedRequest = std::make_unique<PendingKeyboardRequest>();
	queuedRequest->inputMode = eInputMode;
	queuedRequest->lineInputMode = eLineInputMode;
	queuedRequest->description = pchDescription ? pchDescription : "";
	queuedRequest->charMax = unCharMax;
	queuedRequest->existingText = pchExistingText ? pchExistingText : "";
	queuedRequest->minimalMode = bUseMinimalMode;
	queuedRequest->userValue = uUserValue;
	queuedRequest->eventDispatch = eventDispatch;
	queuedRequest->owner = owner;

	bool alreadyPending = false;
	{
		std::lock_guard<std::mutex> lock(pendingKeyboardMutex);
		alreadyPending = pendingKeyboardRequest != nullptr;
		if (!alreadyPending)
			pendingKeyboardRequest = std::move(queuedRequest);
	}

	if (alreadyPending) {
		// Never strand the second caller in a paused UI state.
		SubmitPlaceholderKeyboardEvent(VREvent_KeyboardDone, eventDispatch, uUserValue);
		OOVR_LOG("Keyboard request completed without opening: another request is pending");
	} else {
		OOVR_LOGF("Keyboard request queued for compositor thread (callerTid=%lu owner=0x%llX)",
		    (unsigned long)GetCurrentThreadId(),
		    (unsigned long long)(uintptr_t)owner);
	}
	return VROverlayError_None;

#if 0 // Historical unsafe implementation retained for reference; never compile it.
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

	// Clear any dirty D3D11 pipeline state left by overlay rendering (PrismaUI etc.)
	{
		ID3D11Device* clearDev3 = BaseCompositor::dxcomp->GetDevice();
		if (clearDev3 && reinterpret_cast<uintptr_t>(clearDev3) > 0xFFFF) {
			ID3D11DeviceContext* clearCtx3 = nullptr;
			clearDev3->GetImmediateContext(&clearCtx3);
			if (clearCtx3) {
				clearCtx3->ClearState();
				clearCtx3->Flush();
				clearCtx3->Release();
				OOVR_DEBUG_LOG("[KB-DIAG] ClearState()+Flush() before ShowKeyboard");
			}
		}
	}

	ID3D11Device* skDev = BaseCompositor::dxcomp->GetDevice();
	if (!skDev || reinterpret_cast<uintptr_t>(skDev) <= 0xFFFF) {
		SubmitPlaceholderKeyboardEvent(VREvent_KeyboardDone, eventDispatch, uUserValue);
		keyboardCache = "Adventurer";
		return VROverlayError_None;
	}
	try {
		keyboard = make_unique<VRKeyboard>(skDev, uUserValue, unCharMax, bUseMinimalMode, eventDispatch,
		    (VRKeyboard::EGamepadTextInputMode)eInputMode);
		keyboard->contents(VRKeyboard::CHAR_CONV.from_bytes(pchExistingText));
	} catch (const std::exception& e) {
		OOVR_LOGF("Keyboard creation failed (ShowKeyboard): %s", e.what());
		keyboard.reset();
	keyboardOwner = nullptr;
		SubmitPlaceholderKeyboardEvent(VREvent_KeyboardDone, eventDispatch, uUserValue);
		keyboardCache = "Adventurer";
		return VROverlayError_None;
	}
#else
	// No DX11 support — fall back to placeholder
	SubmitPlaceholderKeyboardEvent(VREvent_KeyboardDone, eventDispatch, uUserValue);
	keyboardCache = "Adventurer";
#endif

	return VROverlayError_None;
#endif
}

void BaseOverlay::ProcessPendingOverlayDestroys()
{
	std::vector<OverlayData*> destroys;
	{
		std::lock_guard<std::mutex> lock(pendingOverlayDestroyMutex);
		if (pendingOverlayDestroys.empty())
			return;
		destroys.swap(pendingOverlayDestroys);
	}

	for (OverlayData* overlay : destroys) {
		// A not-yet-created keyboard request may target this overlay. Complete it
		// while the dispatch target still exists, then discard the request.
		std::unique_ptr<PendingKeyboardRequest> canceledRequest;
		{
			std::lock_guard<std::mutex> lock(pendingKeyboardMutex);
			if (pendingKeyboardRequest && pendingKeyboardRequest->owner == overlay)
				canceledRequest = std::move(pendingKeyboardRequest);
		}
		if (canceledRequest) {
			keyboardCache = canceledRequest->existingText;
			SubmitPlaceholderKeyboardEvent(VREvent_KeyboardDone,
			    canceledRequest->eventDispatch, canceledRequest->userValue);
			OOVR_LOGF("Canceled queued keyboard for destroyed overlay '%s'",
			    overlay->key.c_str());
		}

		// VRKeyboard destruction releases its swapchain and D3D resources. This is
		// now guaranteed to run on the same thread as VRKeyboard::Update().
		if (keyboard && keyboardOwner == overlay) {
			OOVR_DEBUG_LOGF("Closing keyboard with owner '%s' on compositor thread (tid=%lu)",
			    overlay->key.c_str(), (unsigned long)GetCurrentThreadId());
			HideKeyboard();
		} else if (keyboardOwner == overlay) {
			keyboardOwner = nullptr;
		}

		if (highQualityOverlay == (VROverlayHandle_t)overlay)
			highQualityOverlay = vr::k_ulOverlayHandleInvalid;

		// destroyQueued prevents a second worker call from entering this queue.
		// Validate by pointer before touching the key in case shutdown already
		// removed the object.
		{
			std::lock_guard<std::mutex> lock(pendingOverlayDestroyMutex);
			if (!validOverlays.count(overlay))
				continue;
			OOVR_DEBUG_LOGF("Destroying overlay on compositor thread: key='%s' overlay=0x%llX",
			    overlay->key.c_str(), (unsigned long long)(uintptr_t)overlay);
			overlays.erase(overlay->key);
			validOverlays.erase(overlay);
			delete overlay;
		}
	}
}

void BaseOverlay::ProcessPendingKeyboardRequest()
{
	std::unique_ptr<PendingKeyboardRequest> request;
	{
		std::lock_guard<std::mutex> lock(pendingKeyboardMutex);
		if (!pendingKeyboardRequest)
			return;
		request = std::move(pendingKeyboardRequest);
	}

	// Do not replace live keyboard GPU resources mid-frame. Completing with the
	// unchanged text releases the requesting UI's input pause safely.
	if (keyboard) {
		keyboardCache = request->existingText;
		SubmitPlaceholderKeyboardEvent(VREvent_KeyboardDone, request->eventDispatch, request->userValue);
		OOVR_LOG("Queued keyboard completed without opening: keyboard already active");
		return;
	}

	if (request->lineInputMode != k_EGamepadTextInputLineModeSingleLine) {
		keyboardCache = request->existingText;
		SubmitPlaceholderKeyboardEvent(VREvent_KeyboardDone, request->eventDispatch, request->userValue);
		OOVR_LOGF("Queued keyboard completed without opening: unsupported line mode %d", request->lineInputMode);
		return;
	}

#if defined(SUPPORT_DX) && defined(SUPPORT_DX11)
	ID3D11Device* skDev = BaseCompositor::dxcomp ? BaseCompositor::dxcomp->GetDevice() : nullptr;
	if (!skDev || reinterpret_cast<uintptr_t>(skDev) <= 0xFFFF) {
		keyboardCache = request->existingText;
		SubmitPlaceholderKeyboardEvent(VREvent_KeyboardDone, request->eventDispatch, request->userValue);
		OOVR_LOG("Queued keyboard completed without opening: D3D11 device unavailable");
		return;
	}

	try {
		keyboard = std::make_unique<VRKeyboard>(skDev, request->userValue, request->charMax,
		    request->minimalMode, request->eventDispatch,
		    (VRKeyboard::EGamepadTextInputMode)request->inputMode);
		keyboard->contents(VRKeyboard::CHAR_CONV.from_bytes(request->existingText));
		keyboardOwner = request->owner;
		OOVR_DEBUG_LOGF("Queued keyboard created on compositor thread (tid=%lu owner=0x%llX)",
		    (unsigned long)GetCurrentThreadId(),
		    (unsigned long long)(uintptr_t)keyboardOwner);
	} catch (const std::exception& e) {
		OOVR_LOGF("Keyboard creation failed on compositor thread: %s", e.what());
		keyboard.reset();
		keyboardOwner = nullptr;
		keyboardCache = request->existingText;
		SubmitPlaceholderKeyboardEvent(VREvent_KeyboardDone, request->eventDispatch, request->userValue);
	}
#else
	keyboardCache = request->existingText;
	SubmitPlaceholderKeyboardEvent(VREvent_KeyboardDone, request->eventDispatch, request->userValue);
#endif
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
	bool bUseMinimalMode = (unFlags & 1) != 0;
	return ShowKeyboard(eInputMode, eLineInputMode, pchDescription, unCharMax, pchExistingText, bUseMinimalMode, uUserValue);
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

	return ShowKeyboardWithDispatch(eInputMode, eLineInputMode, pchDescription,
	    unCharMax, pchExistingText, bUseMinimalMode, uUserValue, dispatch, overlay);
}
EVROverlayError BaseOverlay::ShowKeyboardForOverlay(VROverlayHandle_t ulOverlayHandle, EGamepadTextInputMode eInputMode,
    EGamepadTextInputLineMode eLineInputMode, uint32_t unFlags, const char* pchDescription, uint32_t unCharMax,
    const char* pchExistingText, uint64_t uUserValue)
{
	USEH();

	VRKeyboard::eventDispatch_t dispatch = [overlay](VREvent_t ev) {
		std::lock_guard<std::mutex> lock(overlay->eventMutex);
		overlay->eventQueue.push(ev);
	};

	bool bUseMinimalMode = (unFlags & 1) != 0;
	return ShowKeyboardWithDispatch(eInputMode, eLineInputMode, pchDescription,
	    unCharMax, pchExistingText, bUseMinimalMode, uUserValue, dispatch, overlay);
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
		OOVR_DEBUG_LOGF("HideKeyboard: caching contents (%zu chars)", keyboard->contents().size());
		keyboardCache = VRKeyboard::CHAR_CONV.to_bytes(keyboard->contents());
	} else {
		OOVR_DEBUG_LOG("HideKeyboard: keyboard already null");
	}

	// Check device pointer health before and after destruction
	{
		ID3D11Device* preDestroyDev = BaseCompositor::dxcomp ? BaseCompositor::dxcomp->GetDevice() : nullptr;
		OOVR_DEBUG_LOGF("[KB-DIAG] HideKeyboard BEFORE destroy: dxcomp=0x%llX GetDevice()=0x%llX",
		    (unsigned long long)(uintptr_t)BaseCompositor::dxcomp,
		    (unsigned long long)(uintptr_t)preDestroyDev);
	}

	// Delete the keyboard instance
	keyboard.reset();
	keyboardOwner = nullptr;

	{
		ID3D11Device* postDestroyDev = BaseCompositor::dxcomp ? BaseCompositor::dxcomp->GetDevice() : nullptr;
		OOVR_DEBUG_LOGF("[KB-DIAG] HideKeyboard AFTER destroy: dxcomp=0x%llX GetDevice()=0x%llX",
		    (unsigned long long)(uintptr_t)BaseCompositor::dxcomp,
		    (unsigned long long)(uintptr_t)postDestroyDev);
	}
	OOVR_DEBUG_LOG("HideKeyboard: keyboard instance destroyed");
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
