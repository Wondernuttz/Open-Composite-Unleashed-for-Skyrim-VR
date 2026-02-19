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

#include "Misc/Config.h"
#include "Misc/lodepng.h"

#include <cmath>
#include <stdexcept>
#include <utility>
#include <vector>

// MCI for MP3 sound playback
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

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

	OOVR_LOGF("OpenComposite DLL loaded from: %S", path);

	std::wstring dir(path);
	auto pos = dir.find_last_of(L"\\/");
	if (pos != std::wstring::npos)
		dir = dir.substr(0, pos + 1);

	OOVR_LOGF("OpenComposite DLL directory: %S", dir.c_str());
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

// Sound functions moved after loadResource() definition (see below)

static void TriggerHaptic(int side)
{
	if (s_hapticStrength <= 0) {
		OOVR_LOGF("Haptic blocked: strength=%d", s_hapticStrength);
		return;
	}
	auto system = GetBaseSystem();
	auto input = GetBaseInput();
	if (!system || !input) {
		OOVR_LOG("Haptic blocked: no BaseSystem or BaseInput");
		return;
	}
	vr::ETrackedControllerRole role = (side == 0)
		? vr::TrackedControllerRole_LeftHand
		: vr::TrackedControllerRole_RightHand;
	vr::TrackedDeviceIndex_t deviceIndex = system->GetTrackedDeviceIndexForControllerRole(role);
	if (deviceIndex == vr::k_unTrackedDeviceIndexInvalid) {
		OOVR_LOGF("Haptic blocked: invalid device index for side %d", side);
		return;
	}
	// Scale haptic duration: 0-100% maps to 0-3000 microseconds
	unsigned short duration = (unsigned short)(s_hapticStrength * 30);
	float amplitude = s_hapticStrength / 100.0f;
	OOVR_LOGF("Triggering haptic: side=%d, strength=%d, duration=%d, amplitude=%.2f", side, s_hapticStrength, duration, amplitude);
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
	bool inKeyboardSection = false;
	char line[256];

	while (fgets(line, sizeof(line), f)) {
		if (line[0] == '[') {
			inKeyboardSection = (strstr(line, "[keyboard]") != nullptr);
			continue;
		}
		if (!inKeyboardSection)
			continue;

		float val;
		int ival;
		char sval[32];
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

// Send a virtual key press via Windows SendInput API.
// Sends BOTH a VK event and a scancode-only event so both the Windows message
// queue (for text input) and DirectInput (for game input like console toggle)
// receive the keystroke.
static void SendSingleVK(WORD vk)
{
	WORD scan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
	DWORD flags = (IsExtendedKey(vk) ? KEYEVENTF_EXTENDEDKEY : 0);

	INPUT inputs[4] = {};
	// VK-based down (Windows message queue)
	inputs[0].type = INPUT_KEYBOARD;
	inputs[0].ki.wVk = vk;
	inputs[0].ki.wScan = scan;
	inputs[0].ki.dwFlags = flags;
	// Scancode-only down (DirectInput)
	inputs[1].type = INPUT_KEYBOARD;
	inputs[1].ki.wVk = 0;
	inputs[1].ki.wScan = scan;
	inputs[1].ki.dwFlags = KEYEVENTF_SCANCODE | flags;
	// Scancode-only up
	inputs[2].type = INPUT_KEYBOARD;
	inputs[2].ki.wVk = 0;
	inputs[2].ki.wScan = scan;
	inputs[2].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP | flags;
	// VK-based up
	inputs[3].type = INPUT_KEYBOARD;
	inputs[3].ki.wVk = vk;
	inputs[3].ki.wScan = scan;
	inputs[3].ki.dwFlags = flags | KEYEVENTF_KEYUP;

	::SendInput(4, inputs, sizeof(INPUT));
}

// Post a character to the SKSE plugin (OpenCompositeInput) for direct
// Scaleform injection.  The SKSE plugin's WndProc hook catches this custom
// message and pushes a GFxCharEvent into the active Scaleform movie,
// completely bypassing the game's broken message loop / TranslateMessage.
static constexpr UINT WM_OC_CHAR = WM_APP + 0x4F45;

// lParam: 0 = GFxCharEvent (printable chars), 1 = GFxKeyEvent kKeyDown (control keys)
static void PostCharToGame(wchar_t ch, LPARAM mode = 0)
{
	HWND hwnd = GetGameWindow();
	if (hwnd)
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
// When ch != 0, we skip VK-based events and only send scancode events (for
// DirectInput), relying on PostCharToGame for Scaleform text input. This avoids
// double character entry: VK events produce WM_CHAR via TranslateMessage, and
// PostCharToGame also injects a GFxCharEvent — both reach Scaleform.
// When ch == 0 (console mode, control keys), VK events are included since
// PostCharToGame won't fire and the console needs WM_CHAR from TranslateMessage.
// Set postChar=false to send scancodes only (no VK, no PostCharToGame).
static void SendVirtualKey(WORD vk, bool shift, wchar_t ch = 0, bool postChar = true)
{
	WORD scan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
	WORD shiftScan = (WORD)MapVirtualKeyW(VK_SHIFT, MAPVK_VK_TO_VSC);

	std::vector<INPUT> inputs;

	if (shift) {
		INPUT in = {};
		in.type = INPUT_KEYBOARD;
		if (!ch) {
			// VK shift down (only when not using PostCharToGame)
			in.ki.wVk = VK_SHIFT;
			in.ki.wScan = shiftScan;
			in.ki.dwFlags = 0;
			inputs.push_back(in);
		}
		// Scancode shift down for DirectInput
		in.ki.wVk = 0;
		in.ki.wScan = shiftScan;
		in.ki.dwFlags = KEYEVENTF_SCANCODE;
		inputs.push_back(in);
	}

	if (!ch) {
		// VK-based key down (only when not using PostCharToGame)
		INPUT keyDown = {};
		keyDown.type = INPUT_KEYBOARD;
		keyDown.ki.wVk = vk;
		keyDown.ki.wScan = scan;
		keyDown.ki.dwFlags = 0;
		inputs.push_back(keyDown);
	}
	// Scancode-only key down for DirectInput
	INPUT scanDown = {};
	scanDown.type = INPUT_KEYBOARD;
	scanDown.ki.wVk = 0;
	scanDown.ki.wScan = scan;
	scanDown.ki.dwFlags = KEYEVENTF_SCANCODE;
	inputs.push_back(scanDown);

	// Scancode-only key up
	INPUT scanUp = {};
	scanUp.type = INPUT_KEYBOARD;
	scanUp.ki.wVk = 0;
	scanUp.ki.wScan = scan;
	scanUp.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
	inputs.push_back(scanUp);
	if (!ch) {
		// VK-based key up (only when not using PostCharToGame)
		INPUT keyUp = {};
		keyUp.type = INPUT_KEYBOARD;
		keyUp.ki.wVk = vk;
		keyUp.ki.wScan = scan;
		keyUp.ki.dwFlags = KEYEVENTF_KEYUP;
		inputs.push_back(keyUp);
	}

	if (shift) {
		INPUT in = {};
		in.type = INPUT_KEYBOARD;
		// Scancode shift up
		in.ki.wVk = 0;
		in.ki.wScan = shiftScan;
		in.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
		inputs.push_back(in);
		if (!ch) {
			// VK shift up (only when not using PostCharToGame)
			in.ki.wVk = VK_SHIFT;
			in.ki.dwFlags = KEYEVENTF_KEYUP;
			inputs.push_back(in);
		}
	}

	::SendInput((UINT)inputs.size(), inputs.data(), sizeof(INPUT));

	// Post character directly to SKSE plugin for Scaleform injection.
	// This is the ONLY path to Scaleform when ch != 0 (VK events skipped above).
	// When postChar=false, skip this — scancode alone suffices for DirectInput.
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
		OOVR_LOGF("Loaded SpaceBar.png from resource: %ux%u", s_spaceBarWidth, s_spaceBarHeight);
	}
}

static void InitSounds()
{
	if (s_soundsInitialized) return;
	s_soundsInitialized = true;

	// Load sounds from embedded resources
	s_hoverSoundData = loadResource(RES_O_SND_HOVER, RES_T_WAV);
	s_pressSoundData = loadResource(RES_O_SND_PRESS, RES_T_WAV);

	OOVR_LOGF("Loaded keyboard sounds from resources: hover=%zu bytes, press=%zu bytes",
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
	EnsureGameForeground();
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

#ifdef _WIN32
	// Bring game window to foreground immediately so SendInput keystrokes
	// reach the game. Done here (not on CONSOLE click) to give Windows
	// time to complete the focus switch before the user interacts.
	EnsureGameForeground();
#endif

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

		// Position: 80cm forward, 45cm below head (lower for angled typing)
		layer.pose.position = {
			headLoc.pose.position.x + headFwd.x * 0.80f,
			headLoc.pose.position.y - 0.45f,
			headLoc.pose.position.z + headFwd.z * 0.80f
		};

		// Orientation: face toward user + tilt
		float yaw = atan2f(headFwd.x, headFwd.z);
		s_lastYaw = yaw;
		layer.pose.orientation = buildTiltedOrientation(yaw, s_tiltDegrees);
	} else {
		// Fallback: default position facing -Z with tilt
		layer.pose.position = { 0.0f, 0.9f, -0.80f };
		s_lastYaw = 0.0f;
		layer.pose.orientation = buildTiltedOrientation(0.0f, s_tiltDegrees);
	}

	font = make_unique<SudoFontMeta>(loadResource(RES_O_FNT_PARCHMENT, RES_T_FNTMETA), loadResource(RES_O_FNT_PARCHMENT, RES_T_PNG));
	layout = make_unique<KeyboardLayout>(loadResource(RES_O_KB_EN_GB, RES_T_KBLAYOUT));

	// Load parchment background texture
	{
		auto bgData = loadResource(RES_O_BG_PARCHMENT, RES_T_PNG);
		lodepng::decode(parchmentBg, parchmentW, parchmentH, (const uint8_t*)bgData.data(), bgData.size(), LCT_RGBA, 8);
		OOVR_LOGF("Parchment bg loaded: %ux%u (%zu bytes)", parchmentW, parchmentH, parchmentBg.size());
	}

	// Create laser beam swapchains — tiny solid-color textures, one per hand.
	for (int i = 0; i < 2; i++) {
		XrSwapchainCreateInfo laserSci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
		laserSci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
		laserSci.format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		laserSci.sampleCount = 1;
		laserSci.width = 4;
		laserSci.height = 4;
		laserSci.faceCount = 1;
		laserSci.arraySize = 1;
		laserSci.mipCount = 1;

		OOVR_FAILED_XR_ABORT(xrCreateSwapchain(xr_session.get(), &laserSci, &laserChain[i]));

		uint32_t laserImgCount = 0;
		OOVR_FAILED_XR_ABORT(xrEnumerateSwapchainImages(laserChain[i], 0, &laserImgCount, nullptr));
		std::vector<XrSwapchainImageD3D11KHR> laserImgs(laserImgCount, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
		OOVR_FAILED_XR_ABORT(xrEnumerateSwapchainImages(laserChain[i], laserImgCount, &laserImgCount,
		    (XrSwapchainImageBaseHeader*)laserImgs.data()));

		// Warm white beam — semi-transparent
		uint8_t cr = 255, cg = 240, cb = 220, ca = 180;
		uint32_t packed = cr | (cg << 8) | (cb << 16) | (ca << 24);
		uint32_t colorPixels[16];
		for (int j = 0; j < 16; j++) colorPixels[j] = packed;

		D3D11_TEXTURE2D_DESC ltd = {};
		ltd.Width = 4;
		ltd.Height = 4;
		ltd.MipLevels = 1;
		ltd.ArraySize = 1;
		ltd.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		ltd.SampleDesc = { 1, 0 };
		ltd.Usage = D3D11_USAGE_DEFAULT;

		D3D11_SUBRESOURCE_DATA linit = { colorPixels, sizeof(uint32_t) * 4, sizeof(uint32_t) * 16 };
		CComPtr<ID3D11Texture2D> ltex;
		OOVR_FAILED_DX_ABORT(dev->CreateTexture2D(&ltd, &linit, &ltex));

		XrSwapchainImageAcquireInfo lacq = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
		uint32_t lidx = 0;
		OOVR_FAILED_XR_ABORT(xrAcquireSwapchainImage(laserChain[i], &lacq, &lidx));
		XrSwapchainImageWaitInfo lwait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
		lwait.timeout = 500000000;
		OOVR_FAILED_XR_ABORT(xrWaitSwapchainImage(laserChain[i], &lwait));
		ctx->CopyResource(laserImgs[lidx].texture, ltex);
		XrSwapchainImageReleaseInfo lrel = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		OOVR_FAILED_XR_ABORT(xrReleaseSwapchainImage(laserChain[i], &lrel));

		// Initialize the laser composition layer
		memset(&laserLayer[i], 0, sizeof(laserLayer[i]));
		laserLayer[i].type = XR_TYPE_COMPOSITION_LAYER_QUAD;
		laserLayer[i].layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		laserLayer[i].space = xr_gbl->floorSpace;
		laserLayer[i].eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		laserLayer[i].subImage.swapchain = laserChain[i];
		laserLayer[i].subImage.imageRect.offset = { 0, 0 };
		laserLayer[i].subImage.imageRect.extent = { 4, 4 };
		laserLayer[i].subImage.imageArrayIndex = 0;
	}

	// Create target dot swapchains — small white dots (2 controllers + 1 headset).
	for (int i = 0; i < 3; i++) {
		XrSwapchainCreateInfo dotSci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
		dotSci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
		dotSci.format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		dotSci.sampleCount = 1;
		dotSci.width = 4;
		dotSci.height = 4;
		dotSci.faceCount = 1;
		dotSci.arraySize = 1;
		dotSci.mipCount = 1;

		OOVR_FAILED_XR_ABORT(xrCreateSwapchain(xr_session.get(), &dotSci, &targetDotChain[i]));

		uint32_t dotImgCount = 0;
		OOVR_FAILED_XR_ABORT(xrEnumerateSwapchainImages(targetDotChain[i], 0, &dotImgCount, nullptr));
		std::vector<XrSwapchainImageD3D11KHR> dotImgs(dotImgCount, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
		OOVR_FAILED_XR_ABORT(xrEnumerateSwapchainImages(targetDotChain[i], dotImgCount, &dotImgCount,
		    (XrSwapchainImageBaseHeader*)dotImgs.data()));

		// Solid white dot — fully opaque
		uint8_t cr = 255, cg = 255, cb = 255, ca = 255;
		uint32_t packed = cr | (cg << 8) | (cb << 16) | (ca << 24);
		uint32_t colorPixels[16];
		for (int j = 0; j < 16; j++) colorPixels[j] = packed;

		D3D11_TEXTURE2D_DESC dtd = {};
		dtd.Width = 4;
		dtd.Height = 4;
		dtd.MipLevels = 1;
		dtd.ArraySize = 1;
		dtd.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		dtd.SampleDesc = { 1, 0 };
		dtd.Usage = D3D11_USAGE_DEFAULT;

		D3D11_SUBRESOURCE_DATA dinit = { colorPixels, sizeof(uint32_t) * 4, sizeof(uint32_t) * 16 };
		CComPtr<ID3D11Texture2D> dtex;
		OOVR_FAILED_DX_ABORT(dev->CreateTexture2D(&dtd, &dinit, &dtex));

		XrSwapchainImageAcquireInfo dacq = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
		uint32_t didx = 0;
		OOVR_FAILED_XR_ABORT(xrAcquireSwapchainImage(targetDotChain[i], &dacq, &didx));
		XrSwapchainImageWaitInfo dwait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
		dwait.timeout = 500000000;
		OOVR_FAILED_XR_ABORT(xrWaitSwapchainImage(targetDotChain[i], &dwait));
		ctx->CopyResource(dotImgs[didx].texture, dtex);
		XrSwapchainImageReleaseInfo drel = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		OOVR_FAILED_XR_ABORT(xrReleaseSwapchainImage(targetDotChain[i], &drel));

		// Initialize the target dot composition layer
		memset(&targetDotLayer[i], 0, sizeof(targetDotLayer[i]));
		targetDotLayer[i].type = XR_TYPE_COMPOSITION_LAYER_QUAD;
		targetDotLayer[i].layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		targetDotLayer[i].space = xr_gbl->floorSpace;
		targetDotLayer[i].eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		targetDotLayer[i].subImage.swapchain = targetDotChain[i];
		targetDotLayer[i].subImage.imageRect.offset = { 0, 0 };
		targetDotLayer[i].subImage.imageRect.extent = { 4, 4 };
		targetDotLayer[i].subImage.imageArrayIndex = 0;
		// Size: 0.01m (1cm) dot
		targetDotLayer[i].size.width = 0.01f;
		targetDotLayer[i].size.height = 0.01f;
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
		consoleLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
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
}

VRKeyboard::~VRKeyboard()
{
	if (crosshairChain != XR_NULL_HANDLE) {
		xrDestroySwapchain(crosshairChain);
		crosshairChain = XR_NULL_HANDLE;
	}
	if (consoleChain != XR_NULL_HANDLE) {
		xrDestroySwapchain(consoleChain);
		consoleChain = XR_NULL_HANDLE;
	}
	for (int i = 0; i < 2; i++) {
		if (laserChain[i] != XR_NULL_HANDLE) {
			xrDestroySwapchain(laserChain[i]);
			laserChain[i] = XR_NULL_HANDLE;
		}
	}
	for (int i = 0; i < 3; i++) {
		if (targetDotChain[i] != XR_NULL_HANDLE) {
			xrDestroySwapchain(targetDotChain[i]);
			targetDotChain[i] = XR_NULL_HANDLE;
		}
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

#ifdef _WIN32
	// Keep game window focused while keyboard is active so keystrokes reach it
	static ULONGLONG lastFocusCheck = 0;
	ULONGLONG now = GetTickCount64();
	if (now - lastFocusCheck > 500) { // check every 500ms
		lastFocusCheck = now;
		EnsureGameForeground();
	}

	// Periodically reload settings from INI so configurator changes apply live
	static ULONGLONG lastSettingsCheck = 0;
	if (now - lastSettingsCheck > 1000) { // check every 1000ms (1 second)
		lastSettingsCheck = now;
		LoadKeyboardSettings(); // Reloads all keyboard settings from INI
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
						OOVR_LOG("Config file changed, reloading settings...");
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
							OOVR_LOGF("Settings reloaded: tilt=%.1f opacity=%d scale=%d haptic=%d sounds=%d",
								s_tiltDegrees, s_opacityPercent, s_scalePercent, s_hapticStrength, s_soundsEnabled);
						} else {
							OOVR_LOG("Settings reload failed - could not read ini file");
						}
					}
				}
			}
		}

		// Laser pointer hit testing
		bool anyLaserActive = false;
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
				anyLaserActive = true;
				UpdateLaserBeam(side);
			}
		}

		if (anyLaserActive)
			dirty = true;

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
			if (!hasState[side])
				continue;

			bool trigNow = (states[side].ulButtonPressed & vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Trigger)) != 0;
			bool trigJustPressed = trigNow && !lastTriggerState[side];
			bool trigJustReleased = !trigNow && lastTriggerState[side];
			lastTriggerState[side] = trigNow;

			// Mode toggle button — switch between VR MODE and PC MODE
			if (trigJustPressed && laserOnConsole[side] && !grabActive) {
				sendInputOnly = !sendInputOnly;
				OOVR_LOGF("Mode toggle: sendInputOnly=%d (%s)", sendInputOnly, sendInputOnly ? "PC MODE" : "VR MODE");
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
					layer.pose.position = { 0.0f, -0.45f, -0.80f };
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
						layer.pose.position = {
							hl.pose.position.x + headFwd.x * 0.80f,
							hl.pose.position.y - 0.45f,
							hl.pose.position.z + headFwd.z * 0.80f
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
				int pad = 8;
				int spaceW = font->Width(L' ');
				int textStartX = pad + BORD + 6 + spaceW; // matches Refresh() cursor origin
				int relX = clickTexX - textStartX;

				// Walk through text characters to find nearest boundary
				int accumX = 0;
				int newPos = 0;
				for (int i = 0; i < (int)text.size(); i++) {
					int charW = font->Width(text[i]);
					if (relX < accumX + charW / 2)
						break;
					accumX += charW;
					newPos = i + 1;
				}
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
				} else if (trigNow) {
					// Intersect the laser ray with the ORIGINAL grab plane
					// (not the current keyboard position — avoids feedback lag)
					std::shared_ptr<BaseInput> input = GetBaseInput();
					if (input && input->AreActionsLoaded()) {
						XrSpace aimSpace = XR_NULL_HANDLE;
						input->GetHandSpace((vr::TrackedDeviceIndex_t)(side + 1), aimSpace, true);
						if (aimSpace != XR_NULL_HANDLE) {
							XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
							if (XR_SUCCEEDED(xrLocateSpace(aimSpace, layer.space,
							        xr_gbl->GetBestTime(), &loc))
							    && (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
							    && (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
								XrVector3f rayOrig = loc.pose.position;
								XrVector3f rayFwd = { 0, 0, -1 };
								XrVector3f rayDir;
								rotate_vector_by_quaternion(rayFwd, loc.pose.orientation, rayDir);

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
									if (t > 0.0f) {
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
	}

	if (dirty) {
		dirty = false;
		Refresh();
		if (consoleActive)
			consoleDirty = true; // keyboard text changed, update console overlay
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
				XrSpace aimSpace = XR_NULL_HANDLE;
				input->GetHandSpace((vr::TrackedDeviceIndex_t)(side + 1), aimSpace, true);
				if (aimSpace != XR_NULL_HANDLE) {
					XrSpaceLocation location = { XR_TYPE_SPACE_LOCATION };
					XrResult result = xrLocateSpace(aimSpace, xr_gbl->floorSpace,
					    xr_gbl->GetBestTime(), &location);
					if (XR_SUCCEEDED(result)
					    && (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
					    && (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
						// Project 3 meters forward from controller
						XrVector3f fwd = { 0.0f, 0.0f, -1.0f };
						XrVector3f dir;
						rotate_vector_by_quaternion(fwd, location.pose.orientation, dir);
						targetDotLayer[side].pose.position = {
							location.pose.position.x + dir.x * 3.0f,
							location.pose.position.y + dir.y * 3.0f,
							location.pose.position.z + dir.z * 3.0f
						};
						targetDotLayer[side].pose.orientation = { 0.0f, 0.0f, 0.0f, 1.0f };
						targetDotLayer[side].space = xr_gbl->floorSpace;
					}
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
			}
		}
	}

	// Build layer list: console (behind), keyboard, laser beams, target dots, crosshair (on top)
	if (consoleActive && consoleChain != XR_NULL_HANDLE)
		activeLayers.push_back((XrCompositionLayerBaseHeader*)&consoleLayer);
	activeLayers.push_back((XrCompositionLayerBaseHeader*)&layer);
	for (int side = 0; side < 2; side++) {
		if (laserActive[side])
			activeLayers.push_back((XrCompositionLayerBaseHeader*)&laserLayer[side]);
	}
	if (s_targetMode) {
		for (int i = 0; i < 3; i++) {
			if (targetDotChain[i] != XR_NULL_HANDLE)
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
	if (!input || !input->AreActionsLoaded())
		return -1;

	XrSpace aimSpace = XR_NULL_HANDLE;
	input->GetHandSpace((vr::TrackedDeviceIndex_t)(side + 1), aimSpace, true);
	if (aimSpace == XR_NULL_HANDLE)
		return -1;

	// Locate controller in the keyboard's reference space (viewSpace or floorSpace)
	XrSpaceLocation location = { XR_TYPE_SPACE_LOCATION };
	XrResult result = xrLocateSpace(aimSpace, layer.space,
	    xr_gbl->GetBestTime(), &location);

	if (XR_FAILED(result)
	    || !(location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
	    || !(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
		return -1;

	XrVector3f rayOrigin = location.pose.position;
	XrVector3f fwd = { 0.0f, 0.0f, -1.0f };
	XrVector3f rayDir;
	rotate_vector_by_quaternion(fwd, location.pose.orientation, rayDir);

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

		if (texY >= btnStripY && texY < btnStripY + btnH) {
			int modeBtnX = marginH;
			int modeBtnRight = marginH + CONSOLE_BTN_WIDTH;
			int lockBtnW = TOGGLE_BTN_WIDTH;
			int lockBtnX = (int)texWidth - marginH - lockBtnW;

			if (texX >= modeBtnX && texX < modeBtnRight) {
				laserOnConsole[side] = true;
				return -5; // mode button hit
			}
			if (texX >= lockBtnX && texX < (int)texWidth - marginH) {
				laserOnToggle[side] = true;
				return -3; // toggle button hit
			}
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
		int textBarY = GRAB_BAR_HEIGHT + marginTop;
		int textBarH = keySize;
		if (texY >= textBarY && texY < textBarY + textBarH
		    && texX >= marginH && texX < (int)texWidth - marginH) {
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

	// Hit-test opacity & tilt arrows in RIGHT margin (matches Refresh() layout)
	{
		int ctrlX = (int)texWidth - marginH + 10;
		int ctrlW = marginH - 20;
		int arrowH = 18;
		int arrowW = 22;
		int hitPad = 18; // extra padding around each arrow for easier VR laser targeting
		int fntH = (int)font->GetLineHeight();
		int centerX = ctrlX + ctrlW / 2;
		int hitLeft = centerX - arrowW - 5;
		int hitRight = centerX + arrowW + 5;

		// Opacity section positions (top, must match Refresh)
		int opacTopY = GRAB_BAR_HEIGHT + 8;
		int opacLabelY = opacTopY + arrowH + 4;
		int opacValY = opacLabelY + fntH + 1;
		int opacDownY = opacValY + fntH + 4;

		// Tilt section positions (below opacity, double separation, must match Refresh)
		int tiltTopY = opacDownY + arrowH + 100;
		int tiltLabelY = tiltTopY + arrowH + 4;
		int tiltValY = tiltLabelY + fntH + 1;
		int tiltDownY = tiltValY + fntH + 4;

		if (texX >= hitLeft && texX <= hitRight) {
			if (texY >= opacTopY - hitPad && texY < opacTopY + arrowH + hitPad) {
				laserOnOpacityUp[side] = true;
				return -8;
			}
			if (texY >= opacDownY - hitPad && texY < opacDownY + arrowH + hitPad) {
				laserOnOpacityDown[side] = true;
				return -9;
			}
			if (texY >= tiltTopY - hitPad && texY < tiltTopY + arrowH + hitPad) {
				laserOnTiltUp[side] = true;
				return -6;
			}
			if (texY >= tiltDownY - hitPad && texY < tiltDownY + arrowH + hitPad) {
				laserOnTiltDown[side] = true;
				return -7;
			}
		}
	}

	// Hit-test size arrows in LEFT margin (matches Refresh() layout)
	{
		int ctrlX = 35;
		int ctrlW = marginH - 20;
		int arrowH = 18;
		int arrowW = 22;
		int hitPad = 18; // extra padding around each arrow for easier VR laser targeting
		int fntH = (int)font->GetLineHeight();
		int centerX = ctrlX + ctrlW / 2;
		int hitLeft = centerX - arrowW - 5;
		int hitRight = centerX + arrowW + 5;

		// Vertically center the size section below grab bar
		int sectionH = arrowH + 4 + fntH + 1 + fntH + 4 + arrowH;
		int availH = (int)texHeight - GRAB_BAR_HEIGHT;
		int sizeTopY = GRAB_BAR_HEIGHT + (availH - sectionH) / 2;
		int sizeLabelY = sizeTopY + arrowH + 4;
		int sizeValY = sizeLabelY + fntH + 1;
		int sizeDownY = sizeValY + fntH + 4;

		if (texX >= hitLeft && texX <= hitRight) {
			if (texY >= sizeTopY - hitPad && texY < sizeTopY + arrowH + hitPad) {
				laserOnSizeUp[side] = true;
				return -10;
			}
			if (texY >= sizeDownY - hitPad && texY < sizeDownY + arrowH + hitPad) {
				laserOnSizeDown[side] = true;
				return -11;
			}
		}
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

	if (grip && !grip_last && !grabActive) {
		// If console overlay is active, send tilde to close it in-game too
		if (consoleActive) {
			VkMapping mapping = CharToVK(L'`');
			if (mapping.vk != 0)
				SendVirtualKey(mapping.vk, mapping.needsShift, 0);
			consoleActive = false;
			consoleDirty = true;
			OOVR_LOG("Grip pressed with console open — sent tilde to close console");
		}
		if (!sendInputOnly) {
			// Send Escape to dismiss SkyUI text input dialogs (only when game opened the keyboard)
			SendSingleVK(VK_ESCAPE);
			PostCharToGame(VK_ESCAPE, 1);
			SubmitEvent(VREvent_KeyboardClosed, 0);
		}
		// When opened via controller shortcut (sendInputOnly), just close — no Escape needed
		closed = true;
		return;
	}

	if (selected[side] < 0) {
		s_pressedKey[side] = -1; // No key selected, clear pressed state
		return; // No key selected — nothing to do
	}

	const KeyboardLayout::Key& key = layout->GetKeymap()[selected[side]];

	// Track pressed state for visual feedback
	if (trigger && laserActive[(int)side]) {
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

		if (trigJustPressed && onBackspace) {
			shouldFireBackspace = true;
			backspaceRepeating[side] = true;
			backspaceRepeatNext[side] = now + 400; // initial delay before repeat
		} else if (trigger && backspaceRepeating[side] && onBackspace) {
			if (now >= backspaceRepeatNext[side]) {
				shouldFireBackspace = true;
				backspaceRepeatNext[side] = now + 100; // repeat interval (faster than arrows)
			}
		}
		if (!trigger || !onBackspace) {
			backspaceRepeating[side] = false;
		}

		if (shouldFireBackspace && !trigJustPressed) {
			// Repeat fire — play sound/haptic and send backspace
			PlayPressSound();
			TriggerHaptic((int)side);
			SendSingleVK(VK_BACK);
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
			if (ch == '\x01' || ch == '\x02') {
				ECaseMode target = ch == '\x02' ? ECaseMode::LOCK : ECaseMode::SHIFT;
				caseMode = caseMode == target ? ECaseMode::LOWER : target;
			} else if (ch == '\b') {
				SendSingleVK(VK_BACK);
				if (!consoleActive) PostCharToGame(VK_BACK, 1); // GFxKeyEvent — skip for console (SendInput suffices)
				// Update internal buffer (game-opened keyboard OR console mode)
				if ((!sendInputOnly || consoleActive) && cursorPos > 0 && !text.empty()) {
					text.erase(cursorPos - 1, 1);
					cursorPos--;
					if (consoleActive) consoleDirty = true;
				}
			} else if (ch == '\x03') {
				// Done
				if (sendInputOnly) {
					// PC mode: text was injected via SendInput/GFx — confirm with Enter
					SendSingleVK(VK_RETURN);
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
				closed = true;
			} else if (ch == '\t') {
				SendSingleVK(VK_TAB);
				PostCharToGame(VK_TAB, 1); // GFxKeyEvent for Scaleform (SkyUI needs this!)
			} else if (ch == '\n') {
				SendSingleVK(VK_RETURN);
				if (!consoleActive) PostCharToGame(VK_RETURN, 1); // GFxKeyEvent — skip for console
				if (consoleActive) {
					text.clear();
					cursorPos = 0;
					consoleDirty = true;
				}
			} else if (ch == '\x04') {
				SendSingleVK(VK_UP);
			} else if (ch == '\x05') {
				SendSingleVK(VK_DOWN);
			} else if (ch == '\x06') {
				SendSingleVK(VK_LEFT);
			} else if (ch == '\x07') {
				SendSingleVK(VK_RIGHT);
			} else if (ch >= '\x10' && ch <= '\x1B') {
				// F1-F12 keys: \x10=F1, \x11=F2, ..., \x1B=F12
				int fNum = (ch - '\x10') + 1;
				WORD vk = VK_F1 + (fNum - 1);
				SendSingleVK(vk);
			} else if (ch == '\x0F') {
				// [M] key — toggle crosshair dot at gaze center
				crosshairVisible = !crosshairVisible;
				OOVR_LOGF("Crosshair: %s", crosshairVisible ? "ON" : "OFF");
			} else if (ch == '\x1C') {
				// [T] key — toggle target mode (show dots from controllers and headset)
				s_targetMode = !s_targetMode;
				OOVR_LOGF("Target mode: %s", s_targetMode ? "ON" : "OFF");
			} else if (ch == '\x1D') {
				SendSingleVK(VK_END);
			} else if (ch == '\x0E') {
				// ESC — send to SkyUI/menus to cancel text input (does NOT close keyboard)
				SendSingleVK(VK_ESCAPE);
			} else {
				// Tilde/backtick toggles console INPUT overlay
				if (ch == L'`' || ch == L'~') {
					consoleActive = !consoleActive;
					if (consoleActive) {
						text.clear();
						cursorPos = 0;
					}
					consoleDirty = true;
					OOVR_LOGF("Console overlay: %s", consoleActive ? "OPENED" : "CLOSED");
					// Send scancode for DirectInput console toggle (no VK, no PostCharToGame)
					SendSingleVK(VK_OEM_3);
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
					if (mapping.vk != 0)
						SendVirtualKey(mapping.vk, mapping.needsShift, ch);
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
			} else if (ch == '\x03') {
				if (inputMode != EGamepadTextInputMode::k_EGamepadTextInputModeSubmit)
					closed = true;
				if (!minimal)
					SubmitEvent(VREvent_KeyboardCharInput, 0);
				SubmitEvent(VREvent_KeyboardDone, 0);
			} else if (ch == '\x04' || ch == '\x05' || ch == '\x06' || ch == '\x07') {
				// Arrow keys — no-op in normal mode
			} else if (ch == '\x0E') {
				closed = true;
				SubmitEvent(VREvent_KeyboardClosed, 0);
				return;
			} else if (!minimal && ch == '\t') {
				// Silently soak up tabs
			} else if (!minimal && ch == '\n') {
				// Silently soak up newlines
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

void VRKeyboard::Refresh()
{
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

	pix_t* pixels = new pix_t[desc.Width * desc.Height];

	// ── Parchment background ──
	const int BORD = 2; // Key border thickness in pixels

	// Copy parchment texture as background with reduced opacity for see-through
	// Fill with transparency first
	memset(pixels, 0, desc.Width * desc.Height * sizeof(pix_t));

	// Copy parchment (which may be smaller than texture) centered or at top
	if (!parchmentBg.empty() && parchmentW > 0 && parchmentH > 0) {
		// Center horizontally, align to top vertically
		int offsetX = ((int)desc.Width - (int)parchmentW) / 2;
		int offsetY = 0; // Top-aligned

		float opacityFrac = s_opacityPercent / 100.0f;

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

	int padding = 6;          // Gap between keys

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
				// Keep the original parchment alpha (transparent edges stay transparent)
			}
		}
	};

	auto print = [&](int x, int y, pix_t colour, wstring text, bool hpad = true) {
		SudoFontMeta::pix_t c = { colour.r, colour.g, colour.b, colour.a };
		for (size_t i = 0; i < text.length(); i++) {
			font->Blit(text[i], x, y, desc.Width, c, (SudoFontMeta::pix_t*)pixels, hpad);
			x += font->Width(text[i]);
		}
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
		int textYOff = (btnH - fontH) / 2 + 4;

		bool modeHover = (laserOnConsole[0] || laserOnConsole[1]);
		bool lockHover = (laserOnToggle[0] || laserOnToggle[1]);

		// Button positions aligned with text bar (marginH to desc.Width - marginH)
		int modeBtnX = marginH;
		int modeBtnW = CONSOLE_BTN_WIDTH;
		int lockBtnW = TOGGLE_BTN_WIDTH;
		int lockBtnX = (int)desc.Width - marginH - lockBtnW;

		// ── MODE toggle button (left) ──
		const wchar_t* modeLabel = sendInputOnly ? L"PC MODE" : L"VR MODE";
		fillArea(modeBtnX, btnY, modeBtnW, btnH, 80, 55, 25, 60); // subtle border
		if (modeHover) {
			fillArea(modeBtnX + 1, btnY + 1, modeBtnW - 2, btnH - 2, 40, 25, 10, 120);
		} else if (sendInputOnly) {
			fillArea(modeBtnX + 1, btnY + 1, modeBtnW - 2, btnH - 2, 100, 70, 20, 80);
		} else {
			fillArea(modeBtnX + 1, btnY + 1, modeBtnW - 2, btnH - 2, 60, 40, 20, 30);
		}
		bool lowOpacity = (s_opacityPercent <= 5);
		pix_t modeColour = lowOpacity
		    ? pix_t{ 255, 255, 255, 255 }
		    : modeHover
		        ? pix_t{ 20, 10, 0, 255 }
		        : sendInputOnly
		            ? pix_t{ 30, 15, 5, 255 }
		            : pix_t{ 60, 35, 10, 255 };
		int modeTextW = font->Width(modeLabel);
		print(modeBtnX + (modeBtnW - modeTextW) / 2, btnY + textYOff, modeColour, modeLabel, false);

		// ── LOCK button (right) ──
		fillArea(lockBtnX, btnY, lockBtnW, btnH, 80, 55, 25, 60); // subtle border
		if (headLocked) {
			fillArea(lockBtnX + 1, btnY + 1, lockBtnW - 2, btnH - 2, 100, 70, 20, 80);
		} else if (lockHover) {
			fillArea(lockBtnX + 1, btnY + 1, lockBtnW - 2, btnH - 2, 40, 25, 10, 100);
		} else {
			fillArea(lockBtnX + 1, btnY + 1, lockBtnW - 2, btnH - 2, 60, 40, 20, 30);
		}
		pix_t lockColour = lowOpacity
		    ? pix_t{ 255, 255, 255, 255 }
		    : headLocked
		        ? pix_t{ 20, 10, 0, 255 }
		        : lockHover
		            ? pix_t{ 40, 25, 10, 255 }
		            : pix_t{ 0, 0, 0, 255 };
		int lockTextW = font->Width(L"LOCK");
		print(lockBtnX + (lockBtnW - lockTextW) / 2, btnY + textYOff, lockColour, L"LOCK", false);
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
		    || (key.ch == '\x02' && caseMode == ECaseMode::LOCK);

		bool whiteInk = (s_opacityPercent <= 5); // White text at low opacity for visibility

		// Very subtle 1px faint border around every key
		fillArea(x, y, width, 1, 80, 55, 25, 40);            // top edge
		fillArea(x, y + height - 1, width, 1, 80, 55, 25, 40); // bottom edge
		fillArea(x, y, 1, height, 80, 55, 25, 40);            // left edge
		fillArea(x + width - 1, y, 1, height, 80, 55, 25, 40); // right edge

		// Key interior — mostly parchment showing through
		if (highlighted) {
			// Active shift/caps — warm highlight tint
			fillArea(x + 1, y + 1, width - 2, height - 2, 120, 80, 20, 80);
		}
		// Normal keys: no fill — pure parchment background

		// Controller selection highlights
		bool leftSel = (selected[vr::Eye_Left] == key.id);
		bool rightSel = (selected[vr::Eye_Right] == key.id);

		if (leftSel || rightSel) {
			// Darker highlight for selected key — visible against parchment
			fillArea(x + 1, y + 1, width - 2, height - 2, 60, 35, 10, 100);
		}

		// Text color — BLACK ink on parchment, WHITE when opacity is 1%
		pix_t targetColour;
		if (highlighted) {
			targetColour = { 200, 180, 140, 255 }; // Light text on dark highlighted key
		} else if (leftSel || rightSel) {
			targetColour = { 220, 200, 160, 255 }; // Light text on dark selected key
		} else {
			targetColour = whiteInk ? pix_t{ 255, 255, 255, 255 } : pix_t{ 0, 0, 0, 255 };
		}

		// Check if this is the space bar - draw the space bar image
		bool isSpaceBar = (key.ch == ' ');
		if (isSpaceBar && !s_spaceBarImage.empty()) {
			// Stretch spacebar image to fill entire key box (no padding)
			int padX = 0;
			int padY = 0;
			int drawW = width;
			int drawH = height;
			int drawX = x;
			int drawY = y;

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
								// Use actual image colors, but swap to white at low opacity
								if (whiteInk) {
									// Invert: black scribble becomes white
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
			// Draw arrow triangle centered in key
			int triH = 16;
			int triW = 20;
			int cx = x + width / 2;
			int cy = y + height / 2;

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

			// Check hover/press state for this key
			bool isPressed = (s_pressedKey[0] == key.id || s_pressedKey[1] == key.id);
			bool isHovered = (leftSel || rightSel) && !isPressed;

			// Calculate vertical offset: hover = up 2px, pressed = down 1px
			int yOffset = 0;
			if (isPressed) {
				yOffset = 2; // Push down when pressed
			} else if (isHovered) {
				yOffset = -2; // Pop up when hovering
			}

			// For single-character keys, center the character in the key box
			// For multi-character labels (shift, caps, etc.), use baseline-aligned text
			if (label.length() == 1) {
				// Draw shadow beneath character when hovering (not when pressed)
				if (isHovered) {
					pix_t shadowCol = { 0, 0, 0, 80 }; // Subtle shadow
					SudoFontMeta::pix_t sc = { shadowCol.r, shadowCol.g, shadowCol.b, shadowCol.a };
					font->BlitCentered(label[0], x, y + 2, width, height, desc.Width, sc, (SudoFontMeta::pix_t*)pixels);
				}

				// Draw main character with vertical offset
				SudoFontMeta::pix_t c = { targetColour.r, targetColour.g, targetColour.b, targetColour.a };
				font->BlitCentered(label[0], x, y + yOffset, width, height, desc.Width, c, (SudoFontMeta::pix_t*)pixels);
			} else {
				// Multi-character labels use print
				int textWidth = font->Width(label);

				// Draw shadow for multi-char labels on hover
				if (isHovered) {
					pix_t shadowCol = { 0, 0, 0, 80 };
					print(x + (width - textWidth) / 2, y + padding + 2, shadowCol, label, false);
				}

				// Draw main text with offset
				print(x + (width - textWidth) / 2, y + padding + yOffset, targetColour, label, false);
			}
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
	// BLACK by default, WHITE when opacity is 1% (for visibility on dark backgrounds)
	bool useWhiteInk = (s_opacityPercent <= 5);
	pix_t inkCol = useWhiteInk ? pix_t{ 255, 255, 255, 255 } : pix_t{ 0, 0, 0, 255 };
	pix_t hoverCol = useWhiteInk ? pix_t{ 200, 200, 200, 255 } : pix_t{ 40, 40, 40, 255 };
	int arrowH = 18; // triangle height (compact)
	int arrowW = 22; // triangle base width (compact)
	int fontH = (int)font->GetLineHeight();

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
					p.r = col.r; p.g = col.g; p.b = col.b;
					if (p.a < 200) p.a = 200;
				}
			}
		}
	};

	// ── Opacity & Tilt controls in RIGHT margin ──
	{
		int ctrlX = (int)desc.Width - marginH + 10;
		int ctrlW = marginH - 20;
		int centerX = ctrlX + ctrlW / 2;

		// ── OPACITY section ── (top, just below grab bar)
		int opacTopY = GRAB_BAR_HEIGHT + 8;

		bool opacUpHover = (laserOnOpacityUp[0] || laserOnOpacityUp[1]);
		bool opacDownHover = (laserOnOpacityDown[0] || laserOnOpacityDown[1]);

		drawTriangle(centerX, opacTopY, arrowH, arrowW, true, opacUpHover ? hoverCol : inkCol);
		if (opacUpHover)
			fillArea(centerX - arrowW / 2 - 3, opacTopY - 3, arrowW + 6, arrowH + 6, 40, 25, 10, 80);

		int opacLabelY = opacTopY + arrowH + 4;
		wchar_t opacBuf[32];
		swprintf(opacBuf, 32, L"opac");
		int ow = font->Width(opacBuf);
		print(centerX - ow / 2, opacLabelY, inkCol, opacBuf, false);

		int opacValY = opacLabelY + fontH + 1;
		swprintf(opacBuf, 32, L"%d%%", s_opacityPercent);
		ow = font->Width(opacBuf);
		print(centerX - ow / 2, opacValY, inkCol, opacBuf, false);

		int opacDownY = opacValY + fontH + 4;
		drawTriangle(centerX, opacDownY, arrowH, arrowW, false, opacDownHover ? hoverCol : inkCol);
		if (opacDownHover)
			fillArea(centerX - arrowW / 2 - 3, opacDownY - 3, arrowW + 6, arrowH + 6, 40, 25, 10, 80);

		// ── TILT section ── (below opacity, double separation)
		int tiltTopY = opacDownY + arrowH + 100;

		bool tiltUpHover = (laserOnTiltUp[0] || laserOnTiltUp[1]);
		bool tiltDownHover = (laserOnTiltDown[0] || laserOnTiltDown[1]);

		drawTriangle(centerX, tiltTopY, arrowH, arrowW, true, tiltUpHover ? hoverCol : inkCol);
		if (tiltUpHover)
			fillArea(centerX - arrowW / 2 - 3, tiltTopY - 3, arrowW + 6, arrowH + 6, 40, 25, 10, 80);

		int tiltLabelY = tiltTopY + arrowH + 4;
		wchar_t tiltBuf[32];
		swprintf(tiltBuf, 32, L"tilt");
		int tw = font->Width(tiltBuf);
		print(centerX - tw / 2, tiltLabelY, inkCol, tiltBuf, false);

		int tiltValY = tiltLabelY + fontH + 1;
		swprintf(tiltBuf, 32, L"%.0f", s_tiltDegrees);
		tw = font->Width(tiltBuf);
		print(centerX - tw / 2, tiltValY, inkCol, tiltBuf, false);

		int tiltDownY = tiltValY + fontH + 4;
		drawTriangle(centerX, tiltDownY, arrowH, arrowW, false, tiltDownHover ? hoverCol : inkCol);
		if (tiltDownHover)
			fillArea(centerX - arrowW / 2 - 3, tiltDownY - 3, arrowW + 6, arrowH + 6, 40, 25, 10, 80);
	}

	// ── Size control in LEFT margin ── (moved to right a bit)
	{
		int ctrlX = 35;
		int ctrlW = marginH - 20;
		int centerX = ctrlX + ctrlW / 2;

		// Vertically center the size section below grab bar
		int sectionH = arrowH + 4 + fontH + 1 + fontH + 4 + arrowH;
		int availH = (int)desc.Height - GRAB_BAR_HEIGHT;
		int sizeTopY = GRAB_BAR_HEIGHT + (availH - sectionH) / 2;

		bool sizeUpHover = (laserOnSizeUp[0] || laserOnSizeUp[1]);
		bool sizeDownHover = (laserOnSizeDown[0] || laserOnSizeDown[1]);

		drawTriangle(centerX, sizeTopY, arrowH, arrowW, true, sizeUpHover ? hoverCol : inkCol);
		if (sizeUpHover)
			fillArea(centerX - arrowW / 2 - 3, sizeTopY - 3, arrowW + 6, arrowH + 6, 40, 25, 10, 80);

		int sizeLabelY = sizeTopY + arrowH + 4;
		wchar_t sizeBuf[32];
		swprintf(sizeBuf, 32, L"size");
		int sw = font->Width(sizeBuf);
		print(centerX - sw / 2, sizeLabelY, inkCol, sizeBuf, false);

		int sizeValY = sizeLabelY + fontH + 1;
		swprintf(sizeBuf, 32, L"%d%%", s_scalePercent);
		sw = font->Width(sizeBuf);
		print(centerX - sw / 2, sizeValY, inkCol, sizeBuf, false);

		int sizeDownY = sizeValY + fontH + 4;
		drawTriangle(centerX, sizeDownY, arrowH, arrowW, false, sizeDownHover ? hoverCol : inkCol);
		if (sizeDownHover)
			fillArea(centerX - arrowW / 2 - 3, sizeDownY - 3, arrowW + 6, arrowH + 6, 40, 25, 10, 80);
	}

	if (!minimal) {
		// Text input bar — subtle border on parchment (shifted below grab bar)
		int textBarY = GRAB_BAR_HEIGHT + marginTop;
		int textBarW = (int)desc.Width - 2 * marginH;
		// Faint 1px border
		fillArea(marginH, textBarY, textBarW, 1, 80, 55, 25, 100);
		fillArea(marginH, textBarY + keySize - 1, textBarW, 1, 80, 55, 25, 100);
		fillArea(marginH, textBarY, 1, keySize, 80, 55, 25, 100);
		fillArea(marginH + textBarW - 1, textBarY, 1, keySize, 80, 55, 25, 100);

		if (!sendInputOnly) {
			// Show typed text with blinking cursor (game-opened keyboard only)
			// BLACK ink, WHITE when opacity is 1%
			pix_t targetColour = useWhiteInk ? pix_t{ 255, 255, 255, 255 } : pix_t{ 0, 0, 0, 255 };
			print(marginH + BORD + 6, textBarY + BORD + 4, targetColour, text);

			// Blinking text cursor
			bool cursorVisible = ((GetTickCount64() / 500) % 2) == 0;
			if (cursorVisible) {
				int cursorX = marginH + BORD + 6;
				for (int i = 0; i < cursorPos && i < (int)text.size(); i++)
					cursorX += font->Width(text[i]);
				int cursorY = textBarY + BORD + 2;
				int cursorH = keySize - BORD * 2 - 4;
				if (useWhiteInk)
					fillArea(cursorX, cursorY, 2, cursorH, 255, 255, 255, 255);
				else
					fillArea(cursorX, cursorY, 2, cursorH, 0, 0, 0, 255);
			}
		}
	}

	// Draw laser cursor dots on the keyboard surface
	for (int side = 0; side < 2; side++) {
		if (!laserActive[side])
			continue;

		int cx = (int)(laserU[side] * desc.Width);
		int cy = (int)((1.0f - laserV[side]) * desc.Height);
		int radius = 6;

		// White cursor dot
		int cr = 255, cg = 255, cb = 255;

		// Filled circle
		for (int dy = -radius; dy <= radius; dy++) {
			for (int dx = -radius; dx <= radius; dx++) {
				if (dx * dx + dy * dy <= radius * radius) {
					int px = cx + dx, py = cy + dy;
					if (px >= 0 && px < (int)desc.Width && py >= 0 && py < (int)desc.Height) {
						pix_t& p = pixels[px + py * desc.Width];
						p.r = cr; p.g = cg; p.b = cb; p.a = 255;
					}
				}
			}
		}
	}

	// Create a staging texture with the CPU-rendered pixels
	D3D11_SUBRESOURCE_DATA init[] = {
		{ pixels, sizeof(pix_t) * desc.Width, sizeof(pix_t) * desc.Width * desc.Height }
	};

	CComPtr<ID3D11Texture2D> tex;
	HRESULT rres = dev->CreateTexture2D(&desc, init, &tex);
	OOVR_FAILED_DX_ABORT(rres);
	delete[] pixels;

	// Acquire an image from the OpenXR swap chain
	XrSwapchainImageAcquireInfo acquireInfo = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	uint32_t currentIndex = 0;
	OOVR_FAILED_XR_ABORT(xrAcquireSwapchainImage(chain, &acquireInfo, &currentIndex));

	// Wait for the image to be ready
	XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	waitInfo.timeout = 500000000; // 500ms
	OOVR_FAILED_XR_ABORT(xrWaitSwapchainImage(chain, &waitInfo));

	// Copy the staging texture to the swap chain image
	ctx->CopyResource(swapchainImages[currentIndex].texture, tex);

	// Release the swap chain image
	XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	OOVR_FAILED_XR_ABORT(xrReleaseSwapchainImage(chain, &releaseInfo));
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

	pix_t* pixels = new pix_t[desc.Width * desc.Height];

	const int BORD = 2;
	const int PAD = 8;

	// Fill background — parchment-like warm tan, semi-transparent
	for (UINT y = 0; y < desc.Height; y++) {
		for (UINT x = 0; x < desc.Width; x++) {
			pix_t& p = pixels[x + y * desc.Width];
			p.r = 220; p.g = 195; p.b = 160; p.a = 200;
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
			}
		}
	};

	auto printLine = [&](int x, int y, pix_t colour, const std::wstring& txt) {
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

	// Dark brown border (matches parchment keyboard frame)
	fillArea(0, 0, desc.Width, BORD, 60, 40, 20, 220);                    // top
	fillArea(0, desc.Height - BORD, desc.Width, BORD, 60, 40, 20, 220);   // bottom
	fillArea(0, 0, BORD, desc.Height, 60, 40, 20, 220);                   // left
	fillArea(desc.Width - BORD, 0, BORD, desc.Height, 60, 40, 20, 220);   // right

	// Title bar — "INPUT" centered, doubled height so text fits cleanly
	int titleH = 60;
	fillArea(BORD, BORD, desc.Width - BORD * 2, titleH, 80, 55, 25, 100);  // subtle darker strip
	fillArea(BORD, BORD + titleH, desc.Width - BORD * 2, 2, 60, 40, 20, 220); // dark separator

	pix_t titleColour = { 30, 15, 5, 255 }; // black ink
	int titleTextW = font->Width(L"INPUT");
	int fontH = (int)font->GetLineHeight();
	int titleTextY = BORD + (titleH - fontH) / 2;
	printLine((int)desc.Width / 2 - titleTextW / 2, titleTextY, titleColour, L"INPUT");

	// Input text with blinking cursor — centered vertically in remaining space
	int contentTop = BORD + titleH + 2;
	int contentH = (int)desc.Height - contentTop - BORD;
	int inputY = contentTop + (contentH - fontH) / 2;
	pix_t textColour = { 30, 15, 5, 255 }; // black ink

	printLine(PAD + BORD, inputY, textColour, text);

	// Blinking cursor at end of text
	bool cursorVisible = ((GetTickCount64() / 500) % 2) == 0;
	if (cursorVisible) {
		int cursorX = PAD + BORD;
		for (size_t i = 0; i < text.size(); i++)
			cursorX += font->Width(text[i]);
		fillArea(cursorX, inputY, 2, fontH, 30, 15, 5, 255);
	}

	// Copy to console swapchain
	D3D11_SUBRESOURCE_DATA init = { pixels, sizeof(pix_t) * desc.Width, sizeof(pix_t) * desc.Width * desc.Height };
	CComPtr<ID3D11Texture2D> tex;
	HRESULT hr = dev->CreateTexture2D(&desc, &init, &tex);
	delete[] pixels;
	if (FAILED(hr))
		return;

	XrSwapchainImageAcquireInfo acq = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	uint32_t idx = 0;
	OOVR_FAILED_XR_ABORT(xrAcquireSwapchainImage(consoleChain, &acq, &idx));
	XrSwapchainImageWaitInfo wait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	wait.timeout = 500000000;
	OOVR_FAILED_XR_ABORT(xrWaitSwapchainImage(consoleChain, &wait));
	ctx->CopyResource(consoleSwapImages[idx].texture, tex);
	XrSwapchainImageReleaseInfo rel = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	OOVR_FAILED_XR_ABORT(xrReleaseSwapchainImage(consoleChain, &rel));
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
	sendInputOnly = enabled;
	// sendInputMode is always true — keyboard always uses SendInput
	dirty = true;
}
