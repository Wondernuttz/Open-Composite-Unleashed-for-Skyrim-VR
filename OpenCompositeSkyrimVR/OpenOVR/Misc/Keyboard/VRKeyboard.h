#pragma once

#include <d3d11.h>

#include <codecvt>
#include <functional>
#include <locale>
#include <memory>
#include <string>
#include <vector>

#include "Misc/xr_ext.h"
#include "Misc/xrutil.h"

#include "KeyboardLayout.h"
#include "SudoFontMeta.h"

struct KbThemeDef; // Selectable keyboard theme (palette + font + background), table in VRKeyboard.cpp
class LaserTextureAtlas;

class VRKeyboard {
public:
	typedef std::function<void(vr::VREvent_t)> eventDispatch_t;

	// Yay this isn't in vrtypes.h
	// Maybe we should make the header splitter put enums somewhere else?
	enum EGamepadTextInputMode {
		k_EGamepadTextInputModeNormal = 0,
		k_EGamepadTextInputModePassword = 1,
		k_EGamepadTextInputModeSubmit = 2,
	};

	VRKeyboard(ID3D11Device* dev, uint64_t userValue, uint32_t maxLength, bool minimal, eventDispatch_t dispatch, EGamepadTextInputMode inputMode);
	~VRKeyboard();

	std::wstring contents();
	void contents(std::wstring);

	const std::vector<XrCompositionLayerBaseHeader*>& Update();

	void HandleOverlayInput(vr::EVREye controllerDeviceIndex, vr::VRControllerState_t state, float time);

	enum ECaseMode {
		LOWER,
		SHIFT,
		LOCK,
	};

	static std::wstring_convert<std::codecvt_utf8<wchar_t>> CHAR_CONV;

	bool IsClosed() { return closed; }
	bool IsSendInputMode() const { return sendInputMode; }
	void SetSendInputOnly(bool enabled);

	// Whether the laser for this hand is currently hitting the keyboard quad
	bool IsLaserOnKeyboard(int side) const { return laserActive[side]; }

	void SetTransform(vr::HmdMatrix34_t transform);

private:
	ID3D11Device* const dev;
	ID3D11DeviceContext* ctx;

	bool dirty = true;
	bool closed = false;

	std::wstring text;
	int cursorPos = 0; // insertion point: 0 = before first char, text.size() = after last
	ECaseMode caseMode = LOWER;

	uint64_t userValue; // Arbitary user data, to be passed into the SteamVR events
	uint32_t maxLength;
	bool minimal;
	eventDispatch_t eventDispatch;
	EGamepadTextInputMode inputMode;

	// OpenXR swap chain and composition layer
	XrSwapchain chain = XR_NULL_HANDLE;
	std::vector<XrSwapchainImageD3D11KHR> swapchainImages;
	uint32_t texWidth = 1024;
	uint32_t texHeight = 560; // Increased from 478 to accommodate F-key row
	XrCompositionLayerQuad layer = { XR_TYPE_COMPOSITION_LAYER_QUAD };

	std::unique_ptr<SudoFontMeta> font;
	std::unique_ptr<KeyboardLayout> layout;

	// Active theme (font + background + palette); reloaded when the ini theme changes
	const KbThemeDef* theme = nullptr;
	std::string loadedThemeName;
	std::string loadedFontName;
	std::string loadedLayoutName;

	// Theme background texture (pre-scaled to texWidth x texHeight, RGBA)
	std::vector<uint8_t> parchmentBg;
	unsigned int parchmentW = 0, parchmentH = 0;
	struct DecodedKeyboardArtwork {
		std::vector<uint8_t> pixels;
		unsigned int width = 0;
		unsigned int height = 0;
		std::vector<uint8_t> glowPixels;
		unsigned int glowWidth = 0;
		unsigned int glowHeight = 0;
		int glowX = 0;
		int glowY = 0;
		KeyboardLayout::ImageLayer placement;
		bool prepared = false;
	};
	DecodedKeyboardArtwork customKeyboardBg;
	std::vector<DecodedKeyboardArtwork> customKeyboardSprites;
	DecodedKeyboardArtwork customControlArrow;
	DecodedKeyboardArtwork customConsoleInputBg;
	DecodedKeyboardArtwork customModeVrArtwork;
	DecodedKeyboardArtwork customModePcArtwork;
	DecodedKeyboardArtwork customLockWorldArtwork;
	DecodedKeyboardArtwork customLockHeadArtwork;
	// Reused CPU surfaces. Breathing effects redraw while the keyboard is open;
	// retaining these buffers avoids allocating several megabytes every tick.
	std::vector<uint32_t> keyboardRenderBuffer;
	std::vector<uint32_t> consoleRenderBuffer;
	uint64_t lastAnimationRefreshMs = 0;
	uint64_t lastSlowRefreshLogMs = 0;
	struct PlateGlowMask {
		int plateWidth = 0;
		int plateHeight = 0;
		int plateRadius = 0;
		int glowRadius = 0;
		int width = 0;
		int height = 0;
		std::vector<uint8_t> coverage;
	};
	// Geometry is shared by almost every key. Cache the expanded rounded glow
	// band instead of rebuilding it ring-by-ring on every breathing frame.
	std::vector<PlateGlowMask> plateGlowMasks;

	// These use the OpenVR eye constants
	float lastInputTime[2] = { 0, 0 };
	int repeatCount[2] = { 0, 0 };
	int selected[2] = { -1, -1 };
	uint64_t lastButtonState[2] = { 0, 0 };

	// SendInput mode — injects Windows keystrokes instead of buffering text
	bool sendInputMode = true;  // Always SendInput — keyboard tied to Windows keyboard
	bool sendInputOnly = false; // true when opened via controller shortcut (no text buffer)

	// PC MODE keys stay down until the same controller's trigger is released.
	// Ctrl is a sticky, one-shot modifier: tap Ctrl, tap the shortcut key, then
	// both keys release in the correct order. Tapping Ctrl again cancels it.
	struct HeldPCKey {
		uint16_t vk = 0;
		int keyId = -1;
		bool shift = false;
		bool scanOnly = false;
	};
	HeldPCKey heldPCKeys[2];
	bool ctrlLatched = false;
	bool ctrlSentToPC = false;
	int ctrlLatchSide = -1;
	bool releaseCtrlAfterHeldPCKey[2] = { false, false };
	void PressHeldPCKey(int side, int keyId, uint16_t vk, bool shift, bool scanOnly);
	void PressControlKey(int side, int keyId, uint16_t vk);
	void ReleaseHeldPCKey(int side);
	void ToggleCtrlLatch(int side);
	void ReleaseCtrlLatch();
	void PasteConsoleClipboard();
	bool HandleConsoleShortcut(wchar_t ch);
	void ReleaseAllHeldPCKeys();

	// Grab bar — trigger on the top strip to grab and reposition the keyboard
	static constexpr int GRAB_BAR_HEIGHT = 52; // pixels at top of texture
	static constexpr int TOGGLE_BTN_WIDTH = 120; // headlock toggle button width
	static constexpr int CONSOLE_BTN_WIDTH = 220; // console button width
	bool grabActive = false;
	int grabbingSide = -1;
	XrVector3f grabOffset = {};      // offset from laser hit to keyboard center
	XrVector3f grabPlaneOrigin = {}; // keyboard center when grab started (reference plane)
	bool lastTriggerState[2] = { false, false };
	bool laserOnGrabBar[2] = { false, false };
	bool laserOnToggle[2] = { false, false };
	bool laserOnConsole[2] = { false, false };
	bool laserOnTextBar[2] = { false, false };
	bool laserOnTiltUp[2] = { false, false };
	bool laserOnTiltDown[2] = { false, false };
	bool laserOnOpacityUp[2] = { false, false };
	bool laserOnOpacityDown[2] = { false, false };
	bool laserOnSizeUp[2] = { false, false };
	bool laserOnSizeDown[2] = { false, false };
	bool headLocked = false; // true = head-locked (viewSpace), false = world-anchored (floorSpace)
	XrVector3f headWorldPos = {}; // Head position in world/view space, updated each frame

	// Menu, console, and keyboard laser sprites share one immutable atlas.
	std::shared_ptr<LaserTextureAtlas> laserAtlas;

	// Target dot composition layers (controller 0, controller 1, headset).
	XrCompositionLayerQuad targetDotLayer[3] = {};
	// Live keyboard cursors use their own quads backed by the same tiny dot
	// texture. Cursor motion must not dirty and upload the 1024x560 keyboard.
	XrCompositionLayerQuad cursorDotLayer[2] = {};

	// Laser pointer data
	bool laserActive[2] = { false, false };
	float laserU[2] = {};
	float laserV[2] = {};
	XrVector3f laserOrigin[2] = {};
	XrVector3f laserHitPoint[2] = {};

	// Laser beam composition layers (one per hand, atlas-selected idle/clicked).
	XrCompositionLayerQuad laserLayer[2] = {};

	// All layers returned by Update (keyboard + laser beams)
	std::vector<XrCompositionLayerBaseHeader*> activeLayers;

	// Crosshair dot overlay (toggled by [M] key, visible at view center)
	bool crosshairVisible = false;
	XrSwapchain crosshairChain = XR_NULL_HANDLE;
	XrCompositionLayerQuad crosshairLayer = { XR_TYPE_COMPOSITION_LAYER_QUAD };

	// Console INPUT overlay — floating panel above keyboard showing typed text
	bool consoleActive = false;
	bool consoleDirty = true;
	std::wstring consoleStatus;
	XrSwapchain consoleChain = XR_NULL_HANDLE;
	std::vector<XrSwapchainImageD3D11KHR> consoleSwapImages;
	static constexpr uint32_t consoleTexWidth = 1024;
	static constexpr uint32_t consoleTexHeight = 120;
	XrCompositionLayerQuad consoleLayer = { XR_TYPE_COMPOSITION_LAYER_QUAD };

	void LoadThemeAssets();
	void LoadKeyboardLayout();
	void LoadKeyboardArtwork();
	void Refresh();
	void RefreshConsole();

	void SubmitEvent(vr::EVREventType ev, wchar_t ch);

	int HitTestLaser(int side);
	void UpdateLaserBeam(int side);
};
