#include "stdafx.h"

#include "VRKeyboard.h"

#include <d3d11.h>

#include "Reimpl/BaseCompositor.h"
#include "Reimpl/BaseInput.h"
#include "Reimpl/BaseSystem.h"
#include "generated/static_bases.gen.h"

#include "Misc/ScopeGuard.h"
#include "convert.h"

#include "resources.h"

#include "LaserTextureAtlas.h"
#include "LaserRaySmoothing.h"
#include "Misc/Config.h"
#include "Misc/LaserCalibration.h"
#include "Misc/lodepng.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <stdexcept>
#include <utility>
#include <vector>

// MCI for MP3 sound playback
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

// Input threads read this publication instead of inspecting the render-owned
// keyboard pointer or guessing which desktop window owns its properties.
std::atomic<bool> g_ocuKeyboardActive{false};

// Persistent keyboard settings — survive keyboard close/reopen and game restarts
// Note: These are written by file watcher thread and read by render thread.
// Race condition is acceptable - worst case is momentary volume/haptic glitch
// that self-corrects on next frame. Full mutex would add overhead on every frame.
static float s_tiltDegrees = 22.5f;
static float s_lastYaw = 0.0f; // stored yaw for tilt adjustments
static int s_opacityPercent = 30; // parchment background opacity (1-100)
static int s_scalePercent = 100; // keyboard size scale (50-150%)
static bool s_soundsEnabled = true; // keyboard sounds on/off
static int s_hoverVolume = 50;       // hover sound volume 0-100%
static int s_pressVolume = 50;       // press sound volume 0-100%
static int s_hapticStrength = 50;    // haptic strength 0-100%
static bool s_settingsLoaded = false;
// Note: WASD+E blocking removed - keys are now remapped by SKSE plugin (OpenCompositeInput)

// Space bar image
static std::vector<unsigned char> s_spaceBarImage;
static unsigned int s_spaceBarWidth = 0;
static unsigned int s_spaceBarHeight = 0;
static bool s_spaceBarLoaded = false;

// ── Selectable keyboard themes ──
// Each theme = background PNG + font atlas + palette. Selected by `theme=` in [keyboard].
// Colors are RGBA. Blit() hard-writes pixels (no blending), so labelOutline stamps the
// glyph at 8 offsets in outline[] before the ink pass — a crisp halo, not a soft glow.
struct KbThemeDef {
	const char* name;
	int bgRes;   // RES_O_BG_*
	int fontRes; // RES_O_FNT_*
	bool opacityInkFlip; // flip ink to white when bg opacity <= 5% (parchment behavior)
	bool labelOutline;   // stamp outline[] behind key labels
	uint8_t outline[4];
	uint8_t ink[4];          // key label / margin control ink
	uint8_t inkHi[4];        // label on active shift/caps key
	uint8_t inkSel[4];       // label on hovered/selected key
	uint8_t keyBorder[4];    // 1px key border
	uint8_t keyFillHi[4];    // active shift/caps key fill
	uint8_t keyFillSel[4];   // hovered/selected key fill
	uint8_t btnBorder[4];    // MODE/LOCK button border
	uint8_t btnFillIdle[4];
	uint8_t btnFillHover[4];
	uint8_t btnFillActive[4];
	uint8_t btnInkIdle[4];
	uint8_t btnInkHover[4];
	uint8_t btnInkActive[4];
	uint8_t hoverPlate[4];   // backplate behind hovered margin arrows
	uint8_t arrowHover[4];   // margin arrow hover color
	uint8_t textBarBorder[4];
	uint8_t consoleBg[4];
	uint8_t consoleBorder[4];
	uint8_t consoleInk[4];
	bool tintSpacebar; // recolor the spacebar scribble to ink[] (for dark backgrounds)
	int spacebarRes;   // RES_O_SPACEBAR* image for the space key
	// Pixel nudges aligning drawn controls with background art (draw + hit-test)
	int modeBtnOffX, modeBtnOffY;
	int lockBtnOffX, lockBtnOffY;
	// Modern themes reuse the dark SkyUI panel but draw Configurator-style
	// rounded keys and a soft accent glow in the runtime pixel buffer.
	bool modernKeys;
	uint8_t keyFillIdle[4];
	uint8_t keyGlow[4];
};

static KbThemeDef MakeModernTheme(const char* name,
    uint8_t accentR, uint8_t accentG, uint8_t accentB,
    uint8_t brightR, uint8_t brightG, uint8_t brightB)
{
	KbThemeDef t{};
	auto rgba = [](uint8_t (&dst)[4], uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
		dst[0] = r;
		dst[1] = g;
		dst[2] = b;
		dst[3] = a;
	};

	t.name = name;
	t.bgRes = RES_O_BG_SKYUI;
	t.fontRes = RES_O_FNT_UBUNTU;
	t.opacityInkFlip = false;
	t.labelOutline = true;
	rgba(t.outline, 8, 11, 15, 230);
	rgba(t.ink, 237, 240, 245, 255);
	rgba(t.inkHi, 255, 255, 255, 255);
	rgba(t.inkSel, 255, 255, 255, 255);
	rgba(t.keyBorder, accentR, accentG, accentB, 205);
	rgba(t.keyFillHi, accentR, accentG, accentB, 105);
	rgba(t.keyFillSel, brightR, brightG, brightB, 95);
	rgba(t.btnBorder, accentR, accentG, accentB, 210);
	rgba(t.btnFillIdle, 22, 26, 33, 205);
	rgba(t.btnFillHover, accentR, accentG, accentB, 90);
	rgba(t.btnFillActive, accentR, accentG, accentB, 115);
	rgba(t.btnInkIdle, 237, 240, 245, 255);
	rgba(t.btnInkHover, 255, 255, 255, 255);
	rgba(t.btnInkActive, 255, 255, 255, 255);
	rgba(t.hoverPlate, accentR, accentG, accentB, 80);
	rgba(t.arrowHover, brightR, brightG, brightB, 255);
	rgba(t.textBarBorder, accentR, accentG, accentB, 175);
	rgba(t.consoleBg, 14, 17, 22, 235);
	rgba(t.consoleBorder, accentR, accentG, accentB, 190);
	rgba(t.consoleInk, 237, 240, 245, 255);
	t.tintSpacebar = true;
	t.spacebarRes = RES_O_SPACEBAR;
	t.modernKeys = true;
	rgba(t.keyFillIdle, 22, 26, 33, 175);
	rgba(t.keyGlow, brightR, brightG, brightB, 48);
	return t;
}

static const KbThemeDef K_THEMES[] = {
	{
	    // Faithful to the original hardcoded parchment palette
	    .name = "parchment",
	    .bgRes = RES_O_BG_PARCHMENT,
	    .fontRes = RES_O_FNT_PARCHMENT,
	    .opacityInkFlip = true,
	    .labelOutline = false,
	    .outline = { 0, 0, 0, 0 },
	    .ink = { 0, 0, 0, 255 },
	    .inkHi = { 200, 180, 140, 255 },
	    .inkSel = { 220, 200, 160, 255 },
	    .keyBorder = { 80, 55, 25, 40 },
	    .keyFillHi = { 120, 80, 20, 80 },
	    .keyFillSel = { 60, 35, 10, 100 },
	    .btnBorder = { 80, 55, 25, 60 },
	    .btnFillIdle = { 60, 40, 20, 30 },
	    .btnFillHover = { 40, 25, 10, 110 },
	    .btnFillActive = { 100, 70, 20, 80 },
	    .btnInkIdle = { 60, 35, 10, 255 },
	    .btnInkHover = { 25, 13, 2, 255 },
	    .btnInkActive = { 25, 13, 2, 255 },
	    .hoverPlate = { 40, 25, 10, 80 },
	    .arrowHover = { 40, 40, 40, 255 },
	    .textBarBorder = { 80, 55, 25, 100 },
	    .consoleBg = { 220, 195, 160, 200 },
	    .consoleBorder = { 60, 40, 20, 220 },
	    .consoleInk = { 30, 15, 5, 255 },
	    .tintSpacebar = false,
	    .spacebarRes = RES_O_SPACEBAR,
	},
	{
	    // SkyUI — translucent black panel, clean font, white ink, pale-blue selection
	    .name = "skyui",
	    .bgRes = RES_O_BG_SKYUI,
	    .fontRes = RES_O_FNT_UBUNTU,
	    .opacityInkFlip = false,
	    .labelOutline = false,
	    .outline = { 0, 0, 0, 0 },
	    .ink = { 235, 235, 235, 255 },
	    .inkHi = { 255, 255, 255, 255 },
	    .inkSel = { 255, 255, 255, 255 },
	    .keyBorder = { 255, 255, 255, 30 },
	    .keyFillHi = { 130, 195, 255, 70 },
	    .keyFillSel = { 255, 255, 255, 48 },
	    .btnBorder = { 255, 255, 255, 60 },
	    .btnFillIdle = { 255, 255, 255, 18 },
	    .btnFillHover = { 255, 255, 255, 55 },
	    .btnFillActive = { 130, 195, 255, 90 },
	    .btnInkIdle = { 235, 235, 235, 255 },
	    .btnInkHover = { 255, 255, 255, 255 },
	    .btnInkActive = { 255, 255, 255, 255 },
	    .hoverPlate = { 255, 255, 255, 40 },
	    .arrowHover = { 255, 255, 255, 255 },
	    .textBarBorder = { 255, 255, 255, 70 },
	    .consoleBg = { 12, 12, 12, 215 },
	    .consoleBorder = { 255, 255, 255, 80 },
	    .consoleInk = { 235, 235, 235, 255 },
	    .tintSpacebar = true,
	    .spacebarRes = RES_O_SPACEBAR,
	},
	{
	    // Dwemer — bronze frame art, pale glowing-cyan letters with dark-teal outline
	    .name = "dwemer",
	    .bgRes = RES_O_BG_DWEMER,
	    .fontRes = RES_O_FNT_MEDIEVAL,
	    .opacityInkFlip = false,
	    .labelOutline = true,
	    .outline = { 12, 45, 38, 255 },
	    .ink = { 210, 255, 240, 255 },
	    .inkHi = { 255, 255, 255, 255 },
	    .inkSel = { 255, 255, 255, 255 },
	    .keyBorder = { 190, 140, 70, 90 },
	    .keyFillHi = { 190, 140, 70, 90 },
	    .keyFillSel = { 120, 255, 220, 45 },
	    .btnBorder = { 190, 140, 70, 0 },
	    .btnFillIdle = { 60, 45, 25, 0 },
	    .btnFillHover = { 190, 140, 70, 80 },
	    .btnFillActive = { 190, 140, 70, 120 },
	    .btnInkIdle = { 210, 255, 240, 255 },
	    .btnInkHover = { 30, 22, 12, 255 },
	    .btnInkActive = { 30, 22, 12, 255 },
	    .hoverPlate = { 190, 140, 70, 70 },
	    .arrowHover = { 255, 255, 255, 255 },
	    .textBarBorder = { 190, 140, 70, 0 },
	    .consoleBg = { 38, 30, 20, 225 },
	    .consoleBorder = { 190, 140, 70, 220 },
	    .consoleInk = { 210, 255, 240, 255 },
	    .tintSpacebar = false,
	    .spacebarRes = RES_O_SPACEBAR_DWEMER,
	    .modeBtnOffX = 8,
	    .modeBtnOffY = 6,
	    .lockBtnOffX = -18,
	    .lockBtnOffY = 2,
	},
	{
	    // Sovngarde — night-sky art, always-white letters with dark outline for readability
	    .name = "sovngarde",
	    .bgRes = RES_O_BG_SOVNGARDE,
	    .fontRes = RES_O_FNT_MEDIEVAL,
	    .opacityInkFlip = false,
	    .labelOutline = true,
	    .outline = { 5, 8, 20, 255 },
	    .ink = { 255, 255, 255, 255 },
	    .inkHi = { 255, 255, 255, 255 },
	    .inkSel = { 255, 255, 255, 255 },
	    .keyBorder = { 255, 255, 255, 35 },
	    .keyFillHi = { 255, 255, 255, 65 },
	    .keyFillSel = { 255, 255, 255, 50 },
	    .btnBorder = { 255, 255, 255, 70 },
	    .btnFillIdle = { 255, 255, 255, 20 },
	    .btnFillHover = { 255, 255, 255, 60 },
	    .btnFillActive = { 255, 255, 255, 95 },
	    .btnInkIdle = { 255, 255, 255, 255 },
	    .btnInkHover = { 15, 18, 35, 255 },
	    .btnInkActive = { 15, 18, 35, 255 },
	    .hoverPlate = { 255, 255, 255, 45 },
	    .arrowHover = { 210, 220, 255, 255 },
	    .textBarBorder = { 255, 255, 255, 75 },
	    .consoleBg = { 10, 14, 30, 215 },
	    .consoleBorder = { 255, 255, 255, 85 },
	    .consoleInk = { 255, 255, 255, 255 },
	    .tintSpacebar = true,
	    .spacebarRes = RES_O_SPACEBAR,
	},
	MakeModernTheme("modern_green", 62, 190, 143, 132, 242, 158),
	MakeModernTheme("modern_white", 190, 205, 220, 255, 255, 255),
	MakeModernTheme("modern_blue", 60, 150, 245, 115, 205, 255),
	MakeModernTheme("modern_amber", 218, 145, 55, 255, 205, 115),
	MakeModernTheme("modern_purple", 155, 95, 230, 215, 165, 255),
};

struct KbFontDef {
	const char* name;
	int resource;
};

static const KbFontDef K_FONTS[] = {
	{ "ubuntu", RES_O_FNT_UBUNTU },
	{ "parchment", RES_O_FNT_PARCHMENT },
	{ "medieval", RES_O_FNT_MEDIEVAL },
	{ "ocu_nordic", RES_O_FNT_OCU_NORDIC },
	{ "ocu_unease", RES_O_FNT_OCU_UNEASE },
	{ "cyrodiil", RES_O_FNT_CYRODIIL },
};

// Expands a theme RGBA array into fillArea's (r, g, b, a) argument list
#define KBT4(f) T.f[0], T.f[1], T.f[2], T.f[3]

// Get the directory where the DLL lives (for config file and sounds)
static std::wstring GetOCDllDirectory()
{
	wchar_t path[MAX_PATH] = {};

	// Get the openvr_api.dll module directly by name
	HMODULE hm = GetModuleHandleW(L"openvr_api.dll");
	if (!hm) {
		OOVR_LOGF("Failed to get openvr_api.dll module handle");
		return L"";
	}

	DWORD len = GetModuleFileNameW(hm, path, MAX_PATH);
	if (len == 0) {
		OOVR_LOGF("GetModuleFileNameW failed");
		return L"";
	}

	OOVR_DEBUG_LOGF("OpenComposite DLL loaded from: %S", path);

	std::wstring dir(path);
	auto pos = dir.find_last_of(L"\\/");
	if (pos != std::wstring::npos)
		dir = dir.substr(0, pos + 1);

	OOVR_DEBUG_LOGF("OpenComposite DLL directory: %S", dir.c_str());
	return dir;
}

// Sound and haptic feedback
static int s_lastHoveredKey[2] = { -1, -1 }; // track last hovered key per hand
static int s_lastHoveredArrow[2] = { 0, 0 }; // track last hovered arrow per hand (0=none, -6 to -11)
static int s_pressedKey[2] = { -1, -1 };     // track currently pressed key per hand (trigger held)
static bool s_soundsInitialized = false;
static std::vector<char> s_hoverSoundData;
static std::vector<char> s_pressSoundData;

// Target mode (persists across keyboard open/close)
static bool s_targetMode = false;

// After a local tilde toggle, hold off the bridge console-state sync briefly:
// the console request needs a few game frames before MenuOpenCloseEvent
// updates the bridge, and syncing inside that window would undo the toggle.
static ULONGLONG s_consoleToggleGraceUntil = 0;

// Custom message the SKSE plugin handles (same id as the keyboard-done signal).
// wParam 2 = show game console, 3 = hide game console, via the UI queue.
// Idempotent show/hide replaces injected tilde keystrokes, which double-toggled
// on setups where DirectInput and message-loop mods both saw the keystroke.
static constexpr UINT WM_OC_KB_MSG = WM_APP + 0x4F43;

// Ask the SKSE plugin to show/hide the console. Returns false when the game
// window is unavailable (no plugin path); caller falls back to keystrokes.
static HWND GetGameWindow();
static bool RequestGameConsole(bool show)
{
	HWND hwnd = GetGameWindow();
	if (!hwnd)
		return false;
	PostMessageW(hwnd, WM_OC_KB_MSG, show ? 2 : 3, 0);
	return true;
}

// Sound functions moved after loadResource() definition (see below)

static void TriggerHaptic(int side)
{
	if (s_hapticStrength <= 0) {
		OOVR_DEBUG_LOGF("Haptic blocked: strength=%d", s_hapticStrength);
		return;
	}
	auto system = GetBaseSystem();
	auto input = GetBaseInput();
	if (!system || !input) {
		OOVR_LOG_LIMITEDF(5000, "Haptic blocked: no BaseSystem or BaseInput");
		return;
	}
	vr::ETrackedControllerRole role = (side == 0)
		? vr::TrackedControllerRole_LeftHand
		: vr::TrackedControllerRole_RightHand;
	vr::TrackedDeviceIndex_t deviceIndex = system->GetTrackedDeviceIndexForControllerRole(role);
	if (deviceIndex == vr::k_unTrackedDeviceIndexInvalid) {
		OOVR_LOG_LIMITEDF(5000, "Haptic blocked: invalid device index for side %d", side);
		return;
	}
	// Scale haptic duration: 0-100% maps to 0-3000 microseconds
	unsigned short duration = (unsigned short)(s_hapticStrength * 30);
	float amplitude = s_hapticStrength / 100.0f;
	OOVR_DEBUG_LOGF("Triggering haptic: side=%d, strength=%d, duration=%d, amplitude=%.2f", side, s_hapticStrength, duration, amplitude);
	// Call BaseInput directly — bypasses global Haptics() check so keyboard
	// haptics work even when in-game haptics are disabled
	input->TriggerLegacyHapticPulse(deviceIndex, (uint64_t)duration * 1000, amplitude);
}

// Update a key=value in opencomposite.ini [keyboard] section without corrupting other settings
static void UpdateIniKey(const std::wstring& iniPath, const char* key, const char* value)
{
	FILE* f = _wfopen(iniPath.c_str(), L"r");
	if (!f) return;

	std::vector<std::string> lines;
	char buf[512];
	while (fgets(buf, sizeof(buf), f))
		lines.push_back(buf);
	fclose(f);

	bool inKeyboardSection = false;
	bool keyFound = false;
	std::string keyPrefix = std::string(key) + "=";
	std::string newLine = keyPrefix + value + "\n";
	int keyboardSectionEnd = -1;

	for (size_t i = 0; i < lines.size(); i++) {
		std::string& line = lines[i];
		if (line.size() > 0 && line[0] == '[') {
			if (inKeyboardSection) {
				keyboardSectionEnd = (int)i;
				inKeyboardSection = false;
			}
			if (line.find("[keyboard]") != std::string::npos)
				inKeyboardSection = true;
		}
		if (inKeyboardSection && line.find(keyPrefix) == 0) {
			lines[i] = newLine;
			keyFound = true;
		}
	}
	if (inKeyboardSection)
		keyboardSectionEnd = (int)lines.size();

	if (!keyFound && keyboardSectionEnd >= 0) {
		lines.insert(lines.begin() + keyboardSectionEnd, newLine);
	} else if (!keyFound) {
		lines.push_back("\n[keyboard]\n");
		lines.push_back(newLine);
	}

	f = _wfopen(iniPath.c_str(), L"w");
	if (f) {
		for (const auto& line : lines)
			fputs(line.c_str(), f);
		fclose(f);
	}
}

// Two-handed pinch scale: while one hand grab-drags, the second trigger on the grab
// bar enters pinch mode — hand separation scales the keyboard (same 50-150% value as
// the size arrows and Configurator).
static bool s_pinchActive = false;
static float s_pinchBaseDist = 0.0f;
static int s_pinchBaseScale = 100;

// Sticky spawn position: head-relative offset persisted on grab release. Head-relative
// (not world-anchored) so the keyboard always opens within reach — just where the user
// last parked it. Clamps make even a hand-edited ini un-strandable.
static float s_posForward = 0.80f; // meters in front of head
static float s_posDown = 0.52f;    // meters below head
static float s_posRight = 0.0f;    // meters to the right of head

static void SaveKeyboardPosition()
{
	std::wstring path = GetOCDllDirectory() + L"opencomposite.ini";
	char buf[32];
	snprintf(buf, sizeof(buf), "%.2f", s_posForward);
	UpdateIniKey(path, "positionForward", buf);
	snprintf(buf, sizeof(buf), "%.2f", s_posDown);
	UpdateIniKey(path, "positionDown", buf);
	snprintf(buf, sizeof(buf), "%.2f", s_posRight);
	UpdateIniKey(path, "positionRight", buf);
}

static void SaveKeyboardSettings()
{
	std::wstring path = GetOCDllDirectory() + L"opencomposite.ini";
	char buf[32];
	snprintf(buf, sizeof(buf), "%.1f", s_tiltDegrees);
	UpdateIniKey(path, "displayTilt", buf);
	snprintf(buf, sizeof(buf), "%d", s_opacityPercent);
	UpdateIniKey(path, "displayOpacity", buf);
	snprintf(buf, sizeof(buf), "%d", s_scalePercent);
	UpdateIniKey(path, "displayScale", buf);
	UpdateIniKey(path, "soundsEnabled", s_soundsEnabled ? "true" : "false");
	snprintf(buf, sizeof(buf), "%d", s_hoverVolume);
	UpdateIniKey(path, "hoverVolume", buf);
	snprintf(buf, sizeof(buf), "%d", s_pressVolume);
	UpdateIniKey(path, "pressVolume", buf);
	snprintf(buf, sizeof(buf), "%d", s_hapticStrength);
	UpdateIniKey(path, "hapticStrength", buf);
}

static bool ReloadKeyboardSettings();

static void LoadKeyboardSettings()
{
	if (s_settingsLoaded)
		return;
	s_settingsLoaded = true;

	// Load from opencomposite.ini via the Config system (already parsed at startup)
	s_tiltDegrees = oovr_global_configuration.KbDisplayTilt();
	s_opacityPercent = oovr_global_configuration.KbDisplayOpacity();
	s_scalePercent = oovr_global_configuration.KbDisplayScale();
	s_soundsEnabled = oovr_global_configuration.KbSoundsEnabled();
	// Note: hoverVolume and pressVolume will be loaded by periodic ReloadKeyboardSettings()
	// within 1 second, so we just use defaults here
	s_hoverVolume = 50;
	s_pressVolume = 50;
	s_hapticStrength = oovr_global_configuration.KbHapticStrength();

	// Clamp to valid ranges
	if (s_tiltDegrees < -30.0f) s_tiltDegrees = -30.0f;
	if (s_tiltDegrees > 80.0f) s_tiltDegrees = 80.0f;
	if (s_opacityPercent < 1) s_opacityPercent = 1;
	if (s_opacityPercent > 100) s_opacityPercent = 100;
	if (s_scalePercent < 50) s_scalePercent = 50;
	if (s_scalePercent > 150) s_scalePercent = 150;
	if (s_hoverVolume < 0) s_hoverVolume = 0;
	if (s_hoverVolume > 100) s_hoverVolume = 100;
	if (s_pressVolume < 0) s_pressVolume = 0;
	if (s_pressVolume > 100) s_pressVolume = 100;
	if (s_hapticStrength < 0) s_hapticStrength = 0;
	if (s_hapticStrength > 100) s_hapticStrength = 100;

	// Pick up [keyboard] keys not mirrored in Config (volumes, sticky position)
	ReloadKeyboardSettings();
}

// Re-read opencomposite.ini [keyboard] section (called when file changes externally)
static bool ReloadKeyboardSettings()
{
	std::wstring path = GetOCDllDirectory() + L"opencomposite.ini";

	// Retry file open with short delays to handle file locking from configurator
	FILE* f = nullptr;
	for (int retry = 0; retry < 5 && !f; retry++) {
		f = _wfopen(path.c_str(), L"r");
		if (!f && retry < 4) {
			Sleep(50); // Wait 50ms before retry
		}
	}
	if (!f)
		return false;

	float newTilt = s_tiltDegrees;
	int newOpacity = s_opacityPercent;
	int newScale = s_scalePercent;
	bool newSounds = s_soundsEnabled;
	int newHoverVol = s_hoverVolume;
	int newPressVol = s_pressVolume;
	int newHaptic = s_hapticStrength;
	float newPosF = s_posForward, newPosD = s_posDown, newPosR = s_posRight;
	bool inKeyboardSection = false;
	bool inDefaultSection = true;  // starts in default section (before any [header])
	char line[256];

	while (fgets(line, sizeof(line), f)) {
		if (line[0] == '[') {
			inKeyboardSection = (strstr(line, "[keyboard]") != nullptr);
			inDefaultSection = false;
			continue;
		}

		float val;
		int ival;
		char sval[32];
		char layoutValue[128];

		if (inKeyboardSection) {
			if (sscanf(line, "displayTilt=%f", &val) == 1)
				newTilt = val;
			if (sscanf(line, "displayOpacity=%d", &ival) == 1)
				newOpacity = ival;
			if (sscanf(line, "displayScale=%d", &ival) == 1)
				newScale = ival;
			if (sscanf(line, "soundsEnabled=%31s", sval) == 1)
				newSounds = (strcmp(sval, "true") == 0 || strcmp(sval, "1") == 0);
			if (sscanf(line, "hoverVolume=%d", &ival) == 1)
				newHoverVol = ival;
			if (sscanf(line, "pressVolume=%d", &ival) == 1)
				newPressVol = ival;
			if (sscanf(line, "hapticStrength=%d", &ival) == 1)
				newHaptic = ival;
			if (sscanf(line, "positionForward=%f", &val) == 1)
				newPosF = val;
			if (sscanf(line, "positionDown=%f", &val) == 1)
				newPosD = val;
			if (sscanf(line, "positionRight=%f", &val) == 1)
				newPosR = val;
			if (sscanf(line, "theme=%31s", sval) == 1)
				oovr_global_configuration.kbTheme = sval; // picked up by Update()'s theme check
			if (sscanf(line, "font=%31s", sval) == 1)
				oovr_global_configuration.kbFont = sval; // picked up by Update()'s asset check
			if (sscanf(line, "layout=%127s", layoutValue) == 1)
				oovr_global_configuration.kbLayout = layoutValue; // picked up by Update()'s layout check
		}

		// Hot-reload ASW tuning values (in default section of ini)
		if (inDefaultSection) {
			if (sscanf(line, "aswWarpStrength=%f", &val) == 1)
				oovr_global_configuration.aswWarpStrength = val;
			if (sscanf(line, "aswRotationScale=%f", &val) == 1)
				oovr_global_configuration.aswRotationScale = val;
			if (sscanf(line, "aswTranslationScale=%f", &val) == 1)
				oovr_global_configuration.aswTranslationScale = val;
			if (sscanf(line, "aswDepthScale=%f", &val) == 1)
				oovr_global_configuration.aswDepthScale = val;
			if (sscanf(line, "aswEdgeFadeWidth=%f", &val) == 1)
				oovr_global_configuration.aswEdgeFadeWidth = val;
			if (sscanf(line, "aswNearFadeDepth=%f", &val) == 1)
				oovr_global_configuration.aswNearFadeDepth = val;
		}
	}
	fclose(f);

	if (newTilt < -30.0f) newTilt = -30.0f;
	if (newTilt > 80.0f) newTilt = 80.0f;
	if (newOpacity < 1) newOpacity = 1;
	if (newOpacity > 100) newOpacity = 100;
	if (newScale < 50) newScale = 50;
	if (newScale > 150) newScale = 150;
	if (newHoverVol < 0) newHoverVol = 0;
	if (newHoverVol > 100) newHoverVol = 100;
	if (newPressVol < 0) newPressVol = 0;
	if (newPressVol > 100) newPressVol = 100;
	if (newHaptic < 0) newHaptic = 0;
	if (newHaptic > 100) newHaptic = 100;
	// Position clamps: keyboard must always spawn within reach
	if (newPosF < 0.3f) newPosF = 0.3f;
	if (newPosF > 1.5f) newPosF = 1.5f;
	if (newPosD < -0.3f) newPosD = -0.3f;
	if (newPosD > 1.2f) newPosD = 1.2f;
	if (newPosR < -1.0f) newPosR = -1.0f;
	if (newPosR > 1.0f) newPosR = 1.0f;
	s_posForward = newPosF;
	s_posDown = newPosD;
	s_posRight = newPosR;

	bool changed = (newTilt != s_tiltDegrees || newOpacity != s_opacityPercent ||
	                newScale != s_scalePercent || newSounds != s_soundsEnabled ||
	                newHoverVol != s_hoverVolume || newPressVol != s_pressVolume ||
	                newHaptic != s_hapticStrength);
	s_tiltDegrees = newTilt;
	s_opacityPercent = newOpacity;
	s_scalePercent = newScale;
	s_soundsEnabled = newSounds;
	s_hoverVolume = newHoverVol;
	s_pressVolume = newPressVol;
	s_hapticStrength = newHaptic;
	return changed;
}

// Quaternion multiply (Hamilton product)
static XrQuaternionf qmul(const XrQuaternionf& a, const XrQuaternionf& b)
{
	return {
		a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
		a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
		a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
		a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z
	};
}

// Build orientation: yaw to face user + pitch tilt (positive tiltDeg = bottom toward player)
static XrQuaternionf buildTiltedOrientation(float yaw, float tiltDeg)
{
	float angle = 3.14159265f + yaw;
	XrQuaternionf yawQ = { 0.0f, sinf(angle * 0.5f), 0.0f, cosf(angle * 0.5f) };
	float pitchRad = -tiltDeg * 3.14159265f / 180.0f; // negative = bottom toward player
	XrQuaternionf pitchQ = { sinf(pitchRad * 0.5f), 0.0f, 0.0f, cosf(pitchRad * 0.5f) };
	return qmul(yawQ, pitchQ); // pitch applied in keyboard's local frame
}

#ifdef _WIN32
#pragma comment(lib, "d3d11.lib")

// for debugging only for now
#include <comdef.h>

// ── Keystroke injection helpers ──
// Uses PostMessage to the game window so keystrokes arrive regardless of
// which desktop window has focus (important in VR where you can't see the taskbar).

struct VkMapping {
	WORD vk;
	bool needsShift;
};

static VkMapping CharToVK(wchar_t ch)
{
	SHORT result = VkKeyScanW(ch);
	if (result == -1)
		return { 0, false };
	WORD vk = LOBYTE(result);
	bool shift = (HIBYTE(result) & 1) != 0;
	return { vk, shift };
}

// Find the game's main window (we're running inside the game process)
static BOOL CALLBACK FindVisibleWindowProc(HWND hwnd, LPARAM lParam)
{
	DWORD windowPid;
	GetWindowThreadProcessId(hwnd, &windowPid);
	auto* finder = reinterpret_cast<std::pair<DWORD, HWND>*>(lParam);
	if (windowPid == finder->first && IsWindowVisible(hwnd)) {
		// Pick the largest visible window (the game render window)
		RECT rc;
		GetClientRect(hwnd, &rc);
		int area = (rc.right - rc.left) * (rc.bottom - rc.top);
		if (area > 0) {
			finder->second = hwnd;
			return FALSE; // found it
		}
	}
	return TRUE;
}

static HWND GetGameWindow()
{
	static HWND cached = nullptr;
	if (cached && IsWindow(cached))
		return cached;

	std::pair<DWORD, HWND> finder = { GetCurrentProcessId(), nullptr };
	EnumWindows(FindVisibleWindowProc, reinterpret_cast<LPARAM>(&finder));
	cached = finder.second;
	return cached;
}

// Check if a virtual key is an "extended" key (arrow keys, nav cluster, etc.)
// Extended keys set bit 24 in lParam — without this, arrows map to numpad keys.
static bool IsExtendedKey(WORD vk)
{
	switch (vk) {
	case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT:
	case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
	case VK_PRIOR: case VK_NEXT: // Page Up / Page Down
		return true;
	default:
		return false;
	}
}

// Build the lParam for WM_KEYDOWN / WM_KEYUP messages
static LPARAM MakeKeyLP(WORD scan, bool isUp, bool extended = false, bool wasDown = false)
{
	LPARAM lp = 1; // repeat count = 1
	lp |= ((LPARAM)scan << 16); // scan code in bits 16-23
	if (extended)
		lp |= (1LL << 24); // extended key flag
	if (isUp) {
		lp |= (1LL << 30); // previous key state = down
		lp |= (1LL << 31); // transition state = releasing
	} else if (wasDown) {
		lp |= (1LL << 30); // previous key state = down (auto-repeat)
	}
	return lp;
}

// Ensure the game window is the foreground window so SendInput reaches it.
static void EnsureGameForeground()
{
	HWND hwnd = GetGameWindow();
	if (!hwnd) return;
	if (GetForegroundWindow() == hwnd) return; // already foreground

	// AttachThreadInput lets us call SetForegroundWindow from a background thread
	DWORD foreThread = GetWindowThreadProcessId(GetForegroundWindow(), NULL);
	DWORD curThread = GetCurrentThreadId();
	if (foreThread != curThread)
		AttachThreadInput(foreThread, curThread, TRUE);
	SetForegroundWindow(hwnd);
	if (foreThread != curThread)
		AttachThreadInput(foreThread, curThread, FALSE);
}

// True when a PrismaVR text input element is currently focused (signalled by
// the OC_PRISMA_TEXT window property, set/cleared by PrismaVR's async focus
// tracking). When set, vkey emissions must NOT flow through Windows SendInput
// — Skyrim should never see the keystroke. Direct-delivery via
// PrismaVR_DeliverChar / PrismaVR_DeliverVKey (invoked from PostCharToGame)
// is the only path the keystroke takes. This is what keeps typing in a
// Prisma chat box from triggering Skyrim's game hotkeys (Inventory, Shout,
// AIAgent's OverlayStatusCycle, etc.).
static bool IsPrismaTextFocused()
{
	HWND hwnd = GetGameWindow();
	return hwnd && GetPropW(hwnd, L"OC_PRISMA_TEXT") != nullptr;
}

// Combined gate used at every SendInput call site. Returns true when vkey
// keystrokes must NOT reach Skyrim's input system (DirectInput / Papyrus).
//
// Note: an earlier revision also gated on OC_MENU_ACTIVE (Scaleform menu
// open) to defend against accidental hotkey rebinds while a Skyrim menu was
// active. That over-corrected — it also blocked INTENTIONAL MCM key-remap
// captures from the vkey, which is a legitimate user flow. The single-
// channel emission fix in SendSingleVK / SendVirtualKey already kills the
// 30Hz dual-fire bug that caused unintended MCM rebinds in the first place,
// so the menu-active gate is no longer needed. Kept the PrismaVR-text gate
// because that's a strict bypass: PostCharToGame direct-delivers to Prisma,
// and Skyrim should genuinely never see those keystrokes.
static bool ShouldSuppressSkyrimInput()
{
	return IsPrismaTextFocused();
}

// Send a virtual key press via Windows SendInput API.
//
// Single-channel emission: VK-based events only. Windows synthesizes the
// matching scancode in the lower-level keyboard input stream automatically
// when SendInput processes a VK event, so DirectInput-listening consumers
// (Skyrim's input system, Papyrus OnKeyDown) still see the press. WM_KEYDOWN
// is posted for Scaleform/menu consumers in the same step.
//
// The previous implementation also sent KEYEVENTF_SCANCODE-only events in
// the same batch as a "belt-and-suspenders" cover for DirectInput. That
// caused double-emit: each logical press fired Skyrim's OnKeyDown twice
// (once from the VK-synthesized scancode, once from the explicit one),
// flooding Papyrus key-handlers (e.g. AIAgent's OverlayStatusCycle hotkey)
// at ~30Hz. With one channel we keep both paths covered without duplication.
//
// Suppression gate: when a Prisma text field is focused, this function is
// a no-op for SendInput — PostCharToGame's OC_PRISMA_TEXT branch direct-
// delivers to PrismaVR via PrismaVR_DeliverVKey and Skyrim never sees the
// keystroke. Outside Prisma focus, SendInput fires normally so the user's
// intentional vkey presses (incl. MCM remap captures) reach Skyrim. The
// single-channel emit pattern below ensures one logical press == one
// OnKeyDown in Papyrus, killing the 30Hz dual-fire bug that previously
// caused MCM remap to bind to the wrong key.
static void SendSingleVK(WORD vk, bool pcMode = false)
{
	// PC MODE always passes through to Skyrim. The user explicitly toggled
	// the VR keyboard to PC mode (sendInputOnly), which is the "I'm sending
	// game input now" intent — same role as a physical PC keyboard. Prisma's
	// text-focus gate only applies in VR MODE, where the VR keyboard is
	// primarily a Prisma typing surface.
	//
	// F1-F12 also bypass even in VR mode: they're game hotkeys (toggle-style
	// — open / close panels, debug menus, etc.), never typing keys, so the
	// gate would only break user-mapped hotkeys (e.g. F8 to toggle a Prisma
	// test panel) without ever helping anyone type.
	const bool isFKey = (vk >= VK_F1 && vk <= VK_F12);

	if (!pcMode && !isFKey && ShouldSuppressSkyrimInput()) {
		OOVR_DEBUG_LOGF("[VKEMIT] SendSingleVK(vk=0x%02X) SUPPRESSED (Prisma focused, VR mode)", vk);
		return;
	}
	OOVR_DEBUG_LOGF("[VKEMIT] SendSingleVK(vk=0x%02X) firing SendInput%s%s",
	    vk,
	    pcMode ? " (PC mode)" : "",
	    isFKey ? " (F-key bypass)" : "");

	WORD scan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
	DWORD flags = (IsExtendedKey(vk) ? KEYEVENTF_EXTENDEDKEY : 0);

	INPUT inputs[2] = {};
	// VK-based down — Windows posts WM_KEYDOWN AND injects scancode
	// into the raw input stream that DirectInput observes.
	inputs[0].type = INPUT_KEYBOARD;
	inputs[0].ki.wVk = vk;
	inputs[0].ki.wScan = scan;
	inputs[0].ki.dwFlags = flags;
	// VK-based up
	inputs[1].type = INPUT_KEYBOARD;
	inputs[1].ki.wVk = vk;
	inputs[1].ki.wScan = scan;
	inputs[1].ki.dwFlags = flags | KEYEVENTF_KEYUP;

	EnsureGameForeground();
	::SendInput(2, inputs, sizeof(INPUT));
}

// Send one half of a real PC MODE key hold. Printable keys use the same
// scancode-only DirectInput path as SendVirtualKey; control/F-keys use VK
// events so Windows also supplies WM_KEYDOWN/WM_KEYUP to menu consumers.
// Shift is kept down for the full lifetime of a shifted printable key.
static void SendPCVirtualKeyState(WORD vk, bool shift, bool scanOnly, bool down)
{
	WORD scan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
	WORD shiftScan = (WORD)MapVirtualKeyW(VK_SHIFT, MAPVK_VK_TO_VSC);
	std::vector<INPUT> inputs;

	auto append = [&](WORD eventVk, WORD eventScan, bool eventScanOnly, bool eventDown) {
		INPUT in = {};
		in.type = INPUT_KEYBOARD;
		in.ki.wVk = eventScanOnly ? 0 : eventVk;
		in.ki.wScan = eventScan;
		in.ki.dwFlags = eventScanOnly ? KEYEVENTF_SCANCODE : 0;
		if (IsExtendedKey(eventVk))
			in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
		if (!eventDown)
			in.ki.dwFlags |= KEYEVENTF_KEYUP;
		inputs.push_back(in);
	};

	// Modifier order matters: Shift down before the key, key up before Shift.
	if (down && shift)
		append(VK_SHIFT, shiftScan, scanOnly, true);
	append(vk, scan, scanOnly, down);
	if (!down && shift)
		append(VK_SHIFT, shiftScan, scanOnly, false);

	OOVR_DEBUG_LOGF("[VKEMIT] PC hold vk=0x%02X %s shift=%d scanOnly=%d",
	    vk, down ? "DOWN" : "UP", shift ? 1 : 0, scanOnly ? 1 : 0);
	EnsureGameForeground();
	UINT sent = ::SendInput((UINT)inputs.size(), inputs.data(), sizeof(INPUT));
	if (sent != inputs.size())
		OOVR_LOGF("[VKEMIT] PC hold SendInput sent %u/%u events (error=%lu)",
		    sent, (unsigned)inputs.size(), GetLastError());
}

// Post a character to the SKSE plugin (OpenCompositeInput) for direct
// Scaleform injection.  The SKSE plugin's WndProc hook catches this custom
// message and pushes a GFxCharEvent into the active Scaleform movie,
// completely bypassing the game's broken message loop / TranslateMessage.
static constexpr UINT WM_OC_CHAR = WM_APP + 0x4F45;

// Direct-delivery function exported by PrismaUI.dll when PrismaVR is active.
// When a Prisma text input is focused (signalled via OC_PRISMA_TEXT window
// property), we call this instead of PostMessageW. This:
//   1. Bypasses the Windows message loop entirely — Skyrim never sees the
//      keystroke → no game hotkeys triggered (no more accidental inventory
//      opens, tab-to-menu, etc. when typing in a Prisma chat box).
//   2. Avoids all WndProc subclass chain contention (Dekana's IME refactor,
//      other mods, Skyrim's own WndProc ordering).
//   3. Delivers a properly-constructed Ultralight KeyEvent directly to the
//      focused view, matching Prisma's canonical flatscreen recipe.
// Resolved lazily once per process via GetProcAddress. Null if PrismaUI.dll
// is not loaded (non-Prisma scenarios) or is too old to export the symbol.
typedef void (*PrismaVR_DeliverCharFn)(wchar_t);
typedef void (*PrismaVR_DeliverVKeyFn)(int);

// Resolves both PrismaVR exports once per process. Split into two getters
// so each symbol can be missing independently (if PrismaUI.dll is older and
// only exports one of them — e.g., has DeliverChar but not DeliverVKey).
static PrismaVR_DeliverCharFn GetPrismaVRDeliverChar()
{
	static PrismaVR_DeliverCharFn cached = nullptr;
	static bool resolved = false;
	if (!resolved) {
		HMODULE hPrisma = GetModuleHandleW(L"PrismaUI.dll");
		if (hPrisma) {
			cached = reinterpret_cast<PrismaVR_DeliverCharFn>(
				GetProcAddress(hPrisma, "PrismaVR_DeliverChar"));
		}
		resolved = true;
		OOVR_DEBUG_LOGF("PrismaVR_DeliverChar resolution: hPrisma=0x%llX, fn=0x%llX",
			(unsigned long long)(uintptr_t)hPrisma,
			(unsigned long long)(uintptr_t)cached);
	}
	return cached;
}

static PrismaVR_DeliverVKeyFn GetPrismaVRDeliverVKey()
{
	static PrismaVR_DeliverVKeyFn cached = nullptr;
	static bool resolved = false;
	if (!resolved) {
		HMODULE hPrisma = GetModuleHandleW(L"PrismaUI.dll");
		if (hPrisma) {
			cached = reinterpret_cast<PrismaVR_DeliverVKeyFn>(
				GetProcAddress(hPrisma, "PrismaVR_DeliverVKey"));
		}
		resolved = true;
		OOVR_DEBUG_LOGF("PrismaVR_DeliverVKey resolution: hPrisma=0x%llX, fn=0x%llX",
			(unsigned long long)(uintptr_t)hPrisma,
			(unsigned long long)(uintptr_t)cached);
	}
	return cached;
}

// lParam: 0 = GFxCharEvent (printable chars), 1 = GFxKeyEvent kKeyDown (control keys)
static void PostCharToGame(wchar_t ch, LPARAM mode = 0)
{
	HWND hwnd = GetGameWindow();
	if (!hwnd) return;

	// PrismaVR direct-delivery path: used when a Prisma text input has focus
	// (indicated by the OC_PRISMA_TEXT window property, which PrismaVR sets/clears
	// via its async focus tracking). Bypasses Windows messages entirely so Skyrim
	// never sees the keystroke and no game hotkeys are triggered.
	if (GetPropW(hwnd, L"OC_PRISMA_TEXT")) {
		if (mode == 0) {
			// Printable character (mode 0): use DeliverChar.
			PrismaVR_DeliverCharFn deliverChar = GetPrismaVRDeliverChar();
			if (deliverChar) {
				deliverChar(ch);
				return;
			}
		} else if (mode == 1) {
			// Control key (mode 1): Backspace, Arrow keys, Delete, Enter, Tab, etc.
			// Use DeliverVKey which populates the key_identifier string Chromium
			// needs to recognize these as navigation actions inside <input> fields.
			PrismaVR_DeliverVKeyFn deliverVKey = GetPrismaVRDeliverVKey();
			if (deliverVKey) {
				deliverVKey(static_cast<int>(ch));
				return;
			}
		}
		// Fall-through: PrismaUI.dll not loaded or too old — use the legacy
		// WM_OC_CHAR PostMessage path. PrismaVR_KBSubclassProc on the other side
		// also has a key_identifier-aware fallback for this path.
	}

	PostMessageW(hwnd, WM_OC_CHAR, (WPARAM)ch, mode);
}

// Check if a character is a dangerous action key that could cause crashes
// when sent to the game during text input mode (enchanting, renaming, etc.)
static bool IsActionKey(wchar_t ch)
{
	// Convert to uppercase for comparison
	wchar_t upper = towupper(ch);

	// Movement keys
	if (upper == L'W' || upper == L'A' || upper == L'S' || upper == L'D')
		return true;

	// Combat/action keys
	if (upper == L'R' || upper == L'Z' || upper == L'C')
		return true;

	// Space (jump) - but allow in text for normal typing
	// We'll check this separately based on context

	return false;
}

// Send a character key press via Windows SendInput API (with optional shift).
//
// Two emission modes:
//
//   ch != 0 (printable character): scancode-only events. VK events would also
//     produce WM_CHAR via TranslateMessage, and PostCharToGame separately
//     injects a GFxCharEvent — both reach Scaleform, so emitting VK in this
//     mode would cause double character entry. PostCharToGame is the
//     authoritative Scaleform path here; scancode-only covers DirectInput.
//
//   ch == 0 (control key: ESC, Backspace, Enter, Tab, arrows, etc.): VK
//     events only. Windows synthesizes the matching scancode in the lower
//     keyboard input stream automatically when SendInput processes a VK
//     event, so DirectInput consumers still see the press, AND WM_KEYDOWN
//     is posted for Scaleform/menu consumers — both paths covered with a
//     single emission. Sending an explicit scancode-only event in addition
//     to the VK event causes Skyrim's input pipeline to fire OnKeyDown twice
//     per logical press, which floods Papyrus key handlers (e.g. AIAgent's
//     OverlayStatusCycle hotkey) at ~30Hz when typing through OCU's vkey.
//
// Set postChar=false to suppress the PostCharToGame call — used when only
// the DirectInput / game-hotkey path is desired (rare).
static void SendVirtualKey(WORD vk, bool shift, wchar_t ch = 0, bool postChar = true, bool pcMode = false)
{
	OOVR_DEBUG_LOGF("[VKEMIT] SendVirtualKey(vk=0x%02X shift=%d ch=0x%04X postChar=%d pcMode=%d) prismaFocused=%d",
	    vk, shift ? 1 : 0, (unsigned)ch, postChar ? 1 : 0, pcMode ? 1 : 0, IsPrismaTextFocused() ? 1 : 0);
	WORD scan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
	WORD shiftScan = (WORD)MapVirtualKeyW(VK_SHIFT, MAPVK_VK_TO_VSC);

	std::vector<INPUT> inputs;

	if (shift) {
		INPUT in = {};
		in.type = INPUT_KEYBOARD;
		if (!ch) {
			// VK shift down — Windows also injects the scancode for DirectInput.
			in.ki.wVk = VK_SHIFT;
			in.ki.wScan = shiftScan;
			in.ki.dwFlags = 0;
			inputs.push_back(in);
		} else {
			// ch != 0: VK skipped to avoid WM_CHAR double-entry; scancode-only
			// for DirectInput.
			in.ki.wVk = 0;
			in.ki.wScan = shiftScan;
			in.ki.dwFlags = KEYEVENTF_SCANCODE;
			inputs.push_back(in);
		}
	}

	if (!ch) {
		// Control-key path: VK events only — synthesized scancode covers
		// DirectInput, WM_KEYDOWN covers Scaleform.
		INPUT keyDown = {};
		keyDown.type = INPUT_KEYBOARD;
		keyDown.ki.wVk = vk;
		keyDown.ki.wScan = scan;
		keyDown.ki.dwFlags = 0;
		inputs.push_back(keyDown);

		INPUT keyUp = {};
		keyUp.type = INPUT_KEYBOARD;
		keyUp.ki.wVk = vk;
		keyUp.ki.wScan = scan;
		keyUp.ki.dwFlags = KEYEVENTF_KEYUP;
		inputs.push_back(keyUp);
	} else {
		// Printable-character path: scancode-only for DirectInput; Scaleform
		// gets the character via PostCharToGame below.
		INPUT scanDown = {};
		scanDown.type = INPUT_KEYBOARD;
		scanDown.ki.wVk = 0;
		scanDown.ki.wScan = scan;
		scanDown.ki.dwFlags = KEYEVENTF_SCANCODE;
		inputs.push_back(scanDown);

		INPUT scanUp = {};
		scanUp.type = INPUT_KEYBOARD;
		scanUp.ki.wVk = 0;
		scanUp.ki.wScan = scan;
		scanUp.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
		inputs.push_back(scanUp);
	}

	if (shift) {
		INPUT in = {};
		in.type = INPUT_KEYBOARD;
		if (!ch) {
			// VK shift up — synthesized scancode covers DirectInput.
			in.ki.wVk = VK_SHIFT;
			in.ki.wScan = shiftScan;
			in.ki.dwFlags = KEYEVENTF_KEYUP;
			inputs.push_back(in);
		} else {
			// Scancode shift up.
			in.ki.wVk = 0;
			in.ki.wScan = shiftScan;
			in.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
			inputs.push_back(in);
		}
	}

	// Suppression gate: skip SendInput when a Prisma text field is focused
	// AND we're in VR MODE — PrismaVR direct-delivery handles the keystroke,
	// Skyrim should never see it.
	//
	// In PC MODE the user explicitly chose "send to game" semantics, so we
	// always pass through. F1-F12 also bypass even in VR mode (game hotkeys).
	const bool isFKey = (vk >= VK_F1 && vk <= VK_F12);
	const bool gateBypassed = pcMode || isFKey;
	if (gateBypassed || !ShouldSuppressSkyrimInput()) {
		EnsureGameForeground();
		::SendInput((UINT)inputs.size(), inputs.data(), sizeof(INPUT));
	}

	// Post character directly to SKSE plugin for Scaleform injection.
	// This is the ONLY path to Scaleform when ch != 0 (VK events skipped above).
	// When postChar=false, skip this — scancode alone suffices for DirectInput.
	// When OC_PRISMA_TEXT is set, PostCharToGame redirects to PrismaVR_DeliverChar
	// (mode==0) or PrismaVR_DeliverVKey (mode==1) and Skyrim never sees the keystroke.
	if (ch && postChar)
		PostCharToGame(ch);
}
#endif

static std::vector<char> loadResource(int rid, int type)
{
#ifdef _WIN32
	// Open our OBJ file
	HRSRC ref = FindResource(openovr_module_id, MAKEINTRESOURCE(rid), MAKEINTRESOURCE(type));
	if (!ref) {
		string err = "FindResource error: " + std::to_string(GetLastError());
		OOVR_ABORT(err.c_str());
	}

	char* cstr = (char*)LoadResource(openovr_module_id, ref);
	if (!cstr) {
		string err = "LoadResource error: " + std::to_string(GetLastError());
		OOVR_ABORT(err.c_str());
	}

	DWORD len = SizeofResource(openovr_module_id, ref);
	if (!len) {
		string err = "SizeofResource error: " + std::to_string(GetLastError());
		OOVR_ABORT(err.c_str());
	}

	return std::vector<char>(cstr, cstr + len);
#else
	OOVR_ABORT("Keyboard font loading not implemented on this platform");
	return {};
#endif
}

static void LoadSpaceBarImage()
{
	if (s_spaceBarLoaded)
		return;
	s_spaceBarLoaded = true;

	// Load space bar image from embedded resource
	auto spaceBarData = loadResource(RES_O_SPACEBAR, RES_T_PNG);

	// Decode PNG from memory
	unsigned error = lodepng::decode(s_spaceBarImage, s_spaceBarWidth, s_spaceBarHeight,
	    (const uint8_t*)spaceBarData.data(), spaceBarData.size(), LCT_RGBA, 8);
	if (error) {
		OOVR_LOGF("Failed to decode SpaceBar.png from resource: %s", lodepng_error_text(error));
		s_spaceBarImage.clear();
		s_spaceBarWidth = 0;
		s_spaceBarHeight = 0;
	} else {
		OOVR_DEBUG_LOGF("Loaded SpaceBar.png from resource: %ux%u", s_spaceBarWidth, s_spaceBarHeight);
	}
}

static void InitSounds()
{
	if (s_soundsInitialized) return;
	s_soundsInitialized = true;

	// Load sounds from embedded resources
	s_hoverSoundData = loadResource(RES_O_SND_HOVER, RES_T_WAV);
	s_pressSoundData = loadResource(RES_O_SND_PRESS, RES_T_WAV);

	OOVR_DEBUG_LOGF("Loaded keyboard sounds from resources: hover=%zu bytes, press=%zu bytes",
		s_hoverSoundData.size(), s_pressSoundData.size());
}

// Apply volume scaling to WAV data by modifying audio samples
static std::vector<char> ApplyVolumeToWAV(const std::vector<char>& wavData, int volumePercent)
{
	if (wavData.size() < 44 || volumePercent >= 100)
		return wavData; // Return original if too small or full volume

	std::vector<char> result = wavData;
	float volumeFactor = volumePercent / 100.0f;

	// WAV files: first 44 bytes are header, then come audio samples
	// Assuming 16-bit PCM (most common), each sample is 2 bytes (int16_t)
	int16_t* samples = (int16_t*)(result.data() + 44);
	size_t sampleCount = (result.size() - 44) / 2;

	for (size_t i = 0; i < sampleCount; i++) {
		float scaled = samples[i] * volumeFactor;
		// Clamp to int16_t range to prevent overflow
		if (scaled > 32767.0f) scaled = 32767.0f;
		if (scaled < -32768.0f) scaled = -32768.0f;
		samples[i] = (int16_t)scaled;
	}

	return result;
}

static void PlayHoverSound()
{
	if (!s_soundsEnabled || s_hoverVolume <= 0)
		return;

	InitSounds();
	if (s_hoverSoundData.empty())
		return;

	// Stop any currently playing sound to prevent queueing
	PlaySoundA(NULL, NULL, SND_PURGE);

	// Apply volume scaling and play - use static buffer to persist during async playback
	// (Fix 1.1: local vector went out of scope while async playback continued = UAF)
	static std::vector<char> s_hoverAdjusted;
	s_hoverAdjusted = ApplyVolumeToWAV(s_hoverSoundData, s_hoverVolume);
	PlaySoundA((LPCSTR)s_hoverAdjusted.data(), NULL, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

static void PlayPressSound()
{
	if (!s_soundsEnabled || s_pressVolume <= 0)
		return;

	InitSounds();
	if (s_pressSoundData.empty())
		return;

	// Apply volume scaling and play - use static buffer to persist during async playback
	// (Fix 1.1: local vector went out of scope while async playback continued = UAF)
	static std::vector<char> s_pressAdjusted;
	s_pressAdjusted = ApplyVolumeToWAV(s_pressSoundData, s_pressVolume);
	PlaySoundA((LPCSTR)s_pressAdjusted.data(), NULL, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
}

std::wstring_convert<std::codecvt_utf8<wchar_t>> VRKeyboard::CHAR_CONV;

VRKeyboard::VRKeyboard(ID3D11Device* dev, uint64_t userValue, uint32_t maxLength, bool minimal, eventDispatch_t eventDispatch,
    EGamepadTextInputMode inputMode)
    : dev(dev), userValue(userValue), maxLength(maxLength), minimal(minimal), eventDispatch(eventDispatch), inputMode(inputMode)
{
	LoadKeyboardSettings();
	LoadSpaceBarImage();


#ifdef _WIN32
	// No EnsureGameForeground() here — keyboard auto-creation (text-input detection) was
	// stealing desktop focus every few seconds. Focus is restored in the SendInput paths,
	// the only place it's actually needed.
	OOVR_LOG("VRKeyboard: created (focus untouched until first keypress)");
	// Signal SKSE plugin that VR keyboard is active — SKSE suppresses WM_CHAR
	// to prevent double character entry (scancode WM_CHAR + GFxCharEvent from PostCharToGame)
	HWND kbHwnd = GetGameWindow();
	if (kbHwnd)
		SetPropW(kbHwnd, L"OC_KB_ACTIVE", (HANDLE)1);
#endif

	std::shared_ptr<BaseCompositor> cmp = GetBaseCompositor();
	if (!cmp)
		OOVR_ABORT("Keyboard: Compositor must be active!");

	// Validate the D3D device pointer rigorously.
	// We've seen crashes with dev=0xF (dangling/corrupt pointer) even when callers
	// checked the pointer value beforehand — likely a COM use-after-free race.
	if (!dev || reinterpret_cast<uintptr_t>(dev) <= 0xFFFF) {
		OOVR_LOGF("VRKeyboard: invalid device pointer 0x%llX — aborting keyboard creation",
		    (unsigned long long)reinterpret_cast<uintptr_t>(dev));
		throw std::runtime_error("VRKeyboard: invalid D3D11 device pointer");
	}

	// AddRef the device to prevent COM use-after-free. The compositor may release
	// its device reference on another thread; holding our own ref keeps it alive.
	dev->AddRef();

	if (inputMode == EGamepadTextInputMode::k_EGamepadTextInputModePassword)
		OOVR_ABORT("Password input mode not yet supported!");

	// zero stuff out
	memset(lastInputTime, 0, sizeof(lastInputTime));
	memset(repeatCount, 0, sizeof(repeatCount));
	memset(selected, 0, sizeof(selected));
	memset(lastButtonState, 0, sizeof(lastButtonState));

	// D3D setup (Fix 1.2: add null check for ctx)
	dev->GetImmediateContext(&ctx);
	if (!ctx) {
		OOVR_ABORT("Failed to get D3D11 immediate context for VR keyboard");
	}

	// Create OpenXR swap chain for the keyboard texture
	XrSwapchainCreateInfo swapchainInfo = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
	swapchainInfo.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
	swapchainInfo.format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	swapchainInfo.sampleCount = 1;
	swapchainInfo.width = texWidth;
	swapchainInfo.height = texHeight;
	swapchainInfo.faceCount = 1;
	swapchainInfo.arraySize = 1;
	swapchainInfo.mipCount = 1;

	OOVR_FAILED_XR_ABORT(xrCreateSwapchain(xr_session.get(), &swapchainInfo, &chain));

	// Enumerate swap chain images
	uint32_t imageCount = 0;
	OOVR_FAILED_XR_ABORT(xrEnumerateSwapchainImages(chain, 0, &imageCount, nullptr));

	swapchainImages.resize(imageCount, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
	OOVR_FAILED_XR_ABORT(xrEnumerateSwapchainImages(chain,
	    swapchainImages.size(), &imageCount, (XrSwapchainImageBaseHeader*)swapchainImages.data()));

	// Set up the OpenXR composition layer quad — WORLD-ANCHORED
	// Using floorSpace (stage) so the keyboard stays fixed in world space.
	// The user can grab the top bar and reposition it.
	memset(&layer, 0, sizeof(layer));
	layer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
	layer.next = nullptr;
	layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT
	    | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
	layer.space = xr_gbl->floorSpace; // World-anchored: stays in place
	layer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
	layer.subImage.swapchain = chain;
	layer.subImage.imageRect.offset = { 0, 0 };
	layer.subImage.imageRect.extent = { (int32_t)texWidth, (int32_t)texHeight };
	layer.subImage.imageArrayIndex = 0;

	float scaleFactor = s_scalePercent / 100.0f;
	layer.size.width = 1.05f * scaleFactor;
	layer.size.height = 0.49f * scaleFactor;

	// Spawn the keyboard in front of the player's current head position
	XrSpaceLocation headLoc = { XR_TYPE_SPACE_LOCATION };
	XrResult headResult = xrLocateSpace(xr_gbl->viewSpace, xr_gbl->floorSpace,
	    xr_gbl->GetBestTime(), &headLoc);

	if (XR_SUCCEEDED(headResult)
	    && (headLoc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
	    && (headLoc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
		// Head forward direction projected to horizontal plane (yaw only)
		XrVector3f headFwd;
		XrVector3f localFwd = { 0.0f, 0.0f, -1.0f };
		rotate_vector_by_quaternion(localFwd, headLoc.pose.orientation, headFwd);
		headFwd.y = 0;
		float fwdLen = sqrtf(headFwd.x * headFwd.x + headFwd.z * headFwd.z);
		if (fwdLen > 0.001f) {
			headFwd.x /= fwdLen;
			headFwd.z /= fwdLen;
		} else {
			headFwd = { 0.0f, 0.0f, -1.0f };
		}

		// Sticky spawn: user-parked head-relative offset (defaults: 0.80m fwd, 0.52m below)
		XrVector3f headRight = { -headFwd.z, 0.0f, headFwd.x };
		layer.pose.position = {
			headLoc.pose.position.x + headFwd.x * s_posForward + headRight.x * s_posRight,
			headLoc.pose.position.y - s_posDown,
			headLoc.pose.position.z + headFwd.z * s_posForward + headRight.z * s_posRight
		};

		// Orientation: face toward user + tilt
		float yaw = atan2f(headFwd.x, headFwd.z);
		s_lastYaw = yaw;
		layer.pose.orientation = buildTiltedOrientation(yaw, s_tiltDegrees);
	} else {
		// Fallback: default position facing -Z with tilt
		layer.pose.position = { 0.0f, 0.83f, -0.80f };
		s_lastYaw = 0.0f;
		layer.pose.orientation = buildTiltedOrientation(0.0f, s_tiltDegrees);
	}

	// Loading the layout also resolves its carried theme/font and artwork.  Do
	// this once, after the .kb exists, so a custom SFN is active on first open.
	LoadKeyboardLayout(); // embedded default or a Keyboard Studio .kb beside the DLL

	// Reuse the immutable laser atlas shared by keyboard, flat menus, and console.
	laserAtlas = LaserTextureAtlas::Acquire(dev);
	const XrSwapchain atlasChain = laserAtlas->GetSwapchain();
	const XrRect2Di idleBeamRect = laserAtlas->GetImageRect(LaserAtlasRegion::BeamIdle);

	for (int i = 0; i < 2; i++) {
		// Initialize the laser composition layer
		memset(&laserLayer[i], 0, sizeof(laserLayer[i]));
		laserLayer[i].type = XR_TYPE_COMPOSITION_LAYER_QUAD;
		laserLayer[i].layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		laserLayer[i].space = xr_gbl->floorSpace;
		laserLayer[i].eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		laserLayer[i].subImage.swapchain = atlasChain;
		laserLayer[i].subImage.imageRect = idleBeamRect;
		laserLayer[i].subImage.imageArrayIndex = 0;
	}

	// Controller/headset dots select the atlas's warm-white dot region.
	const XrRect2Di idleDotRect = laserAtlas->GetImageRect(LaserAtlasRegion::DotIdle);
	for (int i = 0; i < 3; i++) {
		// Initialize the target dot composition layer
		memset(&targetDotLayer[i], 0, sizeof(targetDotLayer[i]));
		targetDotLayer[i].type = XR_TYPE_COMPOSITION_LAYER_QUAD;
		targetDotLayer[i].layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		targetDotLayer[i].space = xr_gbl->floorSpace;
		targetDotLayer[i].eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		targetDotLayer[i].subImage.swapchain = atlasChain;
		targetDotLayer[i].subImage.imageRect = idleDotRect;
		targetDotLayer[i].subImage.imageArrayIndex = 0;
		// The bright core remains about one centimetre; the larger transparent
		// quad carries the soft halo around it.
		targetDotLayer[i].size.width = 0.026f;
		targetDotLayer[i].size.height = 0.026f;
	}
	for (int i = 0; i < 2; ++i) {
		cursorDotLayer[i] = targetDotLayer[i];
		cursorDotLayer[i].size.width = 0.028f;
		cursorDotLayer[i].size.height = 0.028f;
	}

	// Create console INPUT overlay swapchain (floating panel above keyboard)
	{
		XrSwapchainCreateInfo conSci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
		conSci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
		conSci.format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		conSci.sampleCount = 1;
		conSci.width = consoleTexWidth;
		conSci.height = consoleTexHeight;
		conSci.faceCount = 1;
		conSci.arraySize = 1;
		conSci.mipCount = 1;

		OOVR_FAILED_XR_ABORT(xrCreateSwapchain(xr_session.get(), &conSci, &consoleChain));

		uint32_t conImgCount = 0;
		OOVR_FAILED_XR_ABORT(xrEnumerateSwapchainImages(consoleChain, 0, &conImgCount, nullptr));
		consoleSwapImages.resize(conImgCount, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
		OOVR_FAILED_XR_ABORT(xrEnumerateSwapchainImages(consoleChain, conImgCount, &conImgCount,
		    (XrSwapchainImageBaseHeader*)consoleSwapImages.data()));
	}

	// Console layer struct
	{
		memset(&consoleLayer, 0, sizeof(consoleLayer));
		consoleLayer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
		// The CPU renderer writes ordinary (unpremultiplied) RGBA. Advertising
		// that explicitly prevents transparent light colors from becoming an
		// opaque white rectangle on runtimes that assume premultiplied input.
		consoleLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT
		    | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
		consoleLayer.space = xr_gbl->floorSpace;
		consoleLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		consoleLayer.subImage.swapchain = consoleChain;
		consoleLayer.subImage.imageRect.offset = { 0, 0 };
		consoleLayer.subImage.imageRect.extent = { (int32_t)consoleTexWidth, (int32_t)consoleTexHeight };
		consoleLayer.subImage.imageArrayIndex = 0;
		consoleLayer.size.width = 0.80f;
		consoleLayer.size.height = 0.80f * ((float)consoleTexHeight / (float)consoleTexWidth);
	}

	// CROSSHAIR DISABLED FOR DEBUG
	// Create crosshair dot swapchain — tiny white dot for console gaze aiming
	// {
	// 	XrSwapchainCreateInfo chSci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
	// 	... (crosshair code disabled)
	// }
	OOVR_LOG("VR keyboard resources: 2 private swapchains (panel, console) plus shared laser atlas");
	g_ocuKeyboardActive.store(true, std::memory_order_release);
}

VRKeyboard::~VRKeyboard()
{
	g_ocuKeyboardActive.store(false, std::memory_order_release);
	ReleaseAllHeldPCKeys();
	if (crosshairChain != XR_NULL_HANDLE) {
		xrDestroySwapchain(crosshairChain);
		crosshairChain = XR_NULL_HANDLE;
	}
	if (consoleChain != XR_NULL_HANDLE) {
		xrDestroySwapchain(consoleChain);
		consoleChain = XR_NULL_HANDLE;
	}
	if (chain != XR_NULL_HANDLE) {
		xrDestroySwapchain(chain);
		chain = XR_NULL_HANDLE;
	}
	if (ctx)
		ctx->Release();
	if (dev)
		dev->Release();

#ifdef _WIN32
	// Clear OC_KB_ACTIVE so SKSE stops suppressing WM_CHAR
	HWND kbHwnd = GetGameWindow();
	if (kbHwnd)
		SetPropW(kbHwnd, L"OC_KB_ACTIVE", (HANDLE)0);
#endif
}

void VRKeyboard::PressHeldPCKey(int side, int keyId, uint16_t vk, bool shift, bool scanOnly)
{
	if (side < 0 || side >= 2 || vk == 0)
		return;

	// A hand can own only one held key. This also repairs stale state before a
	// new press following a controller reconnect.
	ReleaseHeldPCKey(side);
	heldPCKeys[side] = { vk, keyId, shift, scanOnly };
#ifdef _WIN32
	SendPCVirtualKeyState((WORD)vk, shift, scanOnly, true);
#endif
}

void VRKeyboard::ReleaseHeldPCKey(int side)
{
	if (side < 0 || side >= 2)
		return;

	const bool releaseCtrl = releaseCtrlAfterHeldPCKey[side];
	releaseCtrlAfterHeldPCKey[side] = false;
	HeldPCKey held = heldPCKeys[side];
	heldPCKeys[side] = {};
	s_pressedKey[side] = -1;
#ifdef _WIN32
	if (held.vk != 0)
		SendPCVirtualKeyState((WORD)held.vk, held.shift, held.scanOnly, false);
#endif
	if (releaseCtrl)
		ReleaseCtrlLatch();
}

void VRKeyboard::ToggleCtrlLatch(int side)
{
	if (ctrlLatched) {
		ReleaseCtrlLatch();
		return;
	}

	// Start from a known keyboard state before latching the modifier.
	ReleaseAllHeldPCKeys();
	ctrlLatched = true;
	ctrlLatchSide = side;
#ifdef _WIN32
	SendPCVirtualKeyState(VK_CONTROL, false, false, true);
#endif
	dirty = true;
}

void VRKeyboard::ReleaseCtrlLatch()
{
	if (!ctrlLatched)
		return;

	ctrlLatched = false;
	ctrlLatchSide = -1;
	releaseCtrlAfterHeldPCKey[0] = false;
	releaseCtrlAfterHeldPCKey[1] = false;
#ifdef _WIN32
	SendPCVirtualKeyState(VK_CONTROL, false, false, false);
#endif
	dirty = true;
}

void VRKeyboard::ReleaseAllHeldPCKeys()
{
	ReleaseHeldPCKey(0);
	ReleaseHeldPCKey(1);
	ReleaseCtrlLatch();
}

wstring VRKeyboard::contents()
{
	return text;
}

void VRKeyboard::contents(wstring str)
{
	text = str;
	cursorPos = (int)text.size();
	dirty = true;
}

// Convert thumbstick axis values into D-pad button bits so the keyboard
// navigation code (which checks k_EButton_DPad_*) works with Quest Touch
// controllers that only report thumbstick as analog axes.
static void InjectThumbstickAsDpad(vr::VRControllerState_t& state, float deadzone = 0.5f)
{
	// Axis 0 is the joystick/thumbstick in OpenComposite's mapping.
	// The keyboard layout uses Left/Right for horizontal navigation and
	// Up/Down for row navigation. Quest thumbstick axes are remapped:
	//   Stick Left/Right (X axis) → D-pad Up/Down (row navigation)
	//   Stick Up/Down (Y axis)    → D-pad Left/Right (key navigation)
	float x = state.rAxis[0].x;
	float y = state.rAxis[0].y;

	if (y > deadzone)
		state.ulButtonPressed |= vr::ButtonMaskFromId(vr::k_EButton_DPad_Right);
	if (y < -deadzone)
		state.ulButtonPressed |= vr::ButtonMaskFromId(vr::k_EButton_DPad_Left);
	if (x < -deadzone)
		state.ulButtonPressed |= vr::ButtonMaskFromId(vr::k_EButton_DPad_Down);
	if (x > deadzone)
		state.ulButtonPressed |= vr::ButtonMaskFromId(vr::k_EButton_DPad_Up);
}

static inline float xr_dot(const XrVector3f& a, const XrVector3f& b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

// Compute a quaternion that orients a quad so its Y axis aligns with beamDir
// and the quad faces the viewer at viewerPos.
static XrQuaternionf beamOrientation(XrVector3f beamDir, XrVector3f midpoint, XrVector3f viewerPos)
{
	// up = beamDir (height axis of quad)
	XrVector3f up = beamDir;

	// forward = from midpoint toward viewer
	XrVector3f toViewer = {
		viewerPos.x - midpoint.x,
		viewerPos.y - midpoint.y,
		viewerPos.z - midpoint.z
	};
	float ml = sqrtf(toViewer.x * toViewer.x + toViewer.y * toViewer.y + toViewer.z * toViewer.z);
	XrVector3f fwd;
	if (ml > 0.001f) {
		fwd = { toViewer.x / ml, toViewer.y / ml, toViewer.z / ml };
	} else {
		fwd = { 0, 0, 1 };
	}

	// right = cross(up, fwd)
	XrVector3f right = {
		up.y * fwd.z - up.z * fwd.y,
		up.z * fwd.x - up.x * fwd.z,
		up.x * fwd.y - up.y * fwd.x
	};
	float rl = sqrtf(right.x * right.x + right.y * right.y + right.z * right.z);
	if (rl < 0.001f) {
		right = { 1, 0, 0 };
		rl = 1.0f;
	}
	right.x /= rl; right.y /= rl; right.z /= rl;

	// Re-orthogonalize forward = cross(right, up)
	fwd = {
		right.y * up.z - right.z * up.y,
		right.z * up.x - right.x * up.z,
		right.x * up.y - right.y * up.x
	};

	// Rotation matrix [right | up | fwd] as columns → quaternion
	// R = | right.x  up.x  fwd.x |
	//     | right.y  up.y  fwd.y |
	//     | right.z  up.z  fwd.z |
	float trace = right.x + up.y + fwd.z;
	XrQuaternionf q;

	if (trace > 0) {
		float s = 0.5f / sqrtf(trace + 1.0f);
		q.w = 0.25f / s;
		q.x = (up.z - fwd.y) * s;
		q.y = (fwd.x - right.z) * s;
		q.z = (right.y - up.x) * s;
	} else if (right.x > up.y && right.x > fwd.z) {
		float s = 2.0f * sqrtf(1.0f + right.x - up.y - fwd.z);
		q.w = (up.z - fwd.y) / s;
		q.x = 0.25f * s;
		q.y = (up.x + right.y) / s;
		q.z = (fwd.x + right.z) / s;
	} else if (up.y > fwd.z) {
		float s = 2.0f * sqrtf(1.0f + up.y - right.x - fwd.z);
		q.w = (fwd.x - right.z) / s;
		q.x = (up.x + right.y) / s;
		q.y = 0.25f * s;
		q.z = (fwd.y + up.z) / s;
	} else {
		float s = 2.0f * sqrtf(1.0f + fwd.z - right.x - up.y);
		q.w = (right.y - up.x) / s;
		q.x = (fwd.x + right.z) / s;
		q.y = (fwd.y + up.z) / s;
		q.z = 0.25f * s;
	}

	return q;
}

void VRKeyboard::UpdateLaserBeam(int side)
{
	if (!laserActive[side])
		return;

	XrVector3f A = laserOrigin[side];
	XrVector3f B = laserHitPoint[side];

	float dx = B.x - A.x, dy = B.y - A.y, dz = B.z - A.z;
	float fullLen = sqrtf(dx * dx + dy * dy + dz * dz);
	if (fullLen < 0.01f) {
		laserActive[side] = false;
		return;
	}

	// Beam extends 60% of the way — stops short of the keyboard (Virtual Desktop style)
	float beamFraction = 0.6f;
	float beamLen = fullLen * beamFraction;

	XrVector3f beamEnd = {
		A.x + dx * beamFraction,
		A.y + dy * beamFraction,
		A.z + dz * beamFraction
	};

	XrVector3f dir = { dx / fullLen, dy / fullLen, dz / fullLen };
	XrVector3f mid = {
		(A.x + beamEnd.x) * 0.5f,
		(A.y + beamEnd.y) * 0.5f,
		(A.z + beamEnd.z) * 0.5f
	};

	laserLayer[side].pose.position = mid;
	laserLayer[side].size.width = 0.003f; // 3mm thin
	laserLayer[side].size.height = beamLen;
	laserLayer[side].pose.orientation = beamOrientation(dir, mid, headWorldPos);
}

const std::vector<XrCompositionLayerBaseHeader*>& VRKeyboard::Update()
{
	activeLayers.clear();
	bool targetDotValid[3] = { false, false, false };

#ifdef _WIN32
	ULONGLONG now = GetTickCount64();
	// Periodically reload settings from INI so configurator changes apply live
	static ULONGLONG lastSettingsCheck = 0;
	if (now - lastSettingsCheck > 1000) { // check every 1000ms (1 second)
		lastSettingsCheck = now;
		LoadKeyboardSettings(); // Reloads all keyboard settings from INI
	}

	// Theme changed (configurator save or manual ini edit) — swap font/bg/palette live
	if (oovr_global_configuration.KbTheme() != loadedThemeName
	    || oovr_global_configuration.KbFont() != loadedFontName) {
		LoadThemeAssets();
		dirty = true;
		consoleDirty = true;
	}
	if (oovr_global_configuration.KbLayout() != loadedLayoutName) {
		LoadKeyboardLayout();
		dirty = true;
		consoleDirty = true;
	}
	// Ten animation samples per second keep the slow glow breathing fluid in VR
	// while halving full-surface redraws. There is no closed-keyboard cost.
	if (layout && layout->HasBreathingEffects() && now - lastAnimationRefreshMs >= 100) {
		lastAnimationRefreshMs = now;
		dirty = true;
		const KeyboardLayout::VisualStyle& style = layout->GetVisualStyle();
		if (consoleActive && style.enabled && style.fontGlowEnabled && style.fontBreatheEnabled)
			consoleDirty = true;
	}
#endif

	BaseSystem* sys = GetUnsafeBaseSystem();
	if (sys) {
		float time = (float)(GetTickCount64() / 1000.0);

		// Update head position for beam billboard orientation
		if (headLocked) {
			// In viewSpace the head IS the origin
			headWorldPos = { 0, 0, 0 };
		} else {
			XrSpaceLocation headLoc = { XR_TYPE_SPACE_LOCATION };
			if (XR_SUCCEEDED(xrLocateSpace(xr_gbl->viewSpace, xr_gbl->floorSpace,
			        xr_gbl->GetBestTime(), &headLoc))
			    && (headLoc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) {
				headWorldPos = headLoc.pose.position;
			}
		}

		// Watch opencomposite.ini for external changes (e.g. from configurator)
		{
			static ULONGLONG lastSettingsCheck = 0;
			static FILETIME lastWriteTime = {};
			ULONGLONG now = GetTickCount64();
			if (now - lastSettingsCheck > 1000) { // check once per second
				lastSettingsCheck = now;
				std::wstring settingsPath = GetOCDllDirectory() + L"opencomposite.ini";
				WIN32_FILE_ATTRIBUTE_DATA fad = {};
				if (GetFileAttributesExW(settingsPath.c_str(), GetFileExInfoStandard, &fad)) {
					if (CompareFileTime(&fad.ftLastWriteTime, &lastWriteTime) != 0) {
						lastWriteTime = fad.ftLastWriteTime;
						OOVR_DEBUG_LOG("Config file changed, reloading settings...");
						if (ReloadKeyboardSettings()) {
							// Apply new tilt orientation
							if (headLocked) {
								float pitchRad = -s_tiltDegrees * 3.14159265f / 180.0f;
								layer.pose.orientation = { sinf(pitchRad * 0.5f), 0.0f, 0.0f, cosf(pitchRad * 0.5f) };
							} else {
								layer.pose.orientation = buildTiltedOrientation(s_lastYaw, s_tiltDegrees);
							}
							// Apply new scale
							float sf = s_scalePercent / 100.0f;
							layer.size.width = 1.05f * sf;
							layer.size.height = 0.49f * sf;
							dirty = true;
							OOVR_DEBUG_LOGF("Settings reloaded: tilt=%.1f opacity=%d scale=%d haptic=%d sounds=%d",
								s_tiltDegrees, s_opacityPercent, s_scalePercent, s_hapticStrength, s_soundsEnabled);
						} else {
							OOVR_LOG("Settings reload failed - could not read ini file");
						}
					}
				}
			}
		}

		// Laser pointer hit testing
		for (int side = 0; side < 2; side++) {
			int hitResult = HitTestLaser(side);
			if (hitResult >= 0) {
				if (hitResult != selected[side]) {
					selected[side] = hitResult;
					dirty = true;
					// Hover sound and haptic when highlighting a new key
					if (hitResult != s_lastHoveredKey[side]) {
						s_lastHoveredKey[side] = hitResult;
						// PlayHoverSound(); // DISABLED: causes game stuttering
						TriggerHaptic(side);
					}
				}
			} else {
				// Laser not on any key — clear highlight
				if (selected[side] >= 0) {
					selected[side] = -1;
					s_lastHoveredKey[side] = -1;
					dirty = true;
				}
			}

			// Arrow hover sound and haptic (tilt/opacity/size controls)
			int currentArrow = 0; // 0 = no arrow
			if (laserOnTiltUp[side]) currentArrow = -6;
			else if (laserOnTiltDown[side]) currentArrow = -7;
			else if (laserOnOpacityUp[side]) currentArrow = -8;
			else if (laserOnOpacityDown[side]) currentArrow = -9;
			else if (laserOnSizeUp[side]) currentArrow = -10;
			else if (laserOnSizeDown[side]) currentArrow = -11;

			if (currentArrow != s_lastHoveredArrow[side]) {
				if (currentArrow != 0) {
					// Hovering over a new arrow
					// PlayHoverSound(); // DISABLED: causes game stuttering
					TriggerHaptic(side);
				}
				s_lastHoveredArrow[side] = currentArrow;
			}
			if (laserActive[side]) {
				UpdateLaserBeam(side);

				// Track the laser contact with a tiny composition layer. Cursor motion
				// no longer dirties, recreates, waits on, and uploads the full keyboard
				// texture at headset refresh rate.
				XrVector3f normal;
				rotate_vector_by_quaternion(
					{ 0.0f, 0.0f, 1.0f }, layer.pose.orientation, normal);
				const XrVector3f towardController = {
					laserOrigin[side].x - laserHitPoint[side].x,
					laserOrigin[side].y - laserHitPoint[side].y,
					laserOrigin[side].z - laserHitPoint[side].z
				};
				if (xr_dot(normal, towardController) < 0.0f) {
					normal.x = -normal.x;
					normal.y = -normal.y;
					normal.z = -normal.z;
				}
				constexpr float cursorLiftMeters = 0.0015f;
				cursorDotLayer[side].pose.position = {
					laserHitPoint[side].x + normal.x * cursorLiftMeters,
					laserHitPoint[side].y + normal.y * cursorLiftMeters,
					laserHitPoint[side].z + normal.z * cursorLiftMeters
				};
				cursorDotLayer[side].pose.orientation = layer.pose.orientation;
				cursorDotLayer[side].space = layer.space;
			}
		}

		// Force redraw every 500ms for blinking cursor in non-minimal mode
		if (!minimal) {
			bool cursorBlink = ((GetTickCount64() / 500) % 2) == 0;
			static bool lastCursorBlink = false;
			if (cursorBlink != lastCursorBlink) {
				lastCursorBlink = cursorBlink;
				dirty = true;
			}
		}

		// Get controller states once for grab logic and input handling
		vr::VRControllerState_t states[2] = {};
		bool hasState[2] = { false, false };
		hasState[0] = sys->GetUnmaskedControllerState(1, &states[0], sizeof(states[0]));
		hasState[1] = sys->GetUnmaskedControllerState(2, &states[1], sizeof(states[1]));

		// Grab bar logic — trigger to drag, toggle to switch head-lock mode
		for (int side = 0; side < 2; side++) {
			if (!hasState[side]) {
				// A missing controller state cannot deliver a release edge. Drop
				// any synthetic hold immediately so Windows never keeps a key down.
				ReleaseHeldPCKey(side);
				if (ctrlLatched && ctrlLatchSide == side)
					ReleaseCtrlLatch();
				lastTriggerState[side] = false;
				lastButtonState[side] = 0;
				s_pressedKey[side] = -1;
				continue;
			}

			bool trigNow = (states[side].ulButtonPressed & vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Trigger)) != 0;
			bool trigJustPressed = trigNow && !lastTriggerState[side];
			bool trigJustReleased = !trigNow && lastTriggerState[side];
			lastTriggerState[side] = trigNow;

			// Mode toggle button — switch between VR MODE and PC MODE
			if (trigJustPressed && laserOnConsole[side] && !grabActive) {
				ReleaseAllHeldPCKeys();
				sendInputOnly = !sendInputOnly;
				OOVR_DEBUG_LOGF("Mode toggle: sendInputOnly=%d (%s)", sendInputOnly, sendInputOnly ? "PC MODE" : "VR MODE");
				dirty = true;
				continue;
			}

			// Head-lock toggle — trigger press on the toggle button
			if (trigJustPressed && laserOnToggle[side] && !grabActive) {
				headLocked = !headLocked;
				if (headLocked) {
					// Switch to head-locked mode
					grabActive = false;
					grabbingSide = -1;
					layer.space = xr_gbl->viewSpace;
					layer.pose.position = { s_posRight, -s_posDown, -s_posForward };
					// Apply tilt in view space (yaw=0 since head-relative)
					s_lastYaw = 0.0f;
					float pitchRad = -s_tiltDegrees * 3.14159265f / 180.0f; // negative = bottom toward player
					layer.pose.orientation = { sinf(pitchRad * 0.5f), 0.0f, 0.0f, cosf(pitchRad * 0.5f) };
					for (int i = 0; i < 2; i++)
						laserLayer[i].space = xr_gbl->viewSpace;
					headWorldPos = { 0, 0, 0 };
				} else {
					// Switch to world-anchored — spawn at current head position
					layer.space = xr_gbl->floorSpace;
					for (int i = 0; i < 2; i++)
						laserLayer[i].space = xr_gbl->floorSpace;
					XrSpaceLocation hl = { XR_TYPE_SPACE_LOCATION };
					if (XR_SUCCEEDED(xrLocateSpace(xr_gbl->viewSpace, xr_gbl->floorSpace,
					        xr_gbl->GetBestTime(), &hl))
					    && (hl.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
					    && (hl.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
						XrVector3f headFwd;
						XrVector3f localFwd = { 0.0f, 0.0f, -1.0f };
						rotate_vector_by_quaternion(localFwd, hl.pose.orientation, headFwd);
						headFwd.y = 0;
						float fwdLen = sqrtf(headFwd.x * headFwd.x + headFwd.z * headFwd.z);
						if (fwdLen > 0.001f) { headFwd.x /= fwdLen; headFwd.z /= fwdLen; }
						else { headFwd = { 0.0f, 0.0f, -1.0f }; }
						XrVector3f headRight = { -headFwd.z, 0.0f, headFwd.x };
						layer.pose.position = {
							hl.pose.position.x + headFwd.x * s_posForward + headRight.x * s_posRight,
							hl.pose.position.y - s_posDown,
							hl.pose.position.z + headFwd.z * s_posForward + headRight.z * s_posRight
						};
						float yaw = atan2f(headFwd.x, headFwd.z);
						s_lastYaw = yaw;
						layer.pose.orientation = buildTiltedOrientation(yaw, s_tiltDegrees);
					}
					headWorldPos = hl.pose.position;
				}
				dirty = true;
				continue; // don't also start a grab this frame
			}

			// Arrow controls — tilt, opacity, size with hold-to-repeat
			// Fires on initial press, then repeats every 200ms while held
			{
				static ULONGLONG arrowRepeatNext[2] = {};
				static int arrowRepeatCode[2] = {}; // which arrow is repeating (0=none)

				bool onAnyArrow = laserOnTiltUp[side] || laserOnTiltDown[side]
				    || laserOnOpacityUp[side] || laserOnOpacityDown[side]
				    || laserOnSizeUp[side] || laserOnSizeDown[side];

				int arrowCode = 0;
				if (laserOnTiltUp[side]) arrowCode = -6;
				else if (laserOnTiltDown[side]) arrowCode = -7;
				else if (laserOnOpacityUp[side]) arrowCode = -8;
				else if (laserOnOpacityDown[side]) arrowCode = -9;
				else if (laserOnSizeUp[side]) arrowCode = -10;
				else if (laserOnSizeDown[side]) arrowCode = -11;

				bool shouldFire = false;
				ULONGLONG now = GetTickCount64();

				if (trigJustPressed && onAnyArrow) {
					shouldFire = true;
					arrowRepeatCode[side] = arrowCode;
					arrowRepeatNext[side] = now + 400; // initial delay before repeat
					// Play press sound and haptic on initial arrow press
					PlayPressSound();
					TriggerHaptic(side);
				} else if (trigNow && arrowRepeatCode[side] != 0 && onAnyArrow && arrowCode == arrowRepeatCode[side]) {
					if (now >= arrowRepeatNext[side]) {
						shouldFire = true;
						arrowRepeatNext[side] = now + 150; // repeat interval
					}
				}
				if (!trigNow || !onAnyArrow) {
					arrowRepeatCode[side] = 0;
				}

				if (shouldFire && arrowCode != 0) {
					switch (arrowCode) {
					case -6: // tilt up
						s_tiltDegrees += 1.0f;
						if (s_tiltDegrees > 80.0f) s_tiltDegrees = 80.0f;
						break;
					case -7: // tilt down
						s_tiltDegrees -= 1.0f;
						if (s_tiltDegrees < -30.0f) s_tiltDegrees = -30.0f;
						break;
					case -8: // opacity up
						s_opacityPercent += 5;
						if (s_opacityPercent > 100) s_opacityPercent = 100;
						break;
					case -9: // opacity down
						s_opacityPercent -= 5;
						if (s_opacityPercent < 1) s_opacityPercent = 1;
						break;
					case -10: // size up
						s_scalePercent += 5;
						if (s_scalePercent > 150) s_scalePercent = 150;
						break;
					case -11: // size down
						s_scalePercent -= 5;
						if (s_scalePercent < 50) s_scalePercent = 50;
						break;
					}
					// Apply tilt orientation
					if (arrowCode == -6 || arrowCode == -7) {
						if (headLocked) {
							float pitchRad = -s_tiltDegrees * 3.14159265f / 180.0f;
							layer.pose.orientation = { sinf(pitchRad * 0.5f), 0.0f, 0.0f, cosf(pitchRad * 0.5f) };
						} else {
							layer.pose.orientation = buildTiltedOrientation(s_lastYaw, s_tiltDegrees);
						}
					}
					// Apply scale
					if (arrowCode == -10 || arrowCode == -11) {
						float sf = s_scalePercent / 100.0f;
						layer.size.width = 1.05f * sf;
						layer.size.height = 0.49f * sf;
					}
					SaveKeyboardSettings();
					dirty = true;
					continue;
				}
			}

			// Text bar click — position the text cursor
			if (trigJustPressed && laserOnTextBar[side] && !minimal) {
				int clickTexX = (int)(laserU[side] * texWidth);
				int BORD = 3;
				int textBarX = 120 + int(std::round(layout->GetTextBarOffsetX()));
				int textStartX = textBarX + BORD + 6; // exactly matches Refresh()
				int relX = clickTexX - textStartX;
				const float textScale = layout->GetTextBarDesign().fontScale > 0
				    ? layout->GetTextBarDesign().fontScale : 1.0f;

				// Walk through text characters to find nearest boundary
				int accumX = 0;
				int newPos = 0;
				for (int i = 0; i < (int)text.size(); i++) {
					int charW = std::max(1, int(std::round(font->Width(text[i]) * textScale)));
					if (relX < accumX + charW / 2)
						break;
					accumX += charW;
					newPos = i + 1;
				}
#ifdef _WIN32
				// Mirror the caret jump into the game's Scaleform text box.
				// There is no absolute set-caret injection, but N arrow-key
				// events land on exactly the same position (the buffer and
				// the game field hold the same text in buffered modes).
				if (!sendInputOnly || consoleActive) {
					int delta = newPos - cursorPos;
					int steps = delta < 0 ? -delta : delta;
					if (steps > 256)
						steps = 256;
					WORD vkStep = delta < 0 ? VK_LEFT : VK_RIGHT;
					for (int s = 0; s < steps; s++)
						PostCharToGame(vkStep, 1);
				}
#endif
				cursorPos = newPos;
				dirty = true;
				continue;
			}

			// Grab — trigger on drag area to reposition (only in world mode)
			// Uses laser ray-plane intersection so the keyboard follows the laser 1:1.
			if (trigJustPressed && laserOnGrabBar[side] && !grabActive && !headLocked) {
				// The laser already hit the keyboard — use that hit point
				if (laserActive[side]) {
					grabActive = true;
					grabbingSide = side;
					grabPlaneOrigin = layer.pose.position;
					// Offset from hit point to keyboard center
					grabOffset = {
						layer.pose.position.x - laserHitPoint[side].x,
						layer.pose.position.y - laserHitPoint[side].y,
						layer.pose.position.z - laserHitPoint[side].z
					};
				}
			}

			if (grabActive && grabbingSide == side) {
				if (trigJustReleased) {
					grabActive = false;
					grabbingSide = -1;
					if (s_pinchActive) {
						s_pinchActive = false;
						SaveKeyboardSettings(); // persist the pinched scale
					}
					// Sticky position: persist the parked spot as a head-relative offset
					if (!headLocked) {
						XrSpaceLocation shl = { XR_TYPE_SPACE_LOCATION };
						if (XR_SUCCEEDED(xrLocateSpace(xr_gbl->viewSpace, xr_gbl->floorSpace,
						        xr_gbl->GetBestTime(), &shl))
						    && (shl.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
						    && (shl.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
							XrVector3f f3;
							rotate_vector_by_quaternion({ 0, 0, -1 }, shl.pose.orientation, f3);
							f3.y = 0;
							float len = sqrtf(f3.x * f3.x + f3.z * f3.z);
							if (len > 0.001f) {
								f3.x /= len;
								f3.z /= len;
								XrVector3f r3 = { -f3.z, 0, f3.x };
								float dx = layer.pose.position.x - shl.pose.position.x;
								float dz = layer.pose.position.z - shl.pose.position.z;
								float fwd = dx * f3.x + dz * f3.z;
								float right = dx * r3.x + dz * r3.z;
								float down = shl.pose.position.y - layer.pose.position.y;
								s_posForward = (fwd < 0.3f) ? 0.3f : ((fwd > 1.5f) ? 1.5f : fwd);
								s_posRight = (right < -1.0f) ? -1.0f : ((right > 1.0f) ? 1.0f : right);
								s_posDown = (down < -0.3f) ? -0.3f : ((down > 1.2f) ? 1.2f : down);
								SaveKeyboardPosition();
								OOVR_DEBUG_LOGF("[KB] Sticky position saved: fwd=%.2f down=%.2f right=%.2f",
								    s_posForward, s_posDown, s_posRight);
							}
						}
					}
				} else if (trigNow) {
					// Two-handed pinch scale: second trigger on the grab bar while this
					// hand is grabbing. Hand separation drives the scale; position freezes
					// while pinching so the keyboard doesn't swim as it stretches.
					{
						auto handSeparation = [&]() -> float {
							std::shared_ptr<BaseInput> input = GetBaseInput();
							if (!input || !input->AreActionsLoaded())
								return 0.0f;
							XrVector3f p[2];
							for (int h = 0; h < 2; h++) {
								XrSpace hs = XR_NULL_HANDLE;
								input->GetHandSpace((vr::TrackedDeviceIndex_t)(h + 1), hs, true);
								if (hs == XR_NULL_HANDLE)
									return 0.0f;
								XrSpaceLocation hloc = { XR_TYPE_SPACE_LOCATION };
								if (XR_FAILED(xrLocateSpace(hs, xr_gbl->floorSpace, xr_gbl->GetBestTime(), &hloc))
								    || !(hloc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
									return 0.0f;
								p[h] = hloc.pose.position;
							}
							float pdx = p[0].x - p[1].x, pdy = p[0].y - p[1].y, pdz = p[0].z - p[1].z;
							return sqrtf(pdx * pdx + pdy * pdy + pdz * pdz);
						};

						int other = 1 - side;
						bool otherTrig = hasState[other] && (states[other].ulButtonPressed & vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Trigger)) != 0;
						if (!s_pinchActive && otherTrig && laserOnGrabBar[other]) {
							float d = handSeparation();
							if (d > 0.01f) {
								s_pinchActive = true;
								s_pinchBaseDist = d;
								s_pinchBaseScale = s_scalePercent;
								PlayPressSound();
							}
						}
						if (s_pinchActive) {
							if (!otherTrig) {
								s_pinchActive = false;
								SaveKeyboardSettings(); // persist the pinched scale
							} else {
								float d = handSeparation();
								if (d > 0.01f && s_pinchBaseDist > 0.01f) {
									int ns = (int)(s_pinchBaseScale * (d / s_pinchBaseDist) + 0.5f);
									if (ns < 50) ns = 50;
									if (ns > 150) ns = 150;
									if (ns != s_scalePercent) {
										s_scalePercent = ns;
										float sf = s_scalePercent / 100.0f;
										layer.size.width = 1.05f * sf;
										layer.size.height = 0.49f * sf;
									}
								}
							}
						}
					}

					// Prisma-style depth control: the NON-grabbing hand's thumbstick Y
					// pushes/pulls the keyboard along its facing normal (matches PrismaVR,
					// where the grabbing hand's stick is reserved). Clamped to arm's reach;
					// the grab plane moves with it so the in-plane slide stays consistent.
					{
						int depthHand = 1 - side;
						float stickY = hasState[depthHand] ? states[depthHand].rAxis[0].y : 0.0f;
						if (!s_pinchActive && fabsf(stickY) > 0.2f) {
							XrVector3f n;
							rotate_vector_by_quaternion({ 0, 0, 1 }, layer.pose.orientation, n);
							float step = stickY * 0.015f; // ~1.2 m/s at 90fps, half deflection
							XrVector3f newPos = {
								layer.pose.position.x - n.x * step,
								layer.pose.position.y - n.y * step,
								layer.pose.position.z - n.z * step
							};
							float ddx = newPos.x - headWorldPos.x;
							float ddz = newPos.z - headWorldPos.z;
							float dist = sqrtf(ddx * ddx + ddz * ddz);
							if (dist >= 0.35f && dist <= 1.6f) {
								grabPlaneOrigin.x += newPos.x - layer.pose.position.x;
								grabPlaneOrigin.y += newPos.y - layer.pose.position.y;
								grabPlaneOrigin.z += newPos.z - layer.pose.position.z;
								layer.pose.position = newPos;
							}
						}
					}
					// Intersect the laser ray with the ORIGINAL grab plane
					// (not the current keyboard position — avoids feedback lag)
					std::shared_ptr<BaseInput> input = GetBaseInput();
					if (input && input->AreActionsLoaded()) {
						XrSpace aimSpace = XR_NULL_HANDLE;
						input->GetHandSpace((vr::TrackedDeviceIndex_t)(side + 1), aimSpace, true);
						if (aimSpace != XR_NULL_HANDLE) {
							XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
							const XrTime grabSampleTime = xr_gbl->GetBestTime();
							if (XR_SUCCEEDED(xrLocateSpace(aimSpace, layer.space,
							        grabSampleTime, &loc))
							    && (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
							    && (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
								XrVector3f rayOrig = loc.pose.position;
								float originDown = oovr_laser_calibration::OriginDown(side);
								if (originDown != 0.0f) {
									XrVector3f downWorld;
									rotate_vector_by_quaternion(
									    { 0.0f, -1.0f, 0.0f }, loc.pose.orientation, downWorld);
									rayOrig.x += downWorld.x * originDown;
									rayOrig.y += downWorld.y * originDown;
									rayOrig.z += downWorld.z * originDown;
								}
								XrVector3f rayFwd = oovr_laser_calibration::LocalForward(side);
								XrVector3f rayDir;
								rotate_vector_by_quaternion(rayFwd, loc.pose.orientation, rayDir);
								if (!oovr_laser_smoothing::Filter(
								        oovr_laser_smoothing::Consumer::Keyboard, side, layer.space,
								        grabSampleTime, rayOrig, rayDir))
									rayDir = {};

								XrVector3f planeN;
								rotate_vector_by_quaternion({ 0, 0, 1 }, layer.pose.orientation, planeN);
								float d = xr_dot(rayDir, planeN);
								if (fabsf(d) > 1e-6f) {
									XrVector3f PO = {
										grabPlaneOrigin.x - rayOrig.x,
										grabPlaneOrigin.y - rayOrig.y,
										grabPlaneOrigin.z - rayOrig.z
									};
									float t = xr_dot(PO, planeN) / d;
									if (t > 0.0f && !s_pinchActive) { // position frozen while pinch-scaling
										XrVector3f hit = {
											rayOrig.x + t * rayDir.x,
											rayOrig.y + t * rayDir.y,
											rayOrig.z + t * rayDir.z
										};
										layer.pose.position = {
											hit.x + grabOffset.x,
											hit.y + grabOffset.y,
											hit.z + grabOffset.z
										};
									}
								}
							}
						}
					}
				}
			}
		}

		// Controller input (trigger for typing)
		for (int side = 0; side < 2; side++) {
			if (!hasState[side])
				continue;
			// InjectThumbstickAsDpad(states[side]); // Disabled — laser pointers handle selection now
			HandleOverlayInput(side == 0 ? vr::Eye_Left : vr::Eye_Right, states[side], time);
		}
	} else {
		// The input system disappeared while the overlay was alive.
		ReleaseAllHeldPCKeys();
		lastTriggerState[0] = lastTriggerState[1] = false;
		lastButtonState[0] = lastButtonState[1] = 0;
	}

	if (dirty) {
		dirty = false;
		Refresh();
		if (consoleActive)
			consoleDirty = true; // keyboard text changed, update console overlay
	}

	// Sync the console overlay to the REAL game console state (SKSE bridge).
	// The old design blind-toggled consoleActive on tilde and hoped every
	// keystroke landed; one lost/doubled toggle inverted the two state machines
	// permanently (overlay open + console closed, or the reverse). With the
	// bridge as source of truth the overlay self-corrects within a frame.
	// Bridge returns -1 when unavailable (no SKSE plugin / other games) and
	// the local toggle keeps working as before.
	if (GetTickCount64() >= s_consoleToggleGraceUntil) {
		extern int OCBridge_ConsoleState();
		int gameConsole = OCBridge_ConsoleState();
		// One-shot diagnostic: says whether the bridge console state is live
		// (-1 = bridge unavailable, 0/1 = real console state)
		static bool s_loggedBridgeProbe = false;
		if (!s_loggedBridgeProbe) {
			s_loggedBridgeProbe = true;
			OOVR_DEBUG_LOGF("Console sync: first bridge probe = %d", gameConsole);
		}
		if (gameConsole >= 0 && (gameConsole != 0) != consoleActive) {
			consoleActive = (gameConsole != 0);
			if (consoleActive) {
				text.clear();
				cursorPos = 0;
			}
			consoleDirty = true;
			OOVR_DEBUG_LOGF("Console overlay synced to game console state: %s",
			    consoleActive ? "OPEN" : "CLOSED");
		}
	}

	// Console INPUT overlay — position above keyboard, refresh when needed
	if (consoleActive && consoleChain != XR_NULL_HANDLE) {
		// Position directly above the keyboard with a small gap
		XrVector3f localUp = { 0, 1, 0 };
		XrVector3f worldUp;
		rotate_vector_by_quaternion(localUp, layer.pose.orientation, worldUp);

		float gap = 0.02f; // 2cm gap
		float kbHalfH = layer.size.height * 0.5f;
		float conHalfH = consoleLayer.size.height * 0.5f;
		float offset = kbHalfH + gap + conHalfH;

		consoleLayer.pose.position = {
		    layer.pose.position.x + worldUp.x * offset,
		    layer.pose.position.y + worldUp.y * offset,
		    layer.pose.position.z + worldUp.z * offset
		};
		consoleLayer.pose.orientation = layer.pose.orientation;
		consoleLayer.space = layer.space;

		// Blinking cursor forces periodic redraw
		bool cursorBlink = ((GetTickCount64() / 500) % 2) == 0;
		static bool lastConsoleBlink = false;
		if (cursorBlink != lastConsoleBlink) {
			lastConsoleBlink = cursorBlink;
			consoleDirty = true;
		}

		if (consoleDirty) {
			consoleDirty = false;
			RefreshConsole();
		}
	}

	// Target mode dots — position them 3m ahead of controllers and headset
	if (s_targetMode) {
		std::shared_ptr<BaseInput> input = GetBaseInput();
		if (input && input->AreActionsLoaded()) {
			// Controller dots (0 and 1)
			for (int side = 0; side < 2; side++) {
				const XrTime targetSampleTime = xr_gbl->GetBestTime();
				XrSpace aimSpace = XR_NULL_HANDLE;
				input->GetHandSpace((vr::TrackedDeviceIndex_t)(side + 1), aimSpace, true);
				if (aimSpace == XR_NULL_HANDLE) {
					oovr_laser_smoothing::Reset(
					    oovr_laser_smoothing::Consumer::KeyboardTarget,
					    side, xr_gbl->floorSpace);
					continue;
				}
				XrSpaceLocation location = { XR_TYPE_SPACE_LOCATION };
				XrResult result = xrLocateSpace(aimSpace, xr_gbl->floorSpace,
				    targetSampleTime, &location);
				if (XR_FAILED(result)
				    || !(location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
				    || !(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
					oovr_laser_smoothing::Reset(
					    oovr_laser_smoothing::Consumer::KeyboardTarget,
					    side, xr_gbl->floorSpace);
					continue;
				}
						// Project 3 meters forward from controller
						XrVector3f fwd = oovr_laser_calibration::LocalForward(side);
						XrVector3f dir;
						rotate_vector_by_quaternion(fwd, location.pose.orientation, dir);
						XrVector3f origin = location.pose.position;
						float originDown = oovr_laser_calibration::OriginDown(side);
						if (originDown != 0.0f) {
							XrVector3f downWorld;
							rotate_vector_by_quaternion(
							    { 0.0f, -1.0f, 0.0f }, location.pose.orientation, downWorld);
							origin.x += downWorld.x * originDown;
							origin.y += downWorld.y * originDown;
							origin.z += downWorld.z * originDown;
						}
						if (oovr_laser_smoothing::Filter(
						        oovr_laser_smoothing::Consumer::KeyboardTarget, side,
						        xr_gbl->floorSpace, targetSampleTime, origin, dir)) {
							targetDotLayer[side].pose.position = {
								origin.x + dir.x * 3.0f,
								origin.y + dir.y * 3.0f,
								origin.z + dir.z * 3.0f
							};
							targetDotLayer[side].pose.orientation = { 0.0f, 0.0f, 0.0f, 1.0f };
							targetDotLayer[side].space = xr_gbl->floorSpace;
							targetDotValid[side] = true;
						}
			}
			// Headset dot (index 2)
			XrSpaceLocation headLoc = { XR_TYPE_SPACE_LOCATION };
			XrResult headResult = xrLocateSpace(xr_gbl->viewSpace, xr_gbl->floorSpace,
			    xr_gbl->GetBestTime(), &headLoc);
			if (XR_SUCCEEDED(headResult)
			    && (headLoc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
			    && (headLoc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
				// Project 3 meters forward from headset
				XrVector3f fwd = { 0.0f, 0.0f, -1.0f };
				XrVector3f dir;
				rotate_vector_by_quaternion(fwd, headLoc.pose.orientation, dir);
				targetDotLayer[2].pose.position = {
					headLoc.pose.position.x + dir.x * 3.0f,
					headLoc.pose.position.y + dir.y * 3.0f,
					headLoc.pose.position.z + dir.z * 3.0f
				};
				targetDotLayer[2].pose.orientation = { 0.0f, 0.0f, 0.0f, 1.0f };
				targetDotLayer[2].space = xr_gbl->floorSpace;
				targetDotValid[2] = true;
			}
		} else {
			for (int side = 0; side < 2; ++side) {
				oovr_laser_smoothing::Reset(
				    oovr_laser_smoothing::Consumer::KeyboardTarget,
				    side, xr_gbl->floorSpace);
			}
		}
	}

	// Build layer list: console (behind), keyboard, laser beams, target dots, crosshair (on top)
	if (consoleActive && consoleChain != XR_NULL_HANDLE)
		activeLayers.push_back((XrCompositionLayerBaseHeader*)&consoleLayer);
	activeLayers.push_back((XrCompositionLayerBaseHeader*)&layer);
	for (int side = 0; side < 2; side++) {
		if (laserActive[side]) {
			// Match the menu pointer: idle is warm white; the complete trigger
			// hold is electric blue, including PC-mode long key presses.
			laserLayer[side].subImage.imageRect = laserAtlas->GetImageRect(
			    lastTriggerState[side] ? LaserAtlasRegion::BeamClicked : LaserAtlasRegion::BeamIdle);
			activeLayers.push_back((XrCompositionLayerBaseHeader*)&laserLayer[side]);
			activeLayers.push_back((XrCompositionLayerBaseHeader*)&cursorDotLayer[side]);
		}
	}
	if (s_targetMode) {
		for (int i = 0; i < 3; i++) {
			if (targetDotValid[i] && laserAtlas)
				activeLayers.push_back((XrCompositionLayerBaseHeader*)&targetDotLayer[i]);
		}
	}
	if (crosshairVisible && crosshairChain != XR_NULL_HANDLE)
		activeLayers.push_back((XrCompositionLayerBaseHeader*)&crosshairLayer);

	return activeLayers;
}

// Returns: key ID (>= 0), -1 (miss), -2 (grab bar drag area), -3 (toggle button)
int VRKeyboard::HitTestLaser(int side)
{
	laserActive[side] = false;
	laserOnGrabBar[side] = false;
	laserOnToggle[side] = false;
	laserOnConsole[side] = false;
	laserOnTextBar[side] = false;
	laserOnTiltUp[side] = false;
	laserOnTiltDown[side] = false;
	laserOnOpacityUp[side] = false;
	laserOnOpacityDown[side] = false;
	laserOnSizeUp[side] = false;
	laserOnSizeDown[side] = false;

	std::shared_ptr<BaseInput> input = GetBaseInput();
	if (!input || !input->AreActionsLoaded()) {
		oovr_laser_smoothing::Reset(
		    oovr_laser_smoothing::Consumer::Keyboard, side, layer.space);
		return -1;
	}

	XrSpace aimSpace = XR_NULL_HANDLE;
	input->GetHandSpace((vr::TrackedDeviceIndex_t)(side + 1), aimSpace, true);
	if (aimSpace == XR_NULL_HANDLE) {
		oovr_laser_smoothing::Reset(
		    oovr_laser_smoothing::Consumer::Keyboard, side, layer.space);
		return -1;
	}

	// Locate controller in the keyboard's reference space (viewSpace or floorSpace)
	XrSpaceLocation location = { XR_TYPE_SPACE_LOCATION };
	const XrTime sampleTime = xr_gbl->GetBestTime();
	XrResult result = xrLocateSpace(aimSpace, layer.space, sampleTime, &location);

	if (XR_FAILED(result)
	    || !(location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
	    || !(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
		oovr_laser_smoothing::Reset(
		    oovr_laser_smoothing::Consumer::Keyboard, side, layer.space);
		return -1;
	}

	XrVector3f rayOrigin = location.pose.position;
	float originDown = oovr_laser_calibration::OriginDown(side);
	if (originDown != 0.0f) {
		XrVector3f downWorld;
		rotate_vector_by_quaternion(
		    { 0.0f, -1.0f, 0.0f }, location.pose.orientation, downWorld);
		rayOrigin.x += downWorld.x * originDown;
		rayOrigin.y += downWorld.y * originDown;
		rayOrigin.z += downWorld.z * originDown;
	}
	XrVector3f fwd = oovr_laser_calibration::LocalForward(side);
	XrVector3f rayDir;
	rotate_vector_by_quaternion(fwd, location.pose.orientation, rayDir);
	if (!oovr_laser_smoothing::Filter(
	        oovr_laser_smoothing::Consumer::Keyboard, side, layer.space,
	        sampleTime, rayOrigin, rayDir))
		return -1;

	// Oriented plane intersection — keyboard can face any direction in world space
	XrVector3f kbCenter = layer.pose.position;
	XrVector3f planeNormal, localRight, localUp;
	rotate_vector_by_quaternion({ 0, 0, 1 }, layer.pose.orientation, planeNormal);
	rotate_vector_by_quaternion({ 1, 0, 0 }, layer.pose.orientation, localRight);
	rotate_vector_by_quaternion({ 0, 1, 0 }, layer.pose.orientation, localUp);

	float denom = xr_dot(rayDir, planeNormal);
	if (fabsf(denom) < 1e-6f)
		return -1; // ray parallel to keyboard plane

	XrVector3f PO = { kbCenter.x - rayOrigin.x, kbCenter.y - rayOrigin.y, kbCenter.z - rayOrigin.z };
	float t = xr_dot(PO, planeNormal) / denom;
	if (t <= 0.0f)
		return -1; // intersection behind the ray

	XrVector3f hitPoint = {
		rayOrigin.x + t * rayDir.x,
		rayOrigin.y + t * rayDir.y,
		rayOrigin.z + t * rayDir.z
	};

	// Project hit onto keyboard local axes for UV coordinates
	XrVector3f HP = { hitPoint.x - kbCenter.x, hitPoint.y - kbCenter.y, hitPoint.z - kbCenter.z };
	float localXCoord = xr_dot(HP, localRight);
	float localYCoord = xr_dot(HP, localUp);

	float u = (localXCoord + layer.size.width * 0.5f) / layer.size.width;
	float v = (localYCoord + layer.size.height * 0.5f) / layer.size.height;

	if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f)
		return -1; // missed the quad

	// Store laser data for beam rendering and cursor dot
	laserActive[side] = true;
	laserOrigin[side] = rayOrigin;
	laserHitPoint[side] = hitPoint;
	laserU[side] = u;
	laserV[side] = v;

	// Convert to pixel coordinates (texture Y is flipped: top of quad = top of texture)
	int texX = (int)(u * texWidth);
	int texY = (int)((1.0f - v) * texHeight);

	int padding = 6;
	int marginH = 120;
	int marginTop = 60;

	// Settings controls render after the keyboard's other content, so their
	// visible arrow rectangles must receive the same topmost hit-test priority.
	// Authored layouts may move an arrow over the invisible MOVE strip, text bar,
	// or key area; those underlying regions must not steal the click.
	{
		const int hitPad = 14;
		auto containsArrow = [&](int groupLeft, int groupTop,
		                         const KeyboardLayout::ControlDesign& design, bool up) {
			const float partWidth = up ? design.upWidth : design.downWidth;
			const float partHeight = up ? design.upHeight : design.downHeight;
			const float offsetX = up ? design.upOffsetX : design.downOffsetX;
			const float offsetY = up ? design.upOffsetY : design.downOffsetY;
			const int left = groupLeft + int(std::round((design.width - partWidth) / 2.0f + offsetX));
			const int top = groupTop + int(std::round(up ? offsetY : design.height - partHeight + offsetY));
			const int width = std::max(4, int(std::round(partWidth)));
			const int height = std::max(4, int(std::round(partHeight)));
			return texX >= left - hitPad && texX < left + width + hitPad
			    && texY >= top - hitPad && texY < top + height + hitPad;
		};

		const int rightBaseX = int(texWidth) - 106;
		const int opacityLeft = rightBaseX + int(std::round(layout->GetOpacityControlOffsetX()));
		const int opacityTop = 60 + int(std::round(layout->GetOpacityControlOffsetY()));
		const auto& opacityDesign = layout->GetOpacityControlDesign();
		if (containsArrow(opacityLeft, opacityTop, opacityDesign, true)) {
			laserOnOpacityUp[side] = true;
			return -8;
		}
		if (containsArrow(opacityLeft, opacityTop, opacityDesign, false)) {
			laserOnOpacityDown[side] = true;
			return -9;
		}

		const int tiltLeft = rightBaseX + int(std::round(layout->GetTiltControlOffsetX()));
		const int tiltTop = 270 + int(std::round(layout->GetTiltControlOffsetY()));
		const auto& tiltDesign = layout->GetTiltControlDesign();
		if (containsArrow(tiltLeft, tiltTop, tiltDesign, true)) {
			laserOnTiltUp[side] = true;
			return -6;
		}
		if (containsArrow(tiltLeft, tiltTop, tiltDesign, false)) {
			laserOnTiltDown[side] = true;
			return -7;
		}

		const int sizeLeft = 39 + int(std::round(layout->GetSizeControlOffsetX()));
		const int sizeTop = 224 + int(std::round(layout->GetSizeControlOffsetY()));
		const auto& sizeDesign = layout->GetSizeControlDesign();
		if (containsArrow(sizeLeft, sizeTop, sizeDesign, true)) {
			laserOnSizeUp[side] = true;
			return -10;
		}
		if (containsArrow(sizeLeft, sizeTop, sizeDesign, false)) {
			laserOnSizeDown[side] = true;
			return -11;
		}
	}

	// Check invisible grab bar region (top strip of texture) — all drag
	if (texY < GRAB_BAR_HEIGHT) {
		laserOnGrabBar[side] = true;
		return -2; // grab bar drag area
	}

	// Check button strip (just above text bar) — MODE / LOCK
	{
		int textBarY = GRAB_BAR_HEIGHT + marginTop;
		int btnH = 32;
		int btnGap = 4;
		int btnStripY = textBarY - btnH - btnGap;

		// Per-button rects, including per-theme art nudges (must match Refresh)
		const auto& modeDesign = layout->GetModeButtonDesign();
		const auto& lockDesign = layout->GetLockButtonDesign();
		int modeBtnW = modeDesign.width > 0 ? std::max(8, int(std::round(modeDesign.width))) : CONSOLE_BTN_WIDTH;
		int modeBtnH = modeDesign.height > 0 ? std::max(8, int(std::round(modeDesign.height))) : btnH;
		int lockBtnW = lockDesign.width > 0 ? std::max(8, int(std::round(lockDesign.width))) : TOGGLE_BTN_WIDTH;
		int lockBtnH = lockDesign.height > 0 ? std::max(8, int(std::round(lockDesign.height))) : btnH;
		int modeBtnX = marginH + theme->modeBtnOffX + int(std::round(layout->GetModeButtonOffsetX()));
		int modeBtnY = btnStripY + theme->modeBtnOffY + int(std::round(layout->GetModeButtonOffsetY()));
		int lockBtnX = (int)texWidth - marginH - TOGGLE_BTN_WIDTH + theme->lockBtnOffX + int(std::round(layout->GetLockButtonOffsetX()));
		int lockBtnY = btnStripY + theme->lockBtnOffY + int(std::round(layout->GetLockButtonOffsetY()));

		if (texX >= modeBtnX && texX < modeBtnX + modeBtnW
		    && texY >= modeBtnY && texY < modeBtnY + modeBtnH) {
			laserOnConsole[side] = true;
			return -5; // mode button hit
		}
		if (texX >= lockBtnX && texX < lockBtnX + lockBtnW
		    && texY >= lockBtnY && texY < lockBtnY + lockBtnH) {
			laserOnToggle[side] = true;
			return -3; // toggle button hit
		}
		if (texY >= btnStripY && texY < btnStripY + btnH) {
			// Everything else in the button strip is MOVE drag
			laserOnGrabBar[side] = true;
			return -2; // drag area
		}
	}

	// Hit-test text input bar (only in non-minimal mode)
	int kbWidth = layout->GetWidth();
	int availW = (int)texWidth - 2 * marginH;
	int keySize = ((availW - padding) / kbWidth) - padding;
	if (!minimal) {
		const auto& textBarDesign = layout->GetTextBarDesign();
		int textBarX = marginH + int(std::round(layout->GetTextBarOffsetX()));
		int textBarY = GRAB_BAR_HEIGHT + marginTop + int(std::round(layout->GetTextBarOffsetY()));
		int textBarW = textBarDesign.width > 0 ? std::max(8, int(std::round(textBarDesign.width))) : availW;
		int textBarH = textBarDesign.height > 0 ? std::max(8, int(std::round(textBarDesign.height))) : keySize;
		if (texY >= textBarY && texY < textBarY + textBarH
		    && texX >= textBarX && texX < textBarX + textBarW) {
			laserOnTextBar[side] = true;
			return -4; // text bar hit
		}
	}

	// Hit-test against keyboard keys (shifted down by grab bar height)
	int keyAreaBaseY = (minimal ? marginTop : marginTop + keySize + padding) + GRAB_BAR_HEIGHT;

	for (const auto& key : layout->GetKeymap()) {
		int kx = marginH + (int)((keySize + padding) * key.x);
		int ky = keyAreaBaseY + (int)((keySize + padding) * key.y);
		int kw = (int)(keySize * key.w);
		int kh = (int)(keySize * key.h);
		if (key.spansToRight)
			kw = (int)texWidth - marginH - kx;

		if (texX >= kx && texX < kx + kw && texY >= ky && texY < ky + kh)
			return key.id;
	}

	// Any empty parchment area is draggable
	laserOnGrabBar[side] = true;
	return -2;
}

void VRKeyboard::HandleOverlayInput(vr::EVREye side, vr::VRControllerState_t state, float time)
{
	using namespace vr;

	// In case this is somehow called after the keyboard is closed, ignore it
	if (IsClosed())
		return;

	uint64_t lastButtons = lastButtonState[side];
	lastButtonState[side] = state.ulButtonPressed;

#define GET_BTTN(var, key) bool var = state.ulButtonPressed & ButtonMaskFromId(key)
#define GET_BTTN_LAST(var, key) \
	GET_BTTN(var, key);         \
	bool var##_last = lastButtons & ButtonMaskFromId(key)
	// DPad navigation disabled — laser pointers handle selection now
	// GET_BTTN(left, k_EButton_DPad_Left);
	// GET_BTTN(right, k_EButton_DPad_Right);
	// GET_BTTN(up, k_EButton_DPad_Up);
	// GET_BTTN(down, k_EButton_DPad_Down);
	GET_BTTN_LAST(trigger, k_EButton_SteamVR_Trigger);
	GET_BTTN_LAST(grip, k_EButton_Grip);
#undef GET_BTTN
#undef GET_BTTN_LAST

	// The release belongs to the key chosen on trigger-down, regardless of
	// where the laser is pointing now.
	if (!trigger && trigger_last)
		ReleaseHeldPCKey((int)side);

	if (grip && !grip_last && !grabActive) {
		// If console overlay is active, close the real console too
		if (consoleActive) {
			if (!RequestGameConsole(false)) {
				VkMapping mapping = CharToVK(L'`');
				if (mapping.vk != 0)
					SendVirtualKey(mapping.vk, mapping.needsShift, 0, true, sendInputOnly || consoleActive);
			}
			consoleActive = false;
			s_consoleToggleGraceUntil = GetTickCount64() + 700;
			consoleDirty = true;
			OOVR_DEBUG_LOG("Grip pressed with console open — requested console hide");
		}
		if (!sendInputOnly) {
			// Send Escape to dismiss SkyUI text input dialogs (only when game opened the keyboard)
			SendSingleVK(VK_ESCAPE, sendInputOnly || consoleActive);
			PostCharToGame(VK_ESCAPE, 1);
			SubmitEvent(VREvent_KeyboardClosed, 0);
		}
		// When opened via controller shortcut (sendInputOnly), just close — no Escape needed
		ReleaseAllHeldPCKeys();
		closed = true;
		return;
	}

	if (selected[side] < 0) {
		if (heldPCKeys[side].vk == 0)
			s_pressedKey[side] = -1; // Preserve the latched PC key while held
		return; // No key selected — nothing to do
	}

	const KeyboardLayout::Key& key = layout->GetKeymap()[selected[side]];

	// Track pressed state for visual feedback
	if (heldPCKeys[side].vk != 0) {
		s_pressedKey[side] = heldPCKeys[side].keyId;
	} else if (trigger && laserActive[(int)side]) {
		s_pressedKey[side] = selected[side];
	} else {
		s_pressedKey[side] = -1;
	}

	// Backspace hold-to-repeat (like arrows)
	// Fires on initial press, then repeats every 150ms while held
	{
		static ULONGLONG backspaceRepeatNext[2] = {};
		static bool backspaceRepeating[2] = {};

		wchar_t ch = caseMode == ECaseMode::LOWER ? key.ch : key.shift;
		bool onBackspace = (ch == '\b') && laserActive[(int)side]
		    && !laserOnGrabBar[(int)side] && !laserOnToggle[(int)side]
		    && !laserOnConsole[(int)side] && !laserOnTextBar[(int)side]
		    && !laserOnTiltUp[(int)side] && !laserOnTiltDown[(int)side]
		    && !laserOnOpacityUp[(int)side] && !laserOnOpacityDown[(int)side]
		    && !laserOnSizeUp[(int)side] && !laserOnSizeDown[(int)side];

		ULONGLONG now = GetTickCount64();
		bool trigJustPressed = trigger && !trigger_last;
		bool shouldFireBackspace = false;

		bool useHeldPCKey = sendInputOnly && !consoleActive;
		if (trigJustPressed && onBackspace && !useHeldPCKey) {
			shouldFireBackspace = true;
			backspaceRepeating[side] = true;
			backspaceRepeatNext[side] = now + 400; // initial delay before repeat
		} else if (trigger && backspaceRepeating[side] && onBackspace) {
			if (now >= backspaceRepeatNext[side]) {
				shouldFireBackspace = true;
				backspaceRepeatNext[side] = now + 100; // repeat interval (faster than arrows)
			}
		}
		if (useHeldPCKey || !trigger || !onBackspace) {
			backspaceRepeating[side] = false;
		}

		if (shouldFireBackspace && !trigJustPressed) {
			// Repeat fire — play sound/haptic and send backspace
			PlayPressSound();
			TriggerHaptic((int)side);
			SendSingleVK(VK_BACK, sendInputOnly || consoleActive);
			if (!consoleActive) PostCharToGame(VK_BACK, 1);
			if ((!sendInputOnly || consoleActive) && cursorPos > 0 && !text.empty()) {
				text.erase(cursorPos - 1, 1);
				cursorPos--;
				if (consoleActive) consoleDirty = true;
			}
		}
	}

	// Only fire key presses when laser is actively hitting the keyboard quad
	// and not on grab bar buttons, toggle, console, or text bar.
	// Without the laserActive check, triggering while pointing at a spell wheel
	// or other UI would accidentally fire the last-hovered key.
	if (trigger && !trigger_last && laserActive[(int)side]
	    && !laserOnGrabBar[(int)side] && !laserOnToggle[(int)side]
	    && !laserOnConsole[(int)side] && !laserOnTextBar[(int)side]
	    && !laserOnTiltUp[(int)side] && !laserOnTiltDown[(int)side]
	    && !laserOnOpacityUp[(int)side] && !laserOnOpacityDown[(int)side]
	    && !laserOnSizeUp[(int)side] && !laserOnSizeDown[(int)side]) {
		// Play press sound and haptic when key is actually activated
		PlayPressSound();
		TriggerHaptic((int)side);

		wchar_t ch = caseMode == ECaseMode::LOWER ? key.ch : key.shift;

		if (sendInputMode) {
			// ── SendInput mode: inject Windows keystrokes + buffer text for GetKeyboardText ──
#ifdef _WIN32
			auto sendHoldableControl = [&](WORD vk) {
				if (sendInputOnly && !consoleActive) {
					const bool consumeCtrl = ctrlLatched;
					PressHeldPCKey((int)side, key.id, vk, false, false);
					if (consumeCtrl)
						releaseCtrlAfterHeldPCKey[(int)side] = true;
				} else {
					SendSingleVK(vk, sendInputOnly || consoleActive);
				}
			};

			if (ch == '\x01' || ch == '\x02') {
				ECaseMode target = ch == '\x02' ? ECaseMode::LOCK : ECaseMode::SHIFT;
				caseMode = caseMode == target ? ECaseMode::LOWER : target;
			} else if (ch == '\b') {
				sendHoldableControl(VK_BACK);
				if (!consoleActive && !ctrlLatched) PostCharToGame(VK_BACK, 1); // Ctrl chords use the scancode path only
				// Update internal buffer (game-opened keyboard OR console mode)
				if ((!sendInputOnly || consoleActive) && cursorPos > 0 && !text.empty()) {
					text.erase(cursorPos - 1, 1);
					cursorPos--;
					if (consoleActive) consoleDirty = true;
				}
			} else if (ch == '\x03' || (ch == '\n' && !sendInputOnly && !consoleActive)) {
				// The physical Enter key is contextual. A game-opened keyboard needs
				// OpenVR's KeyboardDone event so Skyrim can consume GetKeyboardText;
				// a player-opened keyboard still receives an ordinary Enter below.
				// Keep the legacy 0x03 Done code for custom layouts.
				if (sendInputOnly) {
					// PC mode: text was injected via SendInput/GFx — confirm with Enter
					SendSingleVK(VK_RETURN, true);
					if (!consoleActive) PostCharToGame(VK_RETURN, 1);
					constexpr UINT WM_OC_KB = WM_APP + 0x4F43;
					HWND hwnd = GetGameWindow();
					if (hwnd) PostMessageW(hwnd, WM_OC_KB, 1, 0);
				} else {
					// VR mode: game opened keyboard via ShowKeyboard — submit text
					// via KeyboardDone event + doneCallback. Don't inject VK_RETURN
					// as it dismisses the Scaleform dialog before the SKSE plugin
					// callback can read GetKeyboardText.
					SubmitEvent(vr::VREvent_KeyboardDone, 0);
				}
				ReleaseAllHeldPCKeys();
				closed = true;
			} else if (ch == '\t') {
				sendHoldableControl(VK_TAB);
				if (!ctrlLatched) PostCharToGame(VK_TAB, 1); // ordinary Scaleform focus movement
			} else if (ch == '\n') {
				sendHoldableControl(VK_RETURN);
				if (!consoleActive && !ctrlLatched) PostCharToGame(VK_RETURN, 1); // Ctrl chords use the scancode path only
				if (consoleActive) {
					text.clear();
					cursorPos = 0;
					consoleDirty = true;
				}
			} else if (ch == '\x04') {
				sendHoldableControl(VK_UP);
				// Arrows produce no WM_CHAR and posted WM_KEYDOWN never reaches
				// DirectInput, so Scaleform text boxes (console, naming, SkyUI
				// search) only see arrows via the GFxKeyEvent path. Without it
				// the caret cannot move and console history is unreachable.
				if (!ctrlLatched) PostCharToGame(VK_UP, 1);
			} else if (ch == '\x05') {
				sendHoldableControl(VK_DOWN);
				if (!ctrlLatched) PostCharToGame(VK_DOWN, 1);
			} else if (ch == '\x06') {
				sendHoldableControl(VK_LEFT);
				if (!ctrlLatched) PostCharToGame(VK_LEFT, 1);
				// Keep the keyboard's own preview caret in step
				if (cursorPos > 0) {
					cursorPos--;
					if (consoleActive) consoleDirty = true;
				}
			} else if (ch == '\x07') {
				sendHoldableControl(VK_RIGHT);
				if (!ctrlLatched) PostCharToGame(VK_RIGHT, 1);
				if (cursorPos < (int)text.size()) {
					cursorPos++;
					if (consoleActive) consoleDirty = true;
				}
			} else if (ch >= '\x10' && ch <= '\x1B') {
				// F1-F12 keys: \x10=F1, \x11=F2, ..., \x1B=F12
				int fNum = (ch - '\x10') + 1;
				WORD vk = VK_F1 + (fNum - 1);
				sendHoldableControl(vk);
			} else if (ch == '\x0F') {
				// [M] key — toggle crosshair dot at gaze center
				crosshairVisible = !crosshairVisible;
				OOVR_DEBUG_LOGF("Crosshair: %s", crosshairVisible ? "ON" : "OFF");
			} else if (ch == '\x1C') {
				// [T] key — toggle target mode (show dots from controllers and headset)
				s_targetMode = !s_targetMode;
				OOVR_DEBUG_LOGF("Target mode: %s", s_targetMode ? "ON" : "OFF");
			} else if (ch == '\x1D') {
				sendHoldableControl(VK_END);
			} else if (ch == '\x1E') {
				if (sendInputOnly && !consoleActive)
					ToggleCtrlLatch((int)side);
				else
					SendSingleVK(VK_CONTROL, sendInputOnly || consoleActive);
			} else if (ch == '\x1F') {
				// Print Screen is a complete press/release action, not a modifier.
				const bool consumeCtrl = ctrlLatched;
				SendSingleVK(VK_SNAPSHOT, true);
				if (consumeCtrl)
					ReleaseCtrlLatch();
			} else if (ch == '\x0E') {
				// ESC — send to SkyUI/menus to cancel text input (does NOT close keyboard)
				sendHoldableControl(VK_ESCAPE);
			} else {
				// Tilde/backtick toggles console INPUT overlay
				if (ch == L'`' || ch == L'~') {
					consoleActive = !consoleActive;
					s_consoleToggleGraceUntil = GetTickCount64() + 700;
					if (consoleActive) {
						text.clear();
						cursorPos = 0;
					}
					consoleDirty = true;
					OOVR_DEBUG_LOGF("Console overlay: %s", consoleActive ? "OPENED" : "CLOSED");
					// Explicit show/hide via the SKSE plugin (idempotent, no
					// keystroke, immune to double-toggle). Keystroke fallback
					// only when the plugin window is unavailable.
					if (!RequestGameConsole(consoleActive))
						SendSingleVK(VK_OEM_3, sendInputOnly || consoleActive);
				} else if (consoleActive) {
					// Console mode: ONLY PostCharToGame — no scancodes at all.
					// Scancodes produce WM_CHAR via TranslateMessage which doubles in console.
					PostCharToGame(ch);
				} else if (!sendInputOnly) {
					// Game-opened keyboard (ShowKeyboard / enchanting / naming):
					// Only PostCharToGame — no scancodes. Scancodes trigger Skyrim's
					// keyboard-mode detection which disables VR controllers permanently.
					// Respect maxLength — stop sending chars to game when limit reached
					if (maxLength == 0 || text.length() < maxLength) {
						PostCharToGame(ch);
					}
				} else {
					// Player-opened keyboard during gameplay (sendInputOnly=true):
					// Scancodes for DirectInput/MCM hotkeys + PostCharToGame for
					// Scaleform text input (SkyUI search, MCM text fields).
					// ch != 0 suppresses VK events (no double WM_CHAR from VK path).
					// WM_CHAR from scancodes is blocked by SKSE WndProc hook
					// (OC_KB_ACTIVE property) to prevent double entry.
					VkMapping mapping = CharToVK(ch);
					if (mapping.vk != 0) {
						const bool consumeCtrl = ctrlLatched;
						PressHeldPCKey((int)side, key.id, mapping.vk, mapping.needsShift, true);
						if (consumeCtrl)
							releaseCtrlAfterHeldPCKey[(int)side] = true;
						else
							PostCharToGame(ch);
					}
				}
				// Buffer character for display (game-opened keyboard OR console mode)
				// Skip tilde itself — it's a toggle, not console input
				if (ch != L'`' && ch != L'~') {
					if (!sendInputOnly || consoleActive) {
						// Respect maxLength to prevent buffer overflows in games
						if (maxLength == 0 || text.length() < maxLength) {
							text.insert(cursorPos, 1, ch);
							cursorPos++;
						}
					}
					if (consoleActive) consoleDirty = true;
				}
				if (caseMode == ECaseMode::SHIFT)
					caseMode = ECaseMode::LOWER;
			}
#endif
		} else {
			// ── Normal mode: buffer text for GetKeyboardText ──
			bool submitKeyEvent = false;

			if (ch == '\x01' || ch == '\x02') {
				ECaseMode target = ch == '\x02' ? ECaseMode::LOCK : ECaseMode::SHIFT;
				caseMode = caseMode == target ? ECaseMode::LOWER : target;
			} else if (ch == '\b') {
				if (cursorPos > 0 && !text.empty()) {
					text.erase(cursorPos - 1, 1);
					cursorPos--;
				}
				submitKeyEvent = true;
			} else if (ch == '\x03' || ch == '\n') {
				// Normal mode is game-owned text entry. Both the shared Enter key
				// and the legacy Done code complete the OpenVR keyboard request.
				if (inputMode != EGamepadTextInputMode::k_EGamepadTextInputModeSubmit)
					closed = true;
				if (!minimal)
					SubmitEvent(VREvent_KeyboardCharInput, 0);
				SubmitEvent(VREvent_KeyboardDone, 0);
			} else if (ch == '\x06') {
				// Left arrow — move the buffer caret (edits happen at cursorPos)
				if (cursorPos > 0)
					cursorPos--;
			} else if (ch == '\x07') {
				// Right arrow
				if (cursorPos < (int)text.size())
					cursorPos++;
			} else if (ch == '\x04' || ch == '\x05') {
				// Up/Down — no-op in normal mode
			} else if (ch == '\x1E') {
#ifdef _WIN32
				ToggleCtrlLatch((int)side);
#endif
			} else if (ch == '\x1F') {
#ifdef _WIN32
				SendSingleVK(VK_SNAPSHOT, true);
#endif
			} else if (ch == '\x0E') {
				closed = true;
				SubmitEvent(VREvent_KeyboardClosed, 0);
				return;
			} else if (!minimal && ch == '\t') {
				// Silently soak up tabs
			} else {
				// Respect maxLength to prevent buffer overflows in games
				if (maxLength == 0 || text.length() < maxLength) {
					text.insert(cursorPos, 1, ch);
					cursorPos++;
					submitKeyEvent = true;
					if (caseMode == ECaseMode::SHIFT)
						caseMode = ECaseMode::LOWER;
				}
			}

			if (submitKeyEvent) {
				SubmitEvent(VREvent_KeyboardCharInput, minimal ? ch : 0);
			}
		}

		dirty = true;
	}

	// DPad movement disabled — laser pointers handle selection now
	// Kept for future reference:
	/*
	bool any = left || right || up || down;
	if (!any) {
	cancel:
		repeatCount[side] = 0;
		lastInputTime[side] = 0;
		return;
	}

	if (time - lastInputTime[side] < (repeatCount[side] <= 1 ? 0.3 : 0.1))
		return;

	lastInputTime[side] = time;
	repeatCount[side]++;

	int target = -1;
	if (left)
		target = key.toLeft;
	else if (right)
		target = key.toRight;
	else if (up)
		target = key.toUp;
	else if (down)
		target = key.toDown;

	if (target == -1) {
		goto cancel;
	}

	selected[side] = target;
	dirty = true;
	*/
}

void VRKeyboard::SetTransform(vr::HmdMatrix34_t transform)
{
	layer.pose = S2O_om34_pose(transform);
}

struct pix_t {
	uint8_t r, g, b, a;
};

static_assert(sizeof(pix_t) == 4, "padded pix_t");

void VRKeyboard::LoadThemeAssets()
{
	const std::string& configTheme = oovr_global_configuration.KbTheme();
	const std::string& configFont = oovr_global_configuration.KbFont();
	// A Keyboard Studio design is a portable object: theme and font travel in
	// OCUKeyboard.kb rather than forcing a shared mod to replace the user's INI.
	const std::string want = layout && !layout->GetBaseTheme().empty()
	    ? layout->GetBaseTheme()
	    : configTheme;
	const std::string wantFont = layout && !layout->GetFontName().empty()
	    ? layout->GetFontName()
	    : configFont;

	theme = &K_THEMES[0];
	for (const KbThemeDef& t : K_THEMES) {
		if (want == t.name) {
			theme = &t;
			break;
		}
	}
	if (want != theme->name)
		OOVR_LOGF("Keyboard theme '%s' unknown, falling back to '%s'", want.c_str(), theme->name);

	int fontResource = theme->fontRes;
	std::string resolvedFont = "theme";
	bool loadedExternalFont = false;
	if (wantFont.rfind("custom_", 0) == 0) {
		auto readFontFile = [&](const wchar_t* name, size_t maximumBytes) {
			std::vector<char> bytes;
			std::wstring path = GetOCDllDirectory() + name;
			FILE* file = _wfopen(path.c_str(), L"rb");
			if (!file) return bytes;
			fseek(file, 0, SEEK_END);
			long length = ftell(file);
			fseek(file, 0, SEEK_SET);
			if (length > 0 && size_t(length) <= maximumBytes) {
				bytes.resize(size_t(length));
				if (fread(bytes.data(), 1, bytes.size(), file) != bytes.size())
					bytes.clear();
			}
			fclose(file);
			return bytes;
		};
		std::vector<char> metadata = readFontFile(L"OCUKeyboardFont.sfn", 4 * 1024 * 1024);
		std::vector<char> texture = readFontFile(L"OCUKeyboardFont.png", 32 * 1024 * 1024);
		if (!metadata.empty() && !texture.empty()) {
			font = std::make_unique<SudoFontMeta>(std::move(metadata), std::move(texture));
			resolvedFont = wantFont;
			loadedExternalFont = true;
		} else {
			OOVR_LOGF("Custom keyboard font '%s' is missing OCUKeyboardFont.sfn or OCUKeyboardFont.png; using theme default", wantFont.c_str());
		}
	}
	if (!loadedExternalFont && !wantFont.empty() && wantFont != "theme") {
		bool found = false;
		for (const KbFontDef& candidate : K_FONTS) {
			if (wantFont == candidate.name) {
				fontResource = candidate.resource;
				resolvedFont = candidate.name;
				found = true;
				break;
			}
		}
		if (!found && wantFont.rfind("custom_", 0) != 0)
			OOVR_LOGF("Keyboard font '%s' unknown, using theme default", wantFont.c_str());
	}

	if (!loadedExternalFont) {
		font = std::make_unique<SudoFontMeta>(
		    loadResource(fontResource, RES_T_FNTMETA),
		    loadResource(fontResource, RES_T_PNG));
	}

	parchmentBg.clear();
	parchmentW = parchmentH = 0;
	auto bgData = loadResource(theme->bgRes, RES_T_PNG);
	lodepng::decode(parchmentBg, parchmentW, parchmentH, (const uint8_t*)bgData.data(), bgData.size(), LCT_RGBA, 8);
	OOVR_DEBUG_LOGF("Keyboard theme '%s', font '%s' loaded: bg %ux%u",
	    theme->name, resolvedFont.c_str(), parchmentW, parchmentH);

	// Per-theme space bar image (overrides the default loaded at startup)
	{
		auto sbData = loadResource(theme->spacebarRes, RES_T_PNG);
		s_spaceBarImage.clear();
		s_spaceBarWidth = s_spaceBarHeight = 0;
		unsigned err = lodepng::decode(s_spaceBarImage, s_spaceBarWidth, s_spaceBarHeight,
		    (const uint8_t*)sbData.data(), sbData.size(), LCT_RGBA, 8);
		if (err) {
			OOVR_LOGF("Theme spacebar decode failed: %s", lodepng_error_text(err));
			s_spaceBarImage.clear();
			s_spaceBarWidth = s_spaceBarHeight = 0;
		}
	}

	// Track raw INI values for hot reload. The effective design values may come
	// from the loaded layout and are reapplied whenever that layout changes.
	loadedThemeName = configTheme;
	loadedFontName = configFont;
}

void VRKeyboard::LoadKeyboardLayout()
{
	const std::string& requested = oovr_global_configuration.KbLayout();
	std::vector<char> data;
	bool loadedExternal = false;
	const bool automatic = requested.empty() || requested == "auto";
	const std::string filename = automatic ? "OCUKeyboard.kb" : requested;

	if (filename != "embedded" && filename != "default") {
		// Custom layouts intentionally stay in the game root beside openvr_api.dll.
		// Refuse paths and traversal: the ini chooses a file, not an arbitrary disk
		// location, which also makes Keyboard Studio installs portable through MO2.
		const bool safeName = filename.find('/') == std::string::npos
		    && filename.find('\\') == std::string::npos
		    && filename.find(':') == std::string::npos
		    && filename.find("..") == std::string::npos;
		if (safeName) {
			std::wstring wideName = CHAR_CONV.from_bytes(filename);
			std::wstring path = GetOCDllDirectory() + wideName;
			FILE* file = _wfopen(path.c_str(), L"rb");
			if (file) {
				fseek(file, 0, SEEK_END);
				long length = ftell(file);
				fseek(file, 0, SEEK_SET);
				if (length > 0 && length <= 1024 * 1024) {
					data.resize((size_t)length);
					loadedExternal = fread(data.data(), 1, data.size(), file) == data.size();
					if (!loadedExternal)
						data.clear();
				}
				fclose(file);
			}
			if (!loadedExternal && !automatic)
				OOVR_LOGF("Custom keyboard layout '%s' could not be read; using embedded layout", filename.c_str());
		} else {
			OOVR_LOGF("Custom keyboard layout '%s' rejected: layout must be a filename beside openvr_api.dll", filename.c_str());
		}
	}

	if (!loadedExternal)
		data = loadResource(RES_O_KB_EN_GB, RES_T_KBLAYOUT);
	layout = std::make_unique<KeyboardLayout>(std::move(data));
	LoadThemeAssets();
	LoadKeyboardArtwork();
	loadedLayoutName = requested;
	OOVR_DEBUG_LOGF("Keyboard layout loaded: %s", loadedExternal ? filename.c_str() : "embedded en_gb.kb");
}

void VRKeyboard::LoadKeyboardArtwork()
{
	customKeyboardBg = {};
	customKeyboardSprites.clear();
	customControlArrow = {};
	customConsoleInputBg = {};
	customModeVrArtwork = {};
	customModePcArtwork = {};
	customLockWorldArtwork = {};
	customLockHeadArtwork = {};
	if (!layout)
		return;

	auto loadPngBesideDll = [&](const KeyboardLayout::ImageLayer& placement,
	                            DecodedKeyboardArtwork& artwork) {
		const std::string& filename = placement.file;
		if (filename.empty())
			return;
		const bool safeName = filename.find('/') == std::string::npos
		    && filename.find('\\') == std::string::npos
		    && filename.find(':') == std::string::npos
		    && filename.find("..") == std::string::npos;
		if (!safeName) {
			OOVR_LOGF("Keyboard artwork '%s' rejected: only a filename beside openvr_api.dll is allowed", filename.c_str());
			return;
		}

		std::wstring path = GetOCDllDirectory() + CHAR_CONV.from_bytes(filename);
		FILE* file = _wfopen(path.c_str(), L"rb");
		if (!file) {
			OOVR_LOGF("Keyboard artwork '%s' was not found", filename.c_str());
			return;
		}
		fseek(file, 0, SEEK_END);
		long length = ftell(file);
		fseek(file, 0, SEEK_SET);
		if (length <= 0 || length > 32 * 1024 * 1024) {
			fclose(file);
			OOVR_LOGF("Keyboard artwork '%s' has an invalid file size", filename.c_str());
			return;
		}
		std::vector<unsigned char> encoded((size_t)length);
		bool readOk = fread(encoded.data(), 1, encoded.size(), file) == encoded.size();
		fclose(file);
		if (!readOk)
			return;

		unsigned error = lodepng::decode(artwork.pixels, artwork.width, artwork.height,
		    encoded.data(), encoded.size(), LCT_RGBA, 8);
		if (error || artwork.width == 0 || artwork.height == 0
		    || artwork.width > 8192 || artwork.height > 8192) {
			OOVR_LOGF("Keyboard artwork '%s' PNG decode failed: %s", filename.c_str(), lodepng_error_text(error));
			artwork = {};
			return;
		}
		artwork.placement = placement;
	};

	// Scale, rotate, edge-fade, and crop authored layers once when the keyboard
	// opens. Refreshes then only alpha-blend cached pixels, which keeps bilinear
	// quality from becoming a per-frame penalty while the laser cursor moves.
	auto prepareArtwork = [&](DecodedKeyboardArtwork& artwork) {
		if (artwork.pixels.empty() || artwork.width == 0 || artwork.height == 0)
			return;
		const int originalX = int(std::round(artwork.placement.x));
		const int originalY = int(std::round(artwork.placement.y));
		const int originalW = std::max(1, int(std::round(artwork.placement.width)));
		const int originalH = std::max(1, int(std::round(artwork.placement.height)));
		const int visibleLeft = std::max(0, originalX);
		const int visibleTop = std::max(0, originalY);
		const int visibleRight = std::min(int(texWidth), originalX + originalW);
		const int visibleBottom = std::min(int(texHeight), originalY + originalH);
		if (visibleRight <= visibleLeft || visibleBottom <= visibleTop) {
			artwork = {};
			return;
		}

		const unsigned int sourceW = artwork.width;
		const unsigned int sourceH = artwork.height;
		std::vector<uint8_t> source = std::move(artwork.pixels);
		const int outputW = visibleRight - visibleLeft;
		const int outputH = visibleBottom - visibleTop;
		std::vector<uint8_t> output(size_t(outputW) * outputH * 4, 0);
		const float radians = -artwork.placement.rotation * math_pi / 180.0f;
		const float cosine = std::cos(radians);
		const float sine = std::sin(radians);
		const float centerX = originalW / 2.0f;
		const float centerY = originalH / 2.0f;
		const int edgeFade = std::clamp(artwork.placement.edgeFade, 0, 300);
		const int roundness = std::clamp(artwork.placement.roundness, 0, 100)
		    * std::min(originalW, originalH) / 200;

		auto accumulate = [&](int sx, int sy, float weight,
		                      float& alpha, float& red, float& green, float& blue) {
			const size_t index = (size_t(sy) * sourceW + sx) * 4;
			const float sampleAlpha = source[index + 3] / 255.0f;
			alpha += sampleAlpha * weight;
			red += source[index + 0] * sampleAlpha * weight;
			green += source[index + 1] * sampleAlpha * weight;
			blue += source[index + 2] * sampleAlpha * weight;
		};

		for (int y = 0; y < outputH; ++y) {
			for (int x = 0; x < outputW; ++x) {
				const float rectX = float(visibleLeft - originalX + x) + 0.5f;
				const float rectY = float(visibleTop - originalY + y) + 0.5f;
				const float dx = rectX - centerX;
				const float dy = rectY - centerY;
				const float localX = cosine * dx - sine * dy + centerX;
				const float localY = sine * dx + cosine * dy + centerY;
				if (localX < 0 || localY < 0 || localX >= originalW || localY >= originalH)
					continue;

				const float sampleX = localX * sourceW / originalW - 0.5f;
				const float sampleY = localY * sourceH / originalH - 0.5f;
				const float floorX = std::floor(sampleX);
				const float floorY = std::floor(sampleY);
				const int x0 = std::clamp(int(floorX), 0, int(sourceW) - 1);
				const int y0 = std::clamp(int(floorY), 0, int(sourceH) - 1);
				const int x1 = std::clamp(int(floorX) + 1, 0, int(sourceW) - 1);
				const int y1 = std::clamp(int(floorY) + 1, 0, int(sourceH) - 1);
				const float fx = std::clamp(sampleX - floorX, 0.0f, 1.0f);
				const float fy = std::clamp(sampleY - floorY, 0.0f, 1.0f);
				float alpha = 0, red = 0, green = 0, blue = 0;
				accumulate(x0, y0, (1.0f - fx) * (1.0f - fy), alpha, red, green, blue);
				accumulate(x1, y0, fx * (1.0f - fy), alpha, red, green, blue);
				accumulate(x0, y1, (1.0f - fx) * fy, alpha, red, green, blue);
				accumulate(x1, y1, fx * fy, alpha, red, green, blue);
				if (alpha <= 0.0001f)
					continue;
				float edgeFactor = 1.0f;
				if (roundness > 0) {
					// Signed distance to the rounded rectangle. Feathering must
					// begin at this pill boundary, not at the discarded square
					// image edge, or rounded backgrounds retain a hard halo.
					const float halfWidth = originalW / 2.0f;
					const float halfHeight = originalH / 2.0f;
					const float qx = std::abs(localX - halfWidth) - (halfWidth - roundness);
					const float qy = std::abs(localY - halfHeight) - (halfHeight - roundness);
					const float outsideX = std::max(qx, 0.0f);
					const float outsideY = std::max(qy, 0.0f);
					const float signedDistance = std::sqrt(outsideX * outsideX + outsideY * outsideY)
					    + std::min(std::max(qx, qy), 0.0f) - roundness;
					if (signedDistance >= 0)
						continue;
					if (edgeFade > 0)
						edgeFactor = std::clamp(-signedDistance / edgeFade, 0.0f, 1.0f);
				} else if (edgeFade > 0) {
					const float distance = std::min(std::min(localX, originalW - 1.0f - localX),
					    std::min(localY, originalH - 1.0f - localY));
					edgeFactor = std::clamp(distance / edgeFade, 0.0f, 1.0f);
				}
				const size_t destination = (size_t(y) * outputW + x) * 4;
				output[destination + 0] = uint8_t(std::clamp(red / alpha, 0.0f, 255.0f));
				output[destination + 1] = uint8_t(std::clamp(green / alpha, 0.0f, 255.0f));
				output[destination + 2] = uint8_t(std::clamp(blue / alpha, 0.0f, 255.0f));
				output[destination + 3] = uint8_t(std::clamp(alpha * edgeFactor * 255.0f, 0.0f, 255.0f));
			}
		}

		artwork.pixels = std::move(output);
		artwork.width = unsigned(outputW);
		artwork.height = unsigned(outputH);
		artwork.placement.x = float(visibleLeft);
		artwork.placement.y = float(visibleTop);
		artwork.placement.width = float(outputW);
		artwork.placement.height = float(outputH);
		artwork.placement.rotation = 0;
		artwork.placement.edgeFade = 0;
		artwork.prepared = true;
	};

	auto prepareGlow = [&](DecodedKeyboardArtwork& artwork) {
		const KeyboardLayout::ImageLayer& layer = artwork.placement;
		if (!layer.glowEnabled || artwork.pixels.empty() || artwork.width == 0 || artwork.height == 0)
			return;
		const int radius = std::clamp(layer.glowRadius, 1, 48);
		const int outputW = int(artwork.width) + radius * 2;
		const int outputH = int(artwork.height) + radius * 2;
		std::vector<float> alpha(size_t(outputW) * outputH, 0.0f);
		std::vector<float> horizontal(alpha.size(), 0.0f);
		std::vector<float> blurred(alpha.size(), 0.0f);
		for (unsigned int y = 0; y < artwork.height; ++y)
			for (unsigned int x = 0; x < artwork.width; ++x)
				alpha[size_t(y + radius) * outputW + x + radius] = artwork.pixels[(size_t(y) * artwork.width + x) * 4 + 3] / 255.0f;
		const int diameter = radius * 2 + 1;
		for (int y = 0; y < outputH; ++y) {
			float sum = 0.0f;
			const size_t row = size_t(y) * outputW;
			for (int x = -radius; x < outputW; ++x) {
				if (x + radius < outputW) sum += alpha[row + x + radius];
				if (x - radius - 1 >= 0) sum -= alpha[row + x - radius - 1];
				if (x >= 0) horizontal[row + x] = sum / diameter;
			}
		}
		for (int x = 0; x < outputW; ++x) {
			float sum = 0.0f;
			for (int y = -radius; y < outputH; ++y) {
				if (y + radius < outputH) sum += horizontal[size_t(y + radius) * outputW + x];
				if (y - radius - 1 >= 0) sum -= horizontal[size_t(y - radius - 1) * outputW + x];
				if (y >= 0) blurred[size_t(y) * outputW + x] = sum / diameter;
			}
		}
		artwork.glowPixels.resize(size_t(outputW) * outputH * 4);
		for (size_t pixel = 0; pixel < blurred.size(); ++pixel) {
			const size_t target = pixel * 4;
			artwork.glowPixels[target + 0] = layer.glowColor[0];
			artwork.glowPixels[target + 1] = layer.glowColor[1];
			artwork.glowPixels[target + 2] = layer.glowColor[2];
			artwork.glowPixels[target + 3] = uint8_t(std::clamp(std::sqrt(blurred[pixel]) * layer.glowColor[3], 0.0f, 255.0f));
		}
		artwork.glowWidth = unsigned(outputW);
		artwork.glowHeight = unsigned(outputH);
		artwork.glowX = int(std::round(layer.x)) - radius;
		artwork.glowY = int(std::round(layer.y)) - radius;
	};

	loadPngBesideDll(layout->GetBackgroundLayer(), customKeyboardBg);
	prepareArtwork(customKeyboardBg);
	for (const KeyboardLayout::ImageLayer& placement : layout->GetSprites()) {
		DecodedKeyboardArtwork artwork;
		loadPngBesideDll(placement, artwork);
		prepareArtwork(artwork);
		prepareGlow(artwork);
		if (!artwork.pixels.empty())
			customKeyboardSprites.push_back(std::move(artwork));
	}
	KeyboardLayout::ImageLayer arrowPlacement;
	arrowPlacement.file = layout->GetControlArrowFile();
	arrowPlacement.rotation = layout->GetControlArrowRotation();
	arrowPlacement.glowEnabled = layout->GetControlArrowGlowEnabled();
	const uint8_t* arrowGlowColor = layout->GetControlArrowGlowColor();
	std::copy(arrowGlowColor, arrowGlowColor + 4, arrowPlacement.glowColor);
	arrowPlacement.glowStrength = layout->GetControlArrowGlowStrength();
	arrowPlacement.glowRadius = layout->GetControlArrowGlowRadius();
	arrowPlacement.breatheEnabled = layout->GetControlArrowBreatheEnabled();
	arrowPlacement.breatheMinPercent = layout->GetControlArrowBreatheMinPercent();
	arrowPlacement.breathePeriodSeconds = layout->GetControlArrowBreathePeriodSeconds();
	arrowPlacement.breathePhaseDegrees = layout->GetControlArrowBreathePhaseDegrees();
	loadPngBesideDll(arrowPlacement, customControlArrow);

	KeyboardLayout::ImageLayer consolePlacement;
	consolePlacement.file = layout->GetConsoleInputBackgroundFile();
	consolePlacement.x = 0;
	consolePlacement.y = 0;
	consolePlacement.width = float(consoleTexWidth);
	consolePlacement.height = float(consoleTexHeight);
	loadPngBesideDll(consolePlacement, customConsoleInputBg);
	prepareArtwork(customConsoleInputBg);

	// Semantic top-button art is fitted once to the exact authored button
	// rectangles. Runtime state merely chooses which cached image to composite;
	// it does not create another OpenXR overlay or swapchain.
	const int marginH = 120;
	const int btnH = 32;
	const int btnY = GRAB_BAR_HEIGHT + 60 - btnH - 4;
	const auto& modeDesign = layout->GetModeButtonDesign();
	const auto& lockDesign = layout->GetLockButtonDesign();
	const int modeBtnW = modeDesign.width > 0 ? std::max(8, int(std::round(modeDesign.width))) : CONSOLE_BTN_WIDTH;
	const int modeBtnH = modeDesign.height > 0 ? std::max(8, int(std::round(modeDesign.height))) : btnH;
	const int lockBtnW = lockDesign.width > 0 ? std::max(8, int(std::round(lockDesign.width))) : TOGGLE_BTN_WIDTH;
	const int lockBtnH = lockDesign.height > 0 ? std::max(8, int(std::round(lockDesign.height))) : btnH;
	const int modeThemeX = theme ? theme->modeBtnOffX : 0;
	const int modeThemeY = theme ? theme->modeBtnOffY : 0;
	const int lockThemeX = theme ? theme->lockBtnOffX : 0;
	const int lockThemeY = theme ? theme->lockBtnOffY : 0;
	const int modeBtnX = marginH + modeThemeX + int(std::round(layout->GetModeButtonOffsetX()));
	const int modeBtnY = btnY + modeThemeY + int(std::round(layout->GetModeButtonOffsetY()));
	const int lockBtnX = int(texWidth) - marginH - TOGGLE_BTN_WIDTH + lockThemeX
	    + int(std::round(layout->GetLockButtonOffsetX()));
	const int lockBtnY = btnY + lockThemeY + int(std::round(layout->GetLockButtonOffsetY()));
	const int modeArtworkX = modeBtnX + int(std::round(layout->GetModeArtworkOffsetX()));
	const int modeArtworkY = modeBtnY + int(std::round(layout->GetModeArtworkOffsetY()));
	const int modeArtworkW = layout->GetModeArtworkWidth() > 0
	    ? std::max(8, int(std::round(layout->GetModeArtworkWidth()))) : modeBtnW;
	const int modeArtworkH = layout->GetModeArtworkHeight() > 0
	    ? std::max(8, int(std::round(layout->GetModeArtworkHeight()))) : modeBtnH;
	const int lockArtworkX = lockBtnX + int(std::round(layout->GetLockArtworkOffsetX()));
	const int lockArtworkY = lockBtnY + int(std::round(layout->GetLockArtworkOffsetY()));
	const int lockArtworkW = layout->GetLockArtworkWidth() > 0
	    ? std::max(8, int(std::round(layout->GetLockArtworkWidth()))) : lockBtnW;
	const int lockArtworkH = layout->GetLockArtworkHeight() > 0
	    ? std::max(8, int(std::round(layout->GetLockArtworkHeight()))) : lockBtnH;
	auto loadStateArtwork = [&](const std::string& file, int x, int y, int width, int height,
	                            DecodedKeyboardArtwork& artwork) {
		KeyboardLayout::ImageLayer placement;
		placement.file = file;
		placement.x = float(x);
		placement.y = float(y);
		placement.width = float(width);
		placement.height = float(height);
		loadPngBesideDll(placement, artwork);
		prepareArtwork(artwork);
	};
	loadStateArtwork(layout->GetModeVrArtworkFile(), modeArtworkX, modeArtworkY, modeArtworkW, modeArtworkH, customModeVrArtwork);
	loadStateArtwork(layout->GetModePcArtworkFile(), modeArtworkX, modeArtworkY, modeArtworkW, modeArtworkH, customModePcArtwork);
	loadStateArtwork(layout->GetLockWorldArtworkFile(), lockArtworkX, lockArtworkY, lockArtworkW, lockArtworkH, customLockWorldArtwork);
	loadStateArtwork(layout->GetLockHeadArtworkFile(), lockArtworkX, lockArtworkY, lockArtworkW, lockArtworkH, customLockHeadArtwork);
}

void VRKeyboard::Refresh()
{
	LARGE_INTEGER refreshStart = {};
	LARGE_INTEGER refreshCpuDone = {};
	LARGE_INTEGER refreshDone = {};
	static LARGE_INTEGER performanceFrequency = [] {
		LARGE_INTEGER value = {};
		QueryPerformanceFrequency(&value);
		return value;
	}();
	QueryPerformanceCounter(&refreshStart);

	D3D11_TEXTURE2D_DESC desc;
	desc.Width = texWidth;
	desc.Height = texHeight;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	desc.SampleDesc = { 1, 0 };
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = 0;
	desc.CPUAccessFlags = 0;
	desc.MiscFlags = 0;

	keyboardRenderBuffer.resize(static_cast<size_t>(desc.Width) * desc.Height);
	pix_t* pixels = reinterpret_cast<pix_t*>(keyboardRenderBuffer.data());

	// ── Parchment background ──
	const int BORD = 2; // Key border thickness in pixels

	// Copy parchment texture as background with reduced opacity for see-through
	// Fill with transparency first
	memset(pixels, 0, desc.Width * desc.Height * sizeof(pix_t));
	const double animationSeconds = GetTickCount64() / 1000.0;
	auto breatheMultiplier = [animationSeconds](const KeyboardLayout::ImageLayer& image) {
		if (!image.breatheEnabled)
			return 1.0f;
		const float minimum = std::clamp(image.breatheMinPercent, 0, 100) / 100.0f;
		const float period = std::clamp(image.breathePeriodSeconds, 0.5f, 10.0f);
		const double angle = animationSeconds * math_pi * 2.0 / period
		    + image.breathePhaseDegrees * math_pi / 180.0;
		const float wave = float(0.5 - 0.5 * std::cos(angle));
		return minimum + (1.0f - minimum) * wave;
	};

	// Modern themes start with the Configurator's charcoal window surface and
	// soft rounded border. Other themes retain their embedded artwork.
	auto compositeArtwork = [pixels, &desc, &breatheMultiplier](const DecodedKeyboardArtwork& artwork,
	                                     float globalOpacity = 1.0f,
	                                     float extraRotation = 0.0f,
	                                     const KeyboardLayout::ImageLayer* overridePlacement = nullptr) {
		if (artwork.pixels.empty() || artwork.width == 0 || artwork.height == 0)
			return;
		const KeyboardLayout::ImageLayer& layer = overridePlacement ? *overridePlacement : artwork.placement;
		const int drawX = int(std::round(layer.x));
		const int drawY = int(std::round(layer.y));
		const int drawW = std::max(1, int(std::round(layer.width)));
		const int drawH = std::max(1, int(std::round(layer.height)));
		const float artworkBreathing = layer.glowEnabled ? 1.0f : breatheMultiplier(layer);
		const float opacity = std::clamp(layer.opacity, 0, 100) / 100.0f
		    * std::clamp(globalOpacity, 0.0f, 1.0f) * artworkBreathing;
		const int edgeFade = std::clamp(layer.edgeFade, 0, 300);
		const float radians = -(layer.rotation + extraRotation) * math_pi / 180.0f;
		const float cosine = std::cos(radians);
		const float sine = std::sin(radians);
		const float centerX = drawW / 2.0f;
		const float centerY = drawH / 2.0f;
		if (artwork.prepared && overridePlacement == nullptr && extraRotation == 0.0f) {
			for (unsigned int y = 0; y < artwork.height; ++y) {
				const int targetY = drawY + int(y);
				if (targetY < 0 || targetY >= int(desc.Height))
					continue;
				for (unsigned int x = 0; x < artwork.width; ++x) {
					const int targetX = drawX + int(x);
					if (targetX < 0 || targetX >= int(desc.Width))
						continue;
					const size_t source = (size_t(y) * artwork.width + x) * 4;
					const float alpha = artwork.pixels[source + 3] / 255.0f * opacity;
					if (alpha <= 0)
						continue;
					pix_t& target = pixels[targetX + targetY * desc.Width];
					target.r = uint8_t(artwork.pixels[source + 0] * alpha + target.r * (1.0f - alpha));
					target.g = uint8_t(artwork.pixels[source + 1] * alpha + target.g * (1.0f - alpha));
					target.b = uint8_t(artwork.pixels[source + 2] * alpha + target.b * (1.0f - alpha));
					target.a = uint8_t(std::min(255.0f,
					    artwork.pixels[source + 3] * opacity + target.a * (1.0f - alpha)));
				}
			}
			return;
		}

		auto accumulate = [&](int sx, int sy, float weight,
		                      float& alpha, float& red, float& green, float& blue) {
			const size_t source = (size_t(sy) * artwork.width + sx) * 4;
			const float sampleAlpha = artwork.pixels[source + 3] / 255.0f;
			alpha += sampleAlpha * weight;
			red += artwork.pixels[source + 0] * sampleAlpha * weight;
			green += artwork.pixels[source + 1] * sampleAlpha * weight;
			blue += artwork.pixels[source + 2] * sampleAlpha * weight;
		};

		for (int y = 0; y < drawH; ++y) {
			for (int x = 0; x < drawW; ++x) {
				const float dx = x + 0.5f - centerX;
				const float dy = y + 0.5f - centerY;
				const float localX = cosine * dx - sine * dy + centerX;
				const float localY = sine * dx + cosine * dy + centerY;
				if (localX < 0 || localY < 0 || localX >= drawW || localY >= drawH)
					continue;
				const int targetX = drawX + x;
				const int targetY = drawY + y;
				if (targetX < 0 || targetY < 0 || targetX >= int(desc.Width) || targetY >= int(desc.Height))
					continue;
				const float sampleX = localX * artwork.width / drawW - 0.5f;
				const float sampleY = localY * artwork.height / drawH - 0.5f;
				const float floorX = std::floor(sampleX);
				const float floorY = std::floor(sampleY);
				const int x0 = std::clamp(int(floorX), 0, int(artwork.width) - 1);
				const int y0 = std::clamp(int(floorY), 0, int(artwork.height) - 1);
				const int x1 = std::clamp(int(floorX) + 1, 0, int(artwork.width) - 1);
				const int y1 = std::clamp(int(floorY) + 1, 0, int(artwork.height) - 1);
				const float fx = std::clamp(sampleX - floorX, 0.0f, 1.0f);
				const float fy = std::clamp(sampleY - floorY, 0.0f, 1.0f);
				float sampleAlpha = 0, red = 0, green = 0, blue = 0;
				accumulate(x0, y0, (1.0f - fx) * (1.0f - fy), sampleAlpha, red, green, blue);
				accumulate(x1, y0, fx * (1.0f - fy), sampleAlpha, red, green, blue);
				accumulate(x0, y1, (1.0f - fx) * fy, sampleAlpha, red, green, blue);
				accumulate(x1, y1, fx * fy, sampleAlpha, red, green, blue);
				if (sampleAlpha <= 0.0001f)
					continue;
				float edgeFactor = 1.0f;
				if (edgeFade > 0) {
					const float edgeDistance = std::min(std::min(localX, drawW - 1.0f - localX),
					    std::min(localY, drawH - 1.0f - localY));
					edgeFactor = std::clamp(edgeDistance / edgeFade, 0.0f, 1.0f);
				}
				const float alpha = sampleAlpha * opacity * edgeFactor;
				if (alpha <= 0)
					continue;
				pix_t& target = pixels[targetX + targetY * desc.Width];
				target.r = uint8_t((red / sampleAlpha) * alpha + target.r * (1.0f - alpha));
				target.g = uint8_t((green / sampleAlpha) * alpha + target.g * (1.0f - alpha));
				target.b = uint8_t((blue / sampleAlpha) * alpha + target.b * (1.0f - alpha));
				target.a = uint8_t(std::min(255.0f,
				    sampleAlpha * 255.0f * opacity * edgeFactor + target.a * (1.0f - alpha)));
			}
		}
	};

	if (!customKeyboardBg.pixels.empty()) {
		compositeArtwork(customKeyboardBg, s_opacityPercent / 100.0f);
	} else if (theme && theme->modernKeys) {
		const int radius = 22;
		const float opacityFrac = s_opacityPercent / 100.0f;
		for (int y = 0; y < (int)desc.Height; ++y) {
			for (int x = 0; x < (int)desc.Width; ++x) {
				const int dx = x < radius ? radius - x
				    : x >= (int)desc.Width - radius ? x - ((int)desc.Width - radius - 1)
				                                      : 0;
				const int dy = y < radius ? radius - y
				    : y >= (int)desc.Height - radius ? y - ((int)desc.Height - radius - 1)
				                                       : 0;
				if (dx * dx + dy * dy > radius * radius)
					continue;

				const bool border = x < 2 || y < 2 || x >= (int)desc.Width - 2 || y >= (int)desc.Height - 2
				    || (dx * dx + dy * dy > (radius - 2) * (radius - 2));
				pix_t& p = pixels[x + y * desc.Width];
				p = border ? pix_t{ 48, 56, 68, (uint8_t)(235 * opacityFrac) }
				           : pix_t{ 14, 17, 22, (uint8_t)(235 * opacityFrac) };
			}
		}
	} else if (!parchmentBg.empty() && parchmentW > 0 && parchmentH > 0) {
		// Copy background art (which may be smaller than texture), centered/top.
		// Center horizontally, align to top vertically
		int offsetX = ((int)desc.Width - (int)parchmentW) / 2;
		int offsetY = 0; // Top-aligned

		float opacityFrac = s_opacityPercent / 100.0f;

		// Stock themes are authored at the native runtime size. At full opacity
		// they can seed the render surface with one memcpy instead of more than
		// half a million per-channel assignments on every breathing frame.
		if (s_opacityPercent == 100 && offsetX == 0 && parchmentW == desc.Width
		    && parchmentH == desc.Height) {
			memcpy(pixels, parchmentBg.data(), parchmentBg.size());
		} else {
			for (unsigned int y = 0; y < parchmentH && (y + offsetY) < desc.Height; y++) {
				for (unsigned int x = 0; x < parchmentW && (x + offsetX) < desc.Width; x++) {
					int srcIdx = (y * parchmentW + x) * 4;
					int dstIdx = ((y + offsetY) * desc.Width + (x + offsetX));

					pixels[dstIdx].r = parchmentBg[srcIdx + 0];
					pixels[dstIdx].g = parchmentBg[srcIdx + 1];
					pixels[dstIdx].b = parchmentBg[srcIdx + 2];
					pixels[dstIdx].a = (uint8_t)(parchmentBg[srcIdx + 3] * opacityFrac);
				}
			}
		}
	}

	// Multiple transparent sprites are composited back-to-front below controls
	// and keys, exactly matching Keyboard Studio's authoring order. Their cached
	// procedural halo breathes independently; the PNG itself stays stable.
	auto compositeGlow = [pixels, &desc, &breatheMultiplier](const DecodedKeyboardArtwork& artwork) {
		if (artwork.glowPixels.empty() || artwork.glowWidth == 0 || artwork.glowHeight == 0)
			return;
		const KeyboardLayout::ImageLayer& layer = artwork.placement;
		const float opacity = std::clamp(layer.opacity, 0, 100) / 100.0f
		    * std::clamp(layer.glowStrength, 0, 100) / 100.0f * breatheMultiplier(layer);
		for (unsigned int y = 0; y < artwork.glowHeight; ++y) {
			const int targetY = artwork.glowY + int(y);
			if (targetY < 0 || targetY >= int(desc.Height)) continue;
			for (unsigned int x = 0; x < artwork.glowWidth; ++x) {
				const int targetX = artwork.glowX + int(x);
				if (targetX < 0 || targetX >= int(desc.Width)) continue;
				const size_t source = (size_t(y) * artwork.glowWidth + x) * 4;
				const float alpha = artwork.glowPixels[source + 3] / 255.0f * opacity;
				if (alpha <= 0) continue;
				pix_t& target = pixels[targetX + targetY * desc.Width];
				target.r = uint8_t(artwork.glowPixels[source + 0] * alpha + target.r * (1.0f - alpha));
				target.g = uint8_t(artwork.glowPixels[source + 1] * alpha + target.g * (1.0f - alpha));
				target.b = uint8_t(artwork.glowPixels[source + 2] * alpha + target.b * (1.0f - alpha));
				target.a = uint8_t(std::min(255.0f, artwork.glowPixels[source + 3] * opacity + target.a * (1.0f - alpha)));
			}
		}
	};
	for (const DecodedKeyboardArtwork& sprite : customKeyboardSprites) {
		compositeGlow(sprite);
		compositeArtwork(sprite);
	}

	int padding = 6;          // Gap between keys

	const KbThemeDef& T = *theme;
	const KeyboardLayout::VisualStyle& VS = layout->GetVisualStyle();
	const bool customStyle = VS.enabled;
	const bool modernKeys = T.modernKeys || customStyle;
	const uint8_t* effectiveInk = customStyle ? VS.fontColor : T.ink;
	const uint8_t* effectiveLabelOutlineColor = customStyle ? VS.fontOutlineColor : T.outline;
	const uint8_t* effectiveKeyBorder = customStyle ? VS.keyColor : T.keyBorder;
	const uint8_t* effectivePlateFill = customStyle ? VS.plateFillColor : T.keyFillIdle;
	const uint8_t* effectiveGlow = customStyle ? VS.glowColor : T.keyGlow;
	const uint8_t* effectiveHoverInk = customStyle && VS.hoverEnabled ? VS.hoverColor : effectiveInk;
	const bool effectiveOutline = customStyle ? VS.labelOutline : T.labelOutline;
	uint8_t customHoverFill[4] = {
		VS.hoverColor[0], VS.hoverColor[1], VS.hoverColor[2],
		(uint8_t)std::clamp(VS.hoverStrength * 2, 0, 200)
	};
	uint8_t customActiveFill[4] = {
		VS.keyColor[0], VS.keyColor[1], VS.keyColor[2],
		(uint8_t)std::clamp(VS.keyColor[3] / 2, 36, 140)
	};
	uint8_t customHoverPlate[4] = {
		VS.hoverColor[0], VS.hoverColor[1], VS.hoverColor[2],
		(uint8_t)std::clamp(VS.hoverStrength, 0, 120)
	};
	const uint8_t* effectiveHoverPlate = customStyle && VS.hoverEnabled ? customHoverPlate : T.hoverPlate;
	const uint8_t* effectiveTextBarBorder = customStyle ? VS.keyColor : T.textBarBorder;
	int effectiveGlowStrength = customStyle ? std::clamp(VS.glowStrength, 0, 100) : 35;
	if (customStyle && VS.keyBreatheEnabled) {
		KeyboardLayout::ImageLayer keyBreathe;
		keyBreathe.breatheEnabled = true;
		keyBreathe.breatheMinPercent = VS.keyBreatheMinPercent;
		keyBreathe.breathePeriodSeconds = VS.keyBreathePeriodSeconds;
		keyBreathe.breathePhaseDegrees = VS.keyBreathePhaseDegrees;
		effectiveGlowStrength = int(std::round(effectiveGlowStrength * breatheMultiplier(keyBreathe)));
	}
	int effectiveFontGlowStrength = customStyle && VS.fontGlowEnabled
	    ? std::clamp(VS.fontGlowStrength, 0, 100) : 0;
	if (effectiveFontGlowStrength > 0 && VS.fontBreatheEnabled) {
		KeyboardLayout::ImageLayer fontBreathe;
		fontBreathe.breatheEnabled = true;
		fontBreathe.breatheMinPercent = VS.fontBreatheMinPercent;
		fontBreathe.breathePeriodSeconds = VS.fontBreathePeriodSeconds;
		fontBreathe.breathePhaseDegrees = VS.fontBreathePhaseDegrees;
		effectiveFontGlowStrength = int(std::round(effectiveFontGlowStrength * breatheMultiplier(fontBreathe)));
	}
	const int effectiveFontGlowRadius = customStyle ? std::clamp(VS.fontGlowRadius, 1, 8) : 0;
	const uint8_t effectiveFontGlowAlpha = uint8_t(std::clamp(
	    int(VS.fontGlowColor[3]) * effectiveFontGlowStrength / 100, 0, 255));
	auto tp = [](const uint8_t c[4]) { return pix_t{ c[0], c[1], c[2], c[3] }; };

	// fillArea blends a semi-transparent dark overlay on top of the parchment
	auto fillArea = [pixels, &desc](int x, int y, int w, int h, int r, int g, int b, int a = 160) {
		for (int ix = 0; ix < w; ix++) {
			for (int iy = 0; iy < h; iy++) {
				int px = x + ix;
				int py = y + iy;
				if (px < 0 || py < 0 || px >= (int)desc.Width || py >= (int)desc.Height)
					continue;
				pix_t& p = pixels[px + py * desc.Width];
				// Alpha-blend the overlay onto the parchment
				float af = a / 255.0f;
				p.r = (uint8_t)(r * af + p.r * (1.0f - af));
				p.g = (uint8_t)(g * af + p.g * (1.0f - af));
				p.b = (uint8_t)(b * af + p.b * (1.0f - af));
				p.a = (uint8_t)std::min(255.0f, a + p.a * (1.0f - af));
			}
		}
	};

	// Rounded alpha-blended plate used by the Configurator-style themes. The
	// keyboard is already a CPU-generated RGBA texture, so this is drawn directly
	// into that same byte buffer and does not add another VR overlay or shader.
	auto fillRoundedArea = [pixels, &desc](int x, int y, int w, int h, int radius,
	                           int r, int g, int b, int a) {
		if (w <= 0 || h <= 0 || a <= 0)
			return;
		radius = std::max(0, std::min(radius, std::min(w, h) / 2));
		const int left = x + radius;
		const int right = x + w - radius - 1;
		const int top = y + radius;
		const int bottom = y + h - radius - 1;
		const float af = a / 255.0f;

		for (int py = y; py < y + h; ++py) {
			for (int px = x; px < x + w; ++px) {
				if (px < 0 || py < 0 || px >= (int)desc.Width || py >= (int)desc.Height)
					continue;
				const int dx = px < left ? left - px : px > right ? px - right : 0;
				const int dy = py < top ? top - py : py > bottom ? py - bottom : 0;
				if (dx * dx + dy * dy > radius * radius)
					continue;
				pix_t& p = pixels[px + py * desc.Width];
				p.r = (uint8_t)(r * af + p.r * (1.0f - af));
				p.g = (uint8_t)(g * af + p.g * (1.0f - af));
				p.b = (uint8_t)(b * af + p.b * (1.0f - af));
				p.a = (uint8_t)std::min(255.0f, a + p.a * (1.0f - af));
			}
		}
	};

	// Draw only the rounded border ring. Painting the outline or glow as a full
	// plate underneath a translucent fill makes its color bleed through the
	// entire key, so fill and outline controls appear to do the same thing.
	auto strokeRoundedArea = [pixels, &desc](int x, int y, int w, int h, int radius, int strokeWidth,
	                             int r, int g, int b, int a) {
		if (w <= 0 || h <= 0 || strokeWidth <= 0 || a <= 0)
			return;
		radius = std::max(0, std::min(radius, std::min(w, h) / 2));
		strokeWidth = std::clamp(strokeWidth, 1, std::max(1, std::min(w, h) / 2));
		const int innerX = x + strokeWidth;
		const int innerY = y + strokeWidth;
		const int innerW = w - strokeWidth * 2;
		const int innerH = h - strokeWidth * 2;
		const int innerRadius = std::max(0, radius - strokeWidth);
		const float af = a / 255.0f;
		auto inside = [](int px, int py, int rx, int ry, int rw, int rh, int rr) {
			if (rw <= 0 || rh <= 0 || px < rx || py < ry || px >= rx + rw || py >= ry + rh)
				return false;
			rr = std::max(0, std::min(rr, std::min(rw, rh) / 2));
			const int left = rx + rr;
			const int right = rx + rw - rr - 1;
			const int top = ry + rr;
			const int bottom = ry + rh - rr - 1;
			const int dx = px < left ? left - px : px > right ? px - right : 0;
			const int dy = py < top ? top - py : py > bottom ? py - bottom : 0;
			return dx * dx + dy * dy <= rr * rr;
		};

		for (int py = y; py < y + h; ++py) {
			for (int px = x; px < x + w; ++px) {
				if (px < 0 || py < 0 || px >= (int)desc.Width || py >= (int)desc.Height)
					continue;
				if (!inside(px, py, x, y, w, h, radius)
				    || inside(px, py, innerX, innerY, innerW, innerH, innerRadius))
					continue;
				pix_t& p = pixels[px + py * desc.Width];
				p.r = (uint8_t)(r * af + p.r * (1.0f - af));
				p.g = (uint8_t)(g * af + p.g * (1.0f - af));
				p.b = (uint8_t)(b * af + p.b * (1.0f - af));
				p.a = (uint8_t)std::min(255.0f, a + p.a * (1.0f - af));
			}
		}
	};

	auto paintModernPlate = [&](int x, int y, int w, int h,
	                            const uint8_t fill[4], bool hot) {
		const int radius = std::min(customStyle ? std::clamp(VS.keyRoundness, 0, 30) : 14, h / 2);
		// Cache the union of the expanding rounded glow rings. Breathing only
		// changes the tint alpha; key geometry does not change while the keyboard
		// is open. This preserves the same single-layer effect without repeatedly
		// rasterizing eight complete rounded rectangles per key.
		if (!customStyle || VS.glowEnabled) {
			const int strength = effectiveGlowStrength;
			const int baseAlpha = std::clamp((hot ? 12 : 5) + strength / 4, 0, 80);
			const int glowRadius = customStyle ? std::clamp(VS.glowRadius, 1, 8) : 4;
			const int glowAlpha = baseAlpha * effectiveGlow[3] / 255;
			if (glowAlpha > 0) {
				PlateGlowMask* mask = nullptr;
				for (PlateGlowMask& candidate : plateGlowMasks) {
					if (candidate.plateWidth == w && candidate.plateHeight == h
					    && candidate.plateRadius == radius && candidate.glowRadius == glowRadius) {
						mask = &candidate;
						break;
					}
				}
				if (!mask) {
					PlateGlowMask generated;
					generated.plateWidth = w;
					generated.plateHeight = h;
					generated.plateRadius = radius;
					generated.glowRadius = glowRadius;
					generated.width = w + glowRadius * 2;
					generated.height = h + glowRadius * 2;
					generated.coverage.assign(
					    static_cast<size_t>(generated.width) * generated.height, 0);
					auto insideRounded = [](int px, int py, int rw, int rh, int rr) {
						if (px < 0 || py < 0 || px >= rw || py >= rh)
							return false;
						rr = std::max(0, std::min(rr, std::min(rw, rh) / 2));
						const int left = rr;
						const int right = rw - rr - 1;
						const int top = rr;
						const int bottom = rh - rr - 1;
						const int dx = px < left ? left - px : px > right ? px - right : 0;
						const int dy = py < top ? top - py : py > bottom ? py - bottom : 0;
						return dx * dx + dy * dy <= rr * rr;
					};
					for (int gy = 0; gy < generated.height; ++gy) {
						for (int gx = 0; gx < generated.width; ++gx) {
							const bool inOuter = insideRounded(gx, gy, generated.width,
							    generated.height, radius + glowRadius);
							const bool inPlate = insideRounded(gx - glowRadius, gy - glowRadius,
							    w, h, radius);
							if (inOuter && !inPlate)
								generated.coverage[gx + gy * generated.width] = 255;
						}
					}
					plateGlowMasks.push_back(std::move(generated));
					mask = &plateGlowMasks.back();
				}

				const float alpha = glowAlpha / 255.0f;
				const int drawX = x - glowRadius;
				const int drawY = y - glowRadius;
				for (int gy = 0; gy < mask->height; ++gy) {
					const int targetY = drawY + gy;
					if (targetY < 0 || targetY >= int(desc.Height))
						continue;
					for (int gx = 0; gx < mask->width; ++gx) {
						if (mask->coverage[gx + gy * mask->width] == 0)
							continue;
						const int targetX = drawX + gx;
						if (targetX < 0 || targetX >= int(desc.Width))
							continue;
						pix_t& pixel = pixels[targetX + targetY * desc.Width];
						pixel.r = uint8_t(effectiveGlow[0] * alpha + pixel.r * (1.0f - alpha));
						pixel.g = uint8_t(effectiveGlow[1] * alpha + pixel.g * (1.0f - alpha));
						pixel.b = uint8_t(effectiveGlow[2] * alpha + pixel.b * (1.0f - alpha));
						pixel.a = uint8_t(std::min(255.0f, glowAlpha + pixel.a * (1.0f - alpha)));
					}
				}
			}
		}
		const int outlineWidth = customStyle ? std::clamp(VS.plateOutlineWidth, 0, 8) : 2;
		if (outlineWidth > 0) {
			strokeRoundedArea(x, y, w, h, radius, outlineWidth,
			    effectiveKeyBorder[0], effectiveKeyBorder[1], effectiveKeyBorder[2], effectiveKeyBorder[3]);
		}
		fillRoundedArea(x + outlineWidth, y + outlineWidth,
		    w - outlineWidth * 2, h - outlineWidth * 2,
		    std::max(0, radius - outlineWidth), fill[0], fill[1], fill[2], fill[3]);
	};

	static const int TEXT_OUTLINE_X[] = { -2, 2, 0, 0, -1, 1, -1, 1 };
	static const int TEXT_OUTLINE_Y[] = { 0, 0, -2, 2, -1, -1, 1, 1 };
	auto forEachFontGlowStamp = [&](auto&& callback) {
		if (effectiveFontGlowStrength <= 0 || effectiveFontGlowAlpha == 0)
			return;
		auto emitRing = [&](int radius, float alphaScale) {
			pix_t glowColour = {
				VS.fontGlowColor[0], VS.fontGlowColor[1], VS.fontGlowColor[2],
				uint8_t(std::clamp(int(std::round(effectiveFontGlowAlpha * alphaScale)), 0, 255))
			};
			const int offsetsX[] = { -radius, radius, 0, 0, -radius, radius, -radius, radius };
			const int offsetsY[] = { 0, 0, -radius, radius, -radius, -radius, radius, radius };
			for (int index = 0; index < 8; ++index)
				callback(offsetsX[index], offsetsY[index], glowColour);
		};
		const int innerRadius = std::max(1, effectiveFontGlowRadius / 2);
		emitRing(effectiveFontGlowRadius, innerRadius == effectiveFontGlowRadius ? 1.0f : 0.55f);
		if (innerRadius != effectiveFontGlowRadius)
			emitRing(innerRadius, 0.82f);
	};
	auto printRaw = [&](int x, int y, pix_t colour, const wstring& text, bool hpad) {
		SudoFontMeta::pix_t c = { colour.r, colour.g, colour.b, colour.a };
		for (size_t i = 0; i < text.length(); i++) {
			font->Blit(text[i], x, y, desc.Width, c, (SudoFontMeta::pix_t*)pixels, hpad);
			x += font->Width(text[i]);
		}
	};
	auto print = [&](int x, int y, pix_t colour, const wstring& text, bool hpad = true) {
		forEachFontGlowStamp([&](int dx, int dy, pix_t glowColour) {
			printRaw(x + dx, y + dy, glowColour, text, hpad);
		});
		if (customStyle && effectiveOutline) {
			pix_t outlineColour = tp(effectiveLabelOutlineColor);
			for (int index = 0; index < 8; ++index)
				printRaw(x + TEXT_OUTLINE_X[index], y + TEXT_OUTLINE_Y[index], outlineColour, text, hpad);
		}
		printRaw(x, y, colour, text, hpad);
	};
	auto drawCenteredTextRaw = [&](const wstring& text,
	    int boxX, int boxY, int boxWidth, int boxHeight,
	    float offsetX, float offsetY, float scale, pix_t colour) {
		SudoFontMeta::pix_t c = { colour.r, colour.g, colour.b, colour.a };
		font->BlitTextCentered(text, boxX, boxY, boxWidth, boxHeight,
		    desc.Width, desc.Height, offsetX, offsetY, scale,
		    c, (SudoFontMeta::pix_t*)pixels);
	};
	auto drawCenteredText = [&](const wstring& text,
	    int boxX, int boxY, int boxWidth, int boxHeight,
	    float offsetX, float offsetY, float scale, pix_t colour, bool outline) {
		if (effectiveFontGlowStrength > 0 && effectiveFontGlowAlpha > 0) {
			const int innerRadius = std::max(1, effectiveFontGlowRadius / 2);
			const float outerScale = innerRadius == effectiveFontGlowRadius ? 1.0f : 0.55f;
			SudoFontMeta::pix_t outerColour = {
				VS.fontGlowColor[0], VS.fontGlowColor[1], VS.fontGlowColor[2],
				uint8_t(std::clamp(int(std::round(effectiveFontGlowAlpha * outerScale)), 0, 255))
			};
			SudoFontMeta::pix_t innerColour = {
				VS.fontGlowColor[0], VS.fontGlowColor[1], VS.fontGlowColor[2],
				uint8_t(std::clamp(int(std::round(effectiveFontGlowAlpha * 0.82f)), 0, 255))
			};
			font->BlitTextGlowCentered(text, boxX, boxY, boxWidth, boxHeight,
			    desc.Width, desc.Height, offsetX, offsetY, scale,
			    effectiveFontGlowRadius, innerRadius,
			    outerColour, innerColour,
			    reinterpret_cast<SudoFontMeta::pix_t*>(pixels));
		}
		if (outline) {
			pix_t outlineColour = tp(effectiveLabelOutlineColor);
			for (int index = 0; index < 8; ++index) {
				drawCenteredTextRaw(text, boxX, boxY, boxWidth, boxHeight,
				    offsetX + TEXT_OUTLINE_X[index], offsetY + TEXT_OUTLINE_Y[index],
				    scale, outlineColour);
			}
		}
		drawCenteredTextRaw(text, boxX, boxY, boxWidth, boxHeight,
		    offsetX, offsetY, scale, colour);
	};

	// Scaled-down print function (renders at 1/3 scale for small warnings)
	auto printSmall = [&](int x, int y, pix_t colour, wstring text) {
		// Create temp buffer for full-size rendering
		int maxCharWidth = 50; // Assume max char width
		int lineHeight = (int)font->GetLineHeight();
		int fullWidth = 0;
		for (size_t i = 0; i < text.length(); i++) {
			fullWidth += font->Width(text[i]);
		}

		std::vector<pix_t> tempPixels(fullWidth * lineHeight, {0, 0, 0, 0});

		// Render to temp buffer
		SudoFontMeta::pix_t c = { colour.r, colour.g, colour.b, colour.a };
		int tempX = 0;
		for (size_t i = 0; i < text.length(); i++) {
			font->Blit(text[i], tempX, 0, fullWidth, c, (SudoFontMeta::pix_t*)tempPixels.data(), false);
			tempX += font->Width(text[i]);
		}

		// Downsample to 1/3 size
		int scale = 3;
		int smallWidth = fullWidth / scale;
		int smallHeight = lineHeight / scale;
		for (int sy = 0; sy < smallHeight; sy++) {
			for (int sx = 0; sx < smallWidth; sx++) {
				int srcX = sx * scale;
				int srcY = sy * scale;
				int srcIdx = srcY * fullWidth + srcX;
				if (srcIdx < (int)tempPixels.size()) {
					int dstX = x + sx;
					int dstY = y + sy;
					if (dstX >= 0 && dstX < (int)desc.Width && dstY >= 0 && dstY < (int)desc.Height) {
						pix_t& src = tempPixels[srcIdx];
						if (src.a > 0) {
							pixels[dstY * desc.Width + dstX] = src;
						}
					}
				}
			}
		}
	};

	int marginH = 120;        // Horizontal margin from texture edge to key area
	int marginTop = 60;       // Vertical margin below grab bar before keys
	int marginBot = 50;       // Vertical margin from key area to bottom edge

	// No outer frame — parchment edges define the keyboard shape
	// Grab bar at top is invisible (no darkening or separator)

	// ── MODE / LOCK buttons — just above the text bar ──
	{
		int textBarY = GRAB_BAR_HEIGHT + marginTop; // where text bar starts
		int btnH = 32;
		int btnGap = 4; // gap between buttons and text bar
		int btnY = textBarY - btnH - btnGap;
		int fontH = (int)font->GetLineHeight();

		bool modeHover = (laserOnConsole[0] || laserOnConsole[1]);
		bool lockHover = (laserOnToggle[0] || laserOnToggle[1]);

		// Button positions aligned with text bar, plus per-theme art nudges
		const auto& modeDesign = layout->GetModeButtonDesign();
		const auto& lockDesign = layout->GetLockButtonDesign();
		int modeBtnW = modeDesign.width > 0 ? std::max(8, int(std::round(modeDesign.width))) : CONSOLE_BTN_WIDTH;
		int modeBtnH = modeDesign.height > 0 ? std::max(8, int(std::round(modeDesign.height))) : btnH;
		int lockBtnW = lockDesign.width > 0 ? std::max(8, int(std::round(lockDesign.width))) : TOGGLE_BTN_WIDTH;
		int lockBtnH = lockDesign.height > 0 ? std::max(8, int(std::round(lockDesign.height))) : btnH;
		int modeBtnX = marginH + T.modeBtnOffX + int(std::round(layout->GetModeButtonOffsetX()));
		int modeBtnY = btnY + T.modeBtnOffY + int(std::round(layout->GetModeButtonOffsetY()));
		int lockBtnX = (int)desc.Width - marginH - TOGGLE_BTN_WIDTH + T.lockBtnOffX + int(std::round(layout->GetLockButtonOffsetX()));
		int lockBtnY = btnY + T.lockBtnOffY + int(std::round(layout->GetLockButtonOffsetY()));

		// ── MODE toggle button (left) ──
		const wchar_t* modeLabel = sendInputOnly ? L"PC MODE" : L"VR MODE";
		const uint8_t* modeFill = customStyle
		    ? ((modeHover && VS.hoverEnabled) ? customHoverFill : sendInputOnly ? customActiveFill : effectivePlateFill)
		    : modeHover ? T.btnFillHover : sendInputOnly ? T.btnFillActive : T.btnFillIdle;
		if (VS.topButtonPlatesEnabled) {
			if (modernKeys) {
				paintModernPlate(modeBtnX, modeBtnY, modeBtnW, modeBtnH, modeFill, modeHover || sendInputOnly);
			} else {
				fillArea(modeBtnX, modeBtnY, modeBtnW, modeBtnH, KBT4(btnBorder));
				fillArea(modeBtnX + 1, modeBtnY + 1, modeBtnW - 2, modeBtnH - 2,
				    modeFill[0], modeFill[1], modeFill[2], modeFill[3]);
			}
		}
		const DecodedKeyboardArtwork& modeArtwork = sendInputOnly
		    ? customModePcArtwork : customModeVrArtwork;
		const bool hasModeArtwork = !modeArtwork.pixels.empty();
		if (hasModeArtwork)
			compositeArtwork(modeArtwork);
		bool lowOpacity = !customStyle && T.opacityInkFlip && (s_opacityPercent <= 5);
		pix_t modeColour = lowOpacity
		    ? pix_t{ 255, 255, 255, 255 }
		    : customStyle
		        ? tp(modeHover && VS.hoverEnabled ? effectiveHoverInk : effectiveInk)
		        : modeHover
		            ? tp(T.btnInkHover)
		            : sendInputOnly ? tp(T.btnInkActive) : tp(T.btnInkIdle);
		if ((!hasModeArtwork || layout->GetModeTextOverArtwork()) && modeDesign.fontScale > 0) {
			drawCenteredText(modeLabel, modeBtnX, modeBtnY, modeBtnW, modeBtnH,
			    layout->GetModeTextOffsetX(), layout->GetModeTextOffsetY(),
			    modeDesign.fontScale, modeColour, effectiveOutline);
		} else if (!hasModeArtwork || layout->GetModeTextOverArtwork()) {
			int modeTextW = font->Width(modeLabel);
			int modeTextYOff = (modeBtnH - fontH) / 2 + 4 + int(std::round(layout->GetModeTextOffsetY()));
			print(modeBtnX + (modeBtnW - modeTextW) / 2 + int(std::round(layout->GetModeTextOffsetX())),
			    modeBtnY + modeTextYOff, modeColour, modeLabel, false);
		}

		// ── LOCK button (right) ──
		const uint8_t* lockFill = customStyle
		    ? ((lockHover && VS.hoverEnabled) ? customHoverFill : headLocked ? customActiveFill : effectivePlateFill)
		    : headLocked ? T.btnFillActive : lockHover ? T.btnFillHover : T.btnFillIdle;
		if (VS.topButtonPlatesEnabled) {
			if (modernKeys) {
				paintModernPlate(lockBtnX, lockBtnY, lockBtnW, lockBtnH, lockFill, headLocked || lockHover);
			} else {
				fillArea(lockBtnX, lockBtnY, lockBtnW, lockBtnH, KBT4(btnBorder));
				fillArea(lockBtnX + 1, lockBtnY + 1, lockBtnW - 2, lockBtnH - 2,
				    lockFill[0], lockFill[1], lockFill[2], lockFill[3]);
			}
		}
		const DecodedKeyboardArtwork& lockArtwork = headLocked
		    ? customLockHeadArtwork : customLockWorldArtwork;
		const bool hasLockArtwork = !lockArtwork.pixels.empty();
		if (hasLockArtwork)
			compositeArtwork(lockArtwork);
		pix_t lockColour = lowOpacity
		    ? pix_t{ 255, 255, 255, 255 }
		    : customStyle
		        ? tp(lockHover && VS.hoverEnabled ? effectiveHoverInk : effectiveInk)
		        : headLocked
		            ? tp(T.btnInkActive)
		            : lockHover ? tp(T.btnInkHover) : tp(T.ink);
		if ((!hasLockArtwork || layout->GetLockTextOverArtwork()) && lockDesign.fontScale > 0) {
			drawCenteredText(L"LOCK", lockBtnX, lockBtnY, lockBtnW, lockBtnH,
			    layout->GetLockTextOffsetX(), layout->GetLockTextOffsetY(),
			    lockDesign.fontScale, lockColour, effectiveOutline);
		} else if (!hasLockArtwork || layout->GetLockTextOverArtwork()) {
			int lockTextW = font->Width(L"LOCK");
			int lockTextYOff = (lockBtnH - fontH) / 2 + 4 + int(std::round(layout->GetLockTextOffsetY()));
			print(lockBtnX + (lockBtnW - lockTextW) / 2 + int(std::round(layout->GetLockTextOffsetX())),
			    lockBtnY + lockTextYOff, lockColour, L"LOCK", false);
		}
	}

	int kbWidth = layout->GetWidth();
	int availW = (int)desc.Width - 2 * marginH; // horizontal space for keys
	int keySize = ((availW - padding) / kbWidth) - padding;
	auto drawKey = [&](int x, int y, const KeyboardLayout::Key& key) {
		int width = (int)(keySize * key.w);
		int height = (int)(keySize * key.h);

		if (key.spansToRight) {
			width = (int)desc.Width - marginH - x;
		}

		bool highlighted = (key.ch == '\x01' && caseMode == ECaseMode::SHIFT)
		    || (key.ch == '\x02' && caseMode == ECaseMode::LOCK)
		    || (key.ch == '\x1E' && ctrlLatched);
		bool leftSel = (selected[vr::Eye_Left] == key.id);
		bool rightSel = (selected[vr::Eye_Right] == key.id);
		bool isPressed = (s_pressedKey[0] == key.id || s_pressedKey[1] == key.id);
		bool isHovered = (leftSel || rightSel) && !isPressed;
		int contentYOffset = isPressed ? 2 : isHovered ? -2 : 0;

		bool whiteInk = !customStyle && T.opacityInkFlip && (s_opacityPercent <= 5); // White text at low opacity for visibility

		// Very subtle 1px faint border around every key
		if (VS.keyPlatesEnabled && !modernKeys) {
			fillArea(x, y, width, 1, KBT4(keyBorder));               // top edge
			fillArea(x, y + height - 1, width, 1, KBT4(keyBorder));  // bottom edge
			fillArea(x, y, 1, height, KBT4(keyBorder));              // left edge
			fillArea(x + width - 1, y, 1, height, KBT4(keyBorder));  // right edge
		}

		// Key interior — mostly background showing through
		if (VS.keyPlatesEnabled && highlighted && !modernKeys) {
			// Active shift/caps — highlight tint
			fillArea(x + 1, y + 1, width - 2, height - 2, KBT4(keyFillHi));
		}
		// Normal keys: no fill — pure theme background

		// Controller selection highlights
		if (VS.keyPlatesEnabled && (leftSel || rightSel) && !modernKeys) {
			// Highlight for selected key — visible against the background
			fillArea(x + 1, y + 1, width - 2, height - 2, KBT4(keyFillSel));
		}
		if (VS.keyPlatesEnabled && modernKeys) {
			const uint8_t* fill = customStyle
			    ? (highlighted ? customActiveFill
			        : (leftSel || rightSel) && VS.hoverEnabled ? customHoverFill : effectivePlateFill)
			    : highlighted ? T.keyFillHi : (leftSel || rightSel) ? T.keyFillSel : T.keyFillIdle;
			paintModernPlate(x, y, width, height, fill, highlighted || leftSel || rightSel);
		}

		// Label ink — theme colors; parchment flips to white at very low opacity
		pix_t targetColour;
		if (highlighted && !customStyle) {
			targetColour = tp(T.inkHi);
		} else if ((leftSel || rightSel) && (!customStyle || VS.hoverEnabled)) {
			targetColour = tp(customStyle ? effectiveHoverInk : T.inkSel);
		} else {
			targetColour = whiteInk ? pix_t{ 255, 255, 255, 255 } : tp(effectiveInk);
		}

		// Check if this is the space bar - draw the space bar image
		bool isSpaceBar = (key.ch == ' ');
		// The classic ribbon remains the default for Parchment, but Keyboard
		// Studio can hide it or author its position/scale like any other key content.
		const bool showParchmentSpacebar = std::string(theme->name) == "parchment"
		    && VS.parchmentRibbonEnabled;
		if (isSpaceBar && showParchmentSpacebar && !s_spaceBarImage.empty()) {
			const float ribbonScale = std::clamp(key.labelScale, 0.25f, 3.0f);
			int drawW = std::max(1, int(std::round(width * ribbonScale)));
			int drawH = std::max(1, int(std::round(height * ribbonScale)));
			int drawX = x + (width - drawW) / 2 + int(std::round(key.labelOffsetX));
			int drawY = y + (height - drawH) / 2 + int(std::round(key.labelOffsetY)) + contentYOffset;

			// Blit the image stretched to fit, recolored to targetColour
			for (int dy = 0; dy < drawH; dy++) {
				for (int dx = 0; dx < drawW; dx++) {
					// Map from draw space to source image space
					int srcX = (int)((float)dx / drawW * s_spaceBarWidth);
					int srcY = (int)((float)dy / drawH * s_spaceBarHeight);

					if (srcX >= 0 && srcX < (int)s_spaceBarWidth && srcY >= 0 && srcY < (int)s_spaceBarHeight) {
						// RGBA format from lodepng
						int srcIdx = (srcY * s_spaceBarWidth + srcX) * 4;
						unsigned char r = s_spaceBarImage[srcIdx + 0];
						unsigned char g = s_spaceBarImage[srcIdx + 1];
						unsigned char b = s_spaceBarImage[srcIdx + 2];
						unsigned char alpha = s_spaceBarImage[srcIdx + 3];

						// Only draw dark pixels (the actual black scribble)
						// Average brightness must be < 180 to count as "dark enough"
						int brightness = (r + g + b) / 3;
						bool isDark = (brightness < 180);

						// Only draw dark pixels with high alpha
						if (alpha >= 250 && isDark) {
							int px = drawX + dx;
							int py = drawY + dy;
							if (px >= 0 && px < (int)desc.Width && py >= 0 && py < (int)desc.Height) {
								if (customStyle || T.tintSpacebar) {
									// Dark themes: recolor the black scribble to the theme ink
									pixels[px + py * desc.Width] = targetColour;
								} else if (whiteInk) {
									// Invert: black scribble becomes white at low opacity
									pixels[px + py * desc.Width] = { (uint8_t)(255 - r), (uint8_t)(255 - g), (uint8_t)(255 - b), 255 };
								} else {
									// Use original image colors
									pixels[px + py * desc.Width] = { r, g, b, 255 };
								}
							}
						}
					}
				}
			}
		}

		// Check if this is an arrow key - draw triangle instead of text
		bool isArrowKey = (key.ch == '\x04' || key.ch == '\x05' || key.ch == '\x06' || key.ch == '\x07');
		if (isArrowKey) {
			// Arrow-key content uses the same per-key placement controls as font
			// content. Keyboard Studio can therefore select, move and resize each
			// Up/Down/Left/Right triangle independently.
			float arrowScale = std::clamp(key.labelScale, 0.25f, 3.0f);
			int triH = std::max(4, int(std::round(16.0f * arrowScale)));
			int triW = std::max(4, int(std::round(20.0f * arrowScale)));
			int cx = x + width / 2 + int(std::round(key.labelOffsetX));
			int cy = y + height / 2 + int(std::round(key.labelOffsetY)) + contentYOffset;

			if (key.ch == '\x04') { // Up arrow - triangle pointing up
				int baseY = cy + triH / 2;
				for (int row = 0; row < triH; row++) {
					float frac = 1.0f - (float)row / (float)(triH - 1);
					int halfW = (int)(frac * triW / 2);
					for (int c = -halfW; c <= halfW; c++) {
						int px = cx + c;
						int py = baseY - row;
						if (px >= 0 && px < (int)desc.Width && py >= 0 && py < (int)desc.Height) {
							pixels[px + py * desc.Width] = targetColour;
						}
					}
				}
			} else if (key.ch == '\x05') { // Down arrow - triangle pointing down
				int baseY = cy - triH / 2;
				for (int row = 0; row < triH; row++) {
					float frac = 1.0f - (float)row / (float)(triH - 1);
					int halfW = (int)(frac * triW / 2);
					for (int c = -halfW; c <= halfW; c++) {
						int px = cx + c;
						int py = baseY + row;
						if (px >= 0 && px < (int)desc.Width && py >= 0 && py < (int)desc.Height) {
							pixels[px + py * desc.Width] = targetColour;
						}
					}
				}
			} else if (key.ch == '\x06') { // Left arrow - triangle pointing left
				int baseX = cx + triW / 2;
				for (int col = 0; col < triW; col++) {
					float frac = 1.0f - (float)col / (float)(triW - 1);
					int halfH = (int)(frac * triH / 2);
					for (int r = -halfH; r <= halfH; r++) {
						int px = baseX - col;
						int py = cy + r;
						if (px >= 0 && px < (int)desc.Width && py >= 0 && py < (int)desc.Height) {
							pixels[px + py * desc.Width] = targetColour;
						}
					}
				}
			} else if (key.ch == '\x07') { // Right arrow - triangle pointing right
				int baseX = cx - triW / 2;
				for (int col = 0; col < triW; col++) {
					float frac = 1.0f - (float)col / (float)(triW - 1);
					int halfH = (int)(frac * triH / 2);
					for (int r = -halfH; r <= halfH; r++) {
						int px = baseX + col;
						int py = cy + r;
						if (px >= 0 && px < (int)desc.Width && py >= 0 && py < (int)desc.Height) {
							pixels[px + py * desc.Width] = targetColour;
						}
					}
				}
			}
		} else if (!isSpaceBar) {
			wstring label = caseMode == ECaseMode::LOWER ? key.label : key.labelShift;
			// The shared physical key says Done only while Skyrim owns the text
			// request; console and player-opened keyboards still show Enter.
			if (key.ch == '\n' && !sendInputOnly && !consoleActive)
				label = L"Done";

			// Calculate vertical offset: hover = up 2px, pressed = down 1px
			int yOffset = contentYOffset;

			// Every key now uses the same visible-bounds centering path. The old
			// split renderer centered one-character keys by bitmap bounds but put
			// words such as Shift and F10 on a font baseline, which is why those
			// labels visibly sat lower in OCU Nordic. Keyboard Studio offsets and
			// scaling feed this same renderer, so its preview matches the game.
			if (isHovered && (!customStyle || VS.hoverEnabled))
				drawCenteredTextRaw(label, x, y, width, height,
				    key.labelOffsetX, key.labelOffsetY + yOffset + 2,
				    key.labelScale, pix_t{ 0, 0, 0, 80 });
			drawCenteredText(label, x, y, width, height,
			    key.labelOffsetX, key.labelOffsetY + yOffset,
			    key.labelScale, targetColour, effectiveOutline);
		}
	};

	int keyAreaBaseY = (minimal ? marginTop : marginTop + keySize + padding) + GRAB_BAR_HEIGHT;
	for (const KeyboardLayout::Key& key : layout->GetKeymap()) {
		int x = marginH + (int)((keySize + padding) * key.x);
		int y = keyAreaBaseY + (int)((keySize + padding) * key.y);

		drawKey(x, y, key);
	}

	// ── Warning text below spacebar (scaled down 3x) ──
	// DISABLED: WASD blocking is now handled automatically via menu detection
	/*{
		std::wstring warningText = L"Warning: do not use WASD keys in walking mode, it can crash your game!";
		int fullWidth = 0;
		for (size_t i = 0; i < warningText.length(); i++) {
			fullWidth += font->Width(warningText[i]);
		}
		int smallWidth = fullWidth / 3; // 1/3 scale
		int textX = ((int)desc.Width - smallWidth) / 2; // Center horizontally
		int textY = keyAreaBaseY + (int)((keySize + padding) * 6) + 5; // Below bottom row
		pix_t textCol = { 70, 15, 15, 255 }; // Dark red warning color
		printSmall(textX, textY, textCol, warningText);
	}*/

	// ── Arrow control shared rendering ──
	// Theme ink; parchment flips to white at very low opacity for visibility
	bool useWhiteInk = !customStyle && T.opacityInkFlip && (s_opacityPercent <= 5);
	pix_t inkCol = useWhiteInk ? pix_t{ 255, 255, 255, 255 } : tp(effectiveInk);
	pix_t hoverCol = useWhiteInk ? pix_t{ 200, 200, 200, 255 }
	    : tp(customStyle && VS.hoverEnabled ? effectiveHoverInk : T.arrowHover);
	// Lambda to draw a filled triangle
	auto drawTriangle = [&](int cx, int baseY, int aH, int aW, bool pointUp, pix_t col) {
		for (int row = 0; row < aH; row++) {
			float frac = (float)row / (float)(aH - 1);
			int halfW = (int)((pointUp ? frac : (1.0f - frac)) * aW / 2);
			for (int c = -halfW; c <= halfW; c++) {
				int px = cx + c;
				int py = baseY + row;
				if (px >= 0 && px < (int)desc.Width && py >= 0 && py < (int)desc.Height) {
					pix_t& p = pixels[px + py * desc.Width];
					const float alpha = col.a / 255.0f;
					p.r = uint8_t(col.r * alpha + p.r * (1.0f - alpha));
					p.g = uint8_t(col.g * alpha + p.g * (1.0f - alpha));
					p.b = uint8_t(col.b * alpha + p.b * (1.0f - alpha));
					p.a = uint8_t(std::min(255.0f, col.a + p.a * (1.0f - alpha)));
				}
			}
		}
	};
		auto drawControlArrow = [&](int left, int topY, int arrowW, int arrowH,
	                            bool pointUp, pix_t col, bool hover) {
		const int cx = left + arrowW / 2;
		if (hover) {
			fillArea(left - 3, topY - 3, arrowW + 6, arrowH + 6,
			    effectiveHoverPlate[0], effectiveHoverPlate[1], effectiveHoverPlate[2], effectiveHoverPlate[3]);
		}
		if (layout->GetControlArrowGlowEnabled() && layout->GetControlArrowGlowStrength() > 0) {
			KeyboardLayout::ImageLayer glowPulse;
			glowPulse.breatheEnabled = layout->GetControlArrowBreatheEnabled();
			glowPulse.breatheMinPercent = layout->GetControlArrowBreatheMinPercent();
			glowPulse.breathePeriodSeconds = layout->GetControlArrowBreathePeriodSeconds();
			glowPulse.breathePhaseDegrees = layout->GetControlArrowBreathePhaseDegrees();
			const float pulse = breatheMultiplier(glowPulse);
			const uint8_t* glow = layout->GetControlArrowGlowColor();
			const int radius = std::clamp(layout->GetControlArrowGlowRadius(), 1, 48);
			const float strength = std::clamp(layout->GetControlArrowGlowStrength(), 0, 100) / 100.0f;
			for (int ring = radius; ring >= 1; --ring) {
				const float falloff = 1.0f - float(ring) / float(radius + 1);
				const uint8_t alpha = uint8_t(std::clamp(glow[3] * strength * pulse * falloff * 0.18f, 0.0f, 255.0f));
				drawTriangle(cx, topY - ring, arrowH + ring * 2, arrowW + ring * 2,
				    pointUp, pix_t{ glow[0], glow[1], glow[2], alpha });
			}
		}
		if (!customControlArrow.pixels.empty()) {
			KeyboardLayout::ImageLayer placement = customControlArrow.placement;
			placement.x = float(left);
			placement.y = float(topY);
			placement.width = float(arrowW);
			placement.height = float(arrowH);
			placement.opacity = 100;
			placement.edgeFade = 0;
			placement.rotation += pointUp ? 0.0f : 180.0f;
			compositeArtwork(customControlArrow, 1.0f, 0.0f, &placement);
		} else {
			drawTriangle(cx, topY, arrowH, arrowW, pointUp, col);
		}
	};
	auto drawControl = [&](int groupLeft, int groupTop,
	                       const KeyboardLayout::ControlDesign& design,
	                       const wchar_t* label, const wchar_t* value,
	                       bool upHover, bool downHover) {
		const int groupWidth = std::max(24, int(std::round(design.width)));
		const int upWidth = std::max(4, int(std::round(design.upWidth)));
		const int upHeight = std::max(4, int(std::round(design.upHeight)));
		const int upLeft = groupLeft + int(std::round((design.width - design.upWidth) / 2.0f + design.upOffsetX));
		const int upTop = groupTop + int(std::round(design.upOffsetY));
		const int downWidth = std::max(4, int(std::round(design.downWidth)));
		const int downHeight = std::max(4, int(std::round(design.downHeight)));
		const int downLeft = groupLeft + int(std::round((design.width - design.downWidth) / 2.0f + design.downOffsetX));
		const int downTop = groupTop + int(std::round(design.height - design.downHeight + design.downOffsetY));

		drawControlArrow(upLeft, upTop, upWidth, upHeight, true,
		    upHover ? hoverCol : inkCol, upHover);
		drawCenteredText(label, groupLeft, groupTop + 21, groupWidth, 25,
		    design.labelOffsetX, design.labelOffsetY, design.labelScale,
		    inkCol, customStyle && effectiveOutline);
		drawCenteredText(value, groupLeft, groupTop + 45, groupWidth, 25,
		    design.valueOffsetX, design.valueOffsetY, design.valueScale,
		    inkCol, customStyle && effectiveOutline);
		drawControlArrow(downLeft, downTop, downWidth, downHeight, false,
		    downHover ? hoverCol : inkCol, downHover);
	};

	// ── Opacity & Tilt controls in RIGHT margin ──
	{

		// ── OPACITY section ── (top, just below grab bar)

		bool opacUpHover = (laserOnOpacityUp[0] || laserOnOpacityUp[1]);
		bool opacDownHover = (laserOnOpacityDown[0] || laserOnOpacityDown[1]);

		wchar_t opacValue[32];
		swprintf(opacValue, 32, L"%d%%", s_opacityPercent);
		drawControl(int(desc.Width) - 106 + int(std::round(layout->GetOpacityControlOffsetX())),
		    60 + int(std::round(layout->GetOpacityControlOffsetY())),
		    layout->GetOpacityControlDesign(), L"opac", opacValue,
		    opacUpHover, opacDownHover);

		// ── TILT section ── (below opacity, double separation)
		bool tiltUpHover = (laserOnTiltUp[0] || laserOnTiltUp[1]);
		bool tiltDownHover = (laserOnTiltDown[0] || laserOnTiltDown[1]);

		wchar_t tiltValue[32];
		swprintf(tiltValue, 32, L"%.0f", s_tiltDegrees);
		drawControl(int(desc.Width) - 106 + int(std::round(layout->GetTiltControlOffsetX())),
		    270 + int(std::round(layout->GetTiltControlOffsetY())),
		    layout->GetTiltControlDesign(), L"tilt", tiltValue,
		    tiltUpHover, tiltDownHover);
	}

	// ── Size control in LEFT margin ── (moved to right a bit)
	{
		bool sizeUpHover = (laserOnSizeUp[0] || laserOnSizeUp[1]);
		bool sizeDownHover = (laserOnSizeDown[0] || laserOnSizeDown[1]);

		wchar_t sizeValue[32];
		swprintf(sizeValue, 32, L"%d%%", s_scalePercent);
		drawControl(39 + int(std::round(layout->GetSizeControlOffsetX())),
		    224 + int(std::round(layout->GetSizeControlOffsetY())),
		    layout->GetSizeControlDesign(), L"size", sizeValue,
		    sizeUpHover, sizeDownHover);
	}

	if (!minimal) {
		// Text input bar — subtle border on parchment (shifted below grab bar)
		const auto& textBarDesign = layout->GetTextBarDesign();
		int textBarX = marginH + int(std::round(layout->GetTextBarOffsetX()));
		int textBarY = GRAB_BAR_HEIGHT + marginTop + int(std::round(layout->GetTextBarOffsetY()));
		int textBarW = textBarDesign.width > 0 ? std::max(8, int(std::round(textBarDesign.width))) : (int)desc.Width - 2 * marginH;
		int textBarH = textBarDesign.height > 0 ? std::max(8, int(std::round(textBarDesign.height))) : keySize;
		if (VS.inputBarPlateEnabled) {
			// Visibility is independent of the text/caret hit area.
			const int borderWidth = 1;
			if (borderWidth > 0) {
				fillArea(textBarX, textBarY, textBarW, borderWidth,
				    effectiveTextBarBorder[0], effectiveTextBarBorder[1], effectiveTextBarBorder[2], effectiveTextBarBorder[3]);
				fillArea(textBarX, textBarY + textBarH - borderWidth, textBarW, borderWidth,
				    effectiveTextBarBorder[0], effectiveTextBarBorder[1], effectiveTextBarBorder[2], effectiveTextBarBorder[3]);
				fillArea(textBarX, textBarY, borderWidth, textBarH,
				    effectiveTextBarBorder[0], effectiveTextBarBorder[1], effectiveTextBarBorder[2], effectiveTextBarBorder[3]);
				fillArea(textBarX + textBarW - borderWidth, textBarY, borderWidth, textBarH,
				    effectiveTextBarBorder[0], effectiveTextBarBorder[1], effectiveTextBarBorder[2], effectiveTextBarBorder[3]);
			}
		}

		if (!sendInputOnly) {
			// Show typed text with blinking cursor (game-opened keyboard only)
			pix_t targetColour = useWhiteInk ? pix_t{ 255, 255, 255, 255 } : tp(effectiveInk);
			const float textScale = textBarDesign.fontScale > 0 ? textBarDesign.fontScale : 1.0f;
			const int textStartX = textBarX + BORD + 6;
			if (textBarDesign.fontScale > 0 && !text.empty()) {
				int textWidth = std::max(1, int(std::round(font->Width(text) * textScale)));
				drawCenteredText(text, textStartX, textBarY, textWidth, textBarH,
				    0, 0, textScale, targetColour, effectiveOutline);
			} else {
				print(textStartX, textBarY + BORD + 4, targetColour, text);
			}

			// Blinking text cursor
			bool cursorVisible = ((GetTickCount64() / 500) % 2) == 0;
			if (cursorVisible) {
				int cursorX = textStartX;
				for (int i = 0; i < cursorPos && i < (int)text.size(); i++)
					cursorX += std::max(1, int(std::round(font->Width(text[i]) * textScale)));
				int cursorY = textBarY + BORD + 2;
				int cursorH = std::max(2, textBarH - BORD * 2 - 4);
				fillArea(cursorX, cursorY, 2, cursorH, targetColour.r, targetColour.g, targetColour.b, 255);
			}
		}
	}

	QueryPerformanceCounter(&refreshCpuDone);

	// Acquire an image from the OpenXR swap chain
	XrSwapchainImageAcquireInfo acquireInfo = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	uint32_t currentIndex = 0;
	XrResult xrRes = xrAcquireSwapchainImage(chain, &acquireInfo, &currentIndex);
	if (XR_FAILED(xrRes)) {
		OOVR_LOG_LIMITEDF(5000, "[VRKeyboard] Refresh: xrAcquireSwapchainImage failed %d — skipping frame", xrRes);
		return;
	}

	// Wait for the image to be ready
	XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	waitInfo.timeout = 500000000; // 500ms
	xrRes = xrWaitSwapchainImage(chain, &waitInfo);
	if (XR_FAILED(xrRes)) {
		OOVR_LOG_LIMITEDF(5000, "[VRKeyboard] Refresh: xrWaitSwapchainImage failed %d — releasing and skipping", xrRes);
		XrSwapchainImageReleaseInfo rel = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		xrReleaseSwapchainImage(chain, &rel);
		return;
	}

	// Upload directly to the acquired swapchain image. The old path allocated a
	// fresh 1024x560 D3D texture and copied it on every breathing animation tick.
	ctx->UpdateSubresource(swapchainImages[currentIndex].texture, 0, nullptr,
	    pixels, sizeof(pix_t) * desc.Width, sizeof(pix_t) * desc.Width * desc.Height);

	// Release the swap chain image
	XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	xrReleaseSwapchainImage(chain, &releaseInfo);

	QueryPerformanceCounter(&refreshDone);
	const double frequency = double(performanceFrequency.QuadPart);
	const double cpuMs = (refreshCpuDone.QuadPart - refreshStart.QuadPart) * 1000.0 / frequency;
	const double totalMs = (refreshDone.QuadPart - refreshStart.QuadPart) * 1000.0 / frequency;
	const uint64_t now = GetTickCount64();
	if (totalMs >= 4.0 && now - lastSlowRefreshLogMs >= 5000) {
		lastSlowRefreshLogMs = now;
		OOVR_LOG_LIMITEDF(5000, "[VRKeyboard] Slow refresh: CPU %.2f ms, XR wait/upload %.2f ms, total %.2f ms",
		    cpuMs, std::max(0.0, totalMs - cpuMs), totalMs);
	}
}

void VRKeyboard::RefreshConsole()
{
	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = consoleTexWidth;
	desc.Height = consoleTexHeight;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
	desc.SampleDesc = { 1, 0 };
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = 0;
	desc.CPUAccessFlags = 0;
	desc.MiscFlags = 0;

	consoleRenderBuffer.resize(static_cast<size_t>(desc.Width) * desc.Height);
	pix_t* pixels = reinterpret_cast<pix_t*>(consoleRenderBuffer.data());

	const int BORD = 2;
	const int PAD = 8;

	const KbThemeDef& T = *theme;
	const KeyboardLayout::VisualStyle& VS = layout->GetVisualStyle();
	const bool customStyle = VS.enabled;
	const uint8_t* effectiveConsoleBackground = VS.inputFillOverride ? VS.inputFillColor : T.consoleBg;
	const bool customInputOutline = VS.inputOutlineOverride;
	const uint8_t* effectiveConsoleBorder = customInputOutline
	    ? VS.inputOutlineColor : customStyle ? VS.keyColor : T.consoleBorder;
	const uint8_t* effectiveConsoleInk = customStyle ? VS.fontColor : T.consoleInk;
	const uint8_t* effectiveConsoleOutline = customStyle ? VS.fontOutlineColor : T.outline;
	const uint8_t* effectiveTitleFill = VS.inputFillOverride ? VS.inputFillColor : T.textBarBorder;
	auto tp = [](const uint8_t c[4]) { return pix_t{ c[0], c[1], c[2], c[3] }; };

	// Fill background — theme console panel color, semi-transparent
	for (UINT y = 0; y < desc.Height; y++) {
		for (UINT x = 0; x < desc.Width; x++) {
			pix_t& p = pixels[x + y * desc.Width];
			p.r = effectiveConsoleBackground[0];
			p.g = effectiveConsoleBackground[1];
			p.b = effectiveConsoleBackground[2];
			p.a = effectiveConsoleBackground[3];
		}
	}

	auto fillArea = [pixels, &desc](int x, int y, int w, int h, int r, int g, int b, int a = 200) {
		for (int ix = 0; ix < w; ix++) {
			for (int iy = 0; iy < h; iy++) {
				int px = x + ix, py = y + iy;
				if (px < 0 || py < 0 || px >= (int)desc.Width || py >= (int)desc.Height)
					continue;
				pix_t& p = pixels[px + py * desc.Width];
				float af = a / 255.0f;
				p.r = (uint8_t)(r * af + p.r * (1.0f - af));
				p.g = (uint8_t)(g * af + p.g * (1.0f - af));
				p.b = (uint8_t)(b * af + p.b * (1.0f - af));
				p.a = (uint8_t)std::min(255.0f, a + p.a * (1.0f - af));
			}
		}
	};

	const double animationSeconds = GetTickCount64() / 1000.0;
	auto breatheMultiplier = [animationSeconds](bool enabled, int minimumPercent,
	                              float periodSeconds, float phaseDegrees) {
		if (!enabled)
			return 1.0;
		const double period = std::max(0.1, double(periodSeconds));
		const double angle = animationSeconds * math_pi * 2.0 / period
		    + double(phaseDegrees) * math_pi / 180.0;
		const double wave = (std::sin(angle) + 1.0) * 0.5;
		const double minimum = std::clamp(minimumPercent, 0, 100) / 100.0;
		return minimum + (1.0 - minimum) * wave;
	};
	int effectiveFontGlowStrength = customStyle && VS.fontGlowEnabled
	    ? std::clamp(VS.fontGlowStrength, 0, 100) : 0;
	effectiveFontGlowStrength = int(std::round(effectiveFontGlowStrength * breatheMultiplier(
	    customStyle && VS.fontBreatheEnabled, VS.fontBreatheMinPercent,
	    VS.fontBreathePeriodSeconds, VS.fontBreathePhaseDegrees)));
	const int effectiveFontGlowRadius = customStyle ? std::clamp(VS.fontGlowRadius, 1, 8) : 0;
	const int effectiveFontGlowAlpha = customStyle
	    ? std::clamp(int(VS.fontGlowColor[3]) * effectiveFontGlowStrength / 100, 0, 255) : 0;
	static const int TEXT_OUTLINE_X[] = { -2, 2, 0, 0, -1, 1, -1, 1 };
	static const int TEXT_OUTLINE_Y[] = { 0, 0, -2, 2, -1, -1, 1, 1 };

	auto printRaw = [&](int x, int y, pix_t colour, const std::wstring& txt) {
		SudoFontMeta::pix_t c = { colour.r, colour.g, colour.b, colour.a };
		int cx = x;
		for (size_t i = 0; i < txt.length(); i++) {
			if (cx >= (int)desc.Width - PAD) break;
			wchar_t ch = txt[i];
			int w = font->Width(ch);
			font->Blit(ch, cx, y, desc.Width, c, (SudoFontMeta::pix_t*)pixels, true);
			cx += w;
		}
	};
	auto printLine = [&](int x, int y, pix_t colour, const std::wstring& txt) {
		if (effectiveFontGlowAlpha > 0) {
			auto stampGlow = [&](int radius, float alphaScale) {
				pix_t glow = { VS.fontGlowColor[0], VS.fontGlowColor[1], VS.fontGlowColor[2],
					uint8_t(std::clamp(int(std::round(effectiveFontGlowAlpha * alphaScale)), 0, 255)) };
				const int offsetX[] = { -radius, radius, 0, 0, -radius, radius, -radius, radius };
				const int offsetY[] = { 0, 0, -radius, radius, -radius, -radius, radius, radius };
				for (int index = 0; index < 8; ++index)
					printRaw(x + offsetX[index], y + offsetY[index], glow, txt);
			};
			const int innerRadius = std::max(1, effectiveFontGlowRadius / 2);
			stampGlow(effectiveFontGlowRadius,
			    innerRadius == effectiveFontGlowRadius ? 1.0f : 0.55f);
			if (innerRadius != effectiveFontGlowRadius)
				stampGlow(innerRadius, 0.82f);
		}
		if (customStyle && VS.labelOutline) {
			pix_t outline = tp(effectiveConsoleOutline);
			for (int index = 0; index < 8; ++index)
				printRaw(x + TEXT_OUTLINE_X[index], y + TEXT_OUTLINE_Y[index], outline, txt);
		}
		printRaw(x, y, colour, txt);
	};

	// Theme border (matches keyboard frame)
	const int consoleBorderWidth = !VS.inputOutlineVisible ? 0 : customInputOutline
	    ? std::clamp(VS.inputOutlineWidth, 0, 8)
	    : customStyle ? std::clamp(VS.plateOutlineWidth, 0, 8) : BORD;
	// Title bar — "INPUT" centered, doubled height so text fits cleanly
	const int titleH = 60;
	fillArea(consoleBorderWidth, consoleBorderWidth,
	    desc.Width - consoleBorderWidth * 2, titleH,
	    effectiveTitleFill[0], effectiveTitleFill[1], effectiveTitleFill[2], effectiveTitleFill[3]);

	// Optional artwork is composited into this existing console swapchain. It
	// adds no OpenXR overlay or swapchain and does not consume overlay budget.
	if (!customConsoleInputBg.pixels.empty()) {
		const int artworkX = int(std::round(customConsoleInputBg.placement.x));
		const int artworkY = int(std::round(customConsoleInputBg.placement.y));
		for (unsigned int y = 0; y < customConsoleInputBg.height; ++y) {
			const int targetY = artworkY + int(y);
			if (targetY < 0 || targetY >= int(desc.Height))
				continue;
			for (unsigned int x = 0; x < customConsoleInputBg.width; ++x) {
				const int targetX = artworkX + int(x);
				if (targetX < 0 || targetX >= int(desc.Width))
					continue;
				const size_t source = (size_t(y) * customConsoleInputBg.width + x) * 4;
				const float alpha = customConsoleInputBg.pixels[source + 3] / 255.0f;
				if (alpha <= 0.0f)
					continue;
				pix_t& target = pixels[targetX + targetY * desc.Width];
				target.r = uint8_t(customConsoleInputBg.pixels[source + 0] * alpha + target.r * (1.0f - alpha));
				target.g = uint8_t(customConsoleInputBg.pixels[source + 1] * alpha + target.g * (1.0f - alpha));
				target.b = uint8_t(customConsoleInputBg.pixels[source + 2] * alpha + target.b * (1.0f - alpha));
				target.a = uint8_t(std::min(255.0f,
				    customConsoleInputBg.pixels[source + 3] + target.a * (1.0f - alpha)));
			}
		}
	}
	if (consoleBorderWidth > 0) {
		fillArea(0, 0, desc.Width, consoleBorderWidth,
		    effectiveConsoleBorder[0], effectiveConsoleBorder[1], effectiveConsoleBorder[2], effectiveConsoleBorder[3]);
		fillArea(0, desc.Height - consoleBorderWidth, desc.Width, consoleBorderWidth,
		    effectiveConsoleBorder[0], effectiveConsoleBorder[1], effectiveConsoleBorder[2], effectiveConsoleBorder[3]);
		fillArea(0, 0, consoleBorderWidth, desc.Height,
		    effectiveConsoleBorder[0], effectiveConsoleBorder[1], effectiveConsoleBorder[2], effectiveConsoleBorder[3]);
		fillArea(desc.Width - consoleBorderWidth, 0, consoleBorderWidth, desc.Height,
		    effectiveConsoleBorder[0], effectiveConsoleBorder[1], effectiveConsoleBorder[2], effectiveConsoleBorder[3]);
		fillArea(consoleBorderWidth, consoleBorderWidth + titleH,
		    desc.Width - consoleBorderWidth * 2, consoleBorderWidth,
		    effectiveConsoleBorder[0], effectiveConsoleBorder[1], effectiveConsoleBorder[2], effectiveConsoleBorder[3]);
	}

	pix_t titleColour = tp(effectiveConsoleInk);
	int titleTextW = font->Width(L"INPUT");
	int fontH = (int)font->GetLineHeight();
	int titleTextX = (int)desc.Width / 2 - titleTextW / 2
	    + int(std::round(VS.inputTitleOffsetX));
	int titleTextY = consoleBorderWidth + (titleH - fontH) / 2
	    + int(std::round(VS.inputTitleOffsetY));
	printLine(titleTextX, titleTextY, titleColour, L"INPUT");

	// Input text with blinking cursor — centered vertically in remaining space
	int contentTop = consoleBorderWidth + titleH + consoleBorderWidth;
	int contentH = (int)desc.Height - contentTop - consoleBorderWidth;
	int inputX = PAD + consoleBorderWidth + int(std::round(VS.inputTextOffsetX));
	int inputY = contentTop + (contentH - fontH) / 2
	    + int(std::round(VS.inputTextOffsetY));
	pix_t textColour = tp(effectiveConsoleInk);

	printLine(inputX, inputY, textColour, text);

	// Blinking cursor at end of text
	bool cursorVisible = ((GetTickCount64() / 500) % 2) == 0;
	if (cursorVisible) {
		int cursorX = inputX;
		for (size_t i = 0; i < text.size(); i++)
			cursorX += font->Width(text[i]);
		fillArea(cursorX, inputY, 2, fontH,
		    effectiveConsoleInk[0], effectiveConsoleInk[1], effectiveConsoleInk[2], effectiveConsoleInk[3]);
	}

	XrSwapchainImageAcquireInfo acq = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	uint32_t idx = 0;
	XrResult xrRes = xrAcquireSwapchainImage(consoleChain, &acq, &idx);
	if (XR_FAILED(xrRes)) {
		OOVR_LOG_LIMITEDF(5000, "[VRKeyboard] RefreshConsole: xrAcquireSwapchainImage failed %d — skipping", xrRes);
		return;
	}
	XrSwapchainImageWaitInfo wait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	wait.timeout = 500000000;
	xrRes = xrWaitSwapchainImage(consoleChain, &wait);
	if (XR_FAILED(xrRes)) {
		OOVR_LOG_LIMITEDF(5000, "[VRKeyboard] RefreshConsole: xrWaitSwapchainImage failed %d — releasing and skipping", xrRes);
		XrSwapchainImageReleaseInfo rel = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		xrReleaseSwapchainImage(consoleChain, &rel);
		return;
	}
	ctx->UpdateSubresource(consoleSwapImages[idx].texture, 0, nullptr,
	    pixels, sizeof(pix_t) * desc.Width, sizeof(pix_t) * desc.Width * desc.Height);
	XrSwapchainImageReleaseInfo rel = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	xrReleaseSwapchainImage(consoleChain, &rel);
}

void VRKeyboard::SubmitEvent(vr::EVREventType ev, wchar_t ch)
{
	// Here's how (from some basic experimentation) the SteamVR keyboard appears to submit events:
	// In minimal mode:
	// * Pressing a key submits a KeyboardCharInput event, with the character stored in cNewInput
	//    TODO find out how this is encoeded with unicode characters
	// * Clicking of the keyboard submits a KeyboardClosed event, with cNewInput empty (all zeros)
	// * Clicking 'done' submits a KeyboardDone event, with cNewInput empty
	// In standard mode:
	// * cNewInput is always empty
	// * Pressing a key submits a KeyboardCharInput event (the app must read
	//    the text via GetKeyboardText if it wants to know the keyboard contents, since cNewInput is empty)
	// * Clicking of the keyboard submits a KeyboardClosed event
	// * Clicking 'done' submits a KeyboardCharInput event, followed by a KeyboardDone event

	vr::VREvent_Keyboard_t data = { 0 };
	data.uUserValue = userValue;

	memset(data.cNewInput, 0, sizeof(data.cNewInput));

	if (ch != 0) {
		string utf8 = CHAR_CONV.to_bytes(ch);

		if (utf8.length() > sizeof(data.cNewInput)) {
			OOVR_ABORTF("Cannot write symbol '%s' with too many bytes (%d bytes UTF8)", utf8.c_str(), (int)utf8.length());
		}

		memcpy(data.cNewInput, utf8.c_str(), utf8.length());
	}

	vr::VREvent_t evt = { 0 };
	evt.eventType = ev;
	evt.trackedDeviceIndex = 0; // This is accurate to SteamVR
	evt.data.keyboard = data;

	eventDispatch(evt);

	// Signal the SKSE plugin (OpenCompositeInput) when keyboard completes
#ifdef _WIN32
	if (ev == vr::VREvent_KeyboardDone || ev == vr::VREvent_KeyboardClosed) {
		constexpr UINT WM_OC_KEYBOARD = WM_APP + 0x4F43;
		HWND hwnd = GetGameWindow();
		if (hwnd) {
			WPARAM result = (ev == vr::VREvent_KeyboardDone) ? 1 : 0;
			PostMessageW(hwnd, WM_OC_KEYBOARD, result, 0);
		}
	}
#endif
}

void VRKeyboard::SetSendInputOnly(bool enabled)
{
	OOVR_DEBUG_LOGF("[VKMODE] SetSendInputOnly(%s) -> %s", enabled ? "true" : "false",
	    enabled ? "PC MODE (scancodes for MCM/DirectInput)" : "VR MODE (text buffer, no scancodes)");
	if (sendInputOnly != enabled)
		ReleaseAllHeldPCKeys();
	sendInputOnly = enabled;
	// sendInputMode is always true — keyboard always uses SendInput
	dirty = true;
}
