#pragma once

#include <d3d11.h>
#include <vector>

#include "Misc/xr_ext.h"
#include "Misc/xrutil.h"

// Renders laser pointers from VR controllers to a virtual menu quad.
// Used for interacting with MCM/pause menus in Skyrim VR.
// Visual style matches VRKeyboard lasers (warm white, 3mm beams, billboard-oriented).
class VRMenuLaser {
public:
	VRMenuLaser(ID3D11Device* dev);
	~VRMenuLaser();

	// Define the menu quad plane in floor space
	void SetMenuQuad(XrPosef pose, XrExtent2Df size);

	// Toggle debug quad visibility and set its opacity (0-100)
	void SetShowDebugQuad(bool show) { showDebugQuad = show; }
	void SetDebugQuadOpacity(int percent) { debugOpacityPercent = percent; }

	// Set debug quad fill color (default: dark green 30,100,30)
	void SetDebugQuadColor(int r, int g, int b) {
		if (r != debugColorR || g != debugColorG || b != debugColorB) {
			debugColorR = r; debugColorG = g; debugColorB = b;
			debugLastBakedOpacity = -1; // force rebake
		}
	}

	// Per-frame update. Returns composition layers to submit.
	// keyboardHitSide[i] = true means keyboard already claimed that hand's laser.
	const std::vector<XrCompositionLayerBaseHeader*>& Update(
	    XrTime predictedTime,
	    const bool keyboardHitSide[2]);

	// Query results after Update()
	bool IsHit(int side) const { return hitActive[side]; }
	float GetHitU(int side) const { return hitU[side]; }
	float GetHitV(int side) const { return hitV[side]; }
	bool IsTriggerPressed(int side) const { return triggerState[side] && !triggerLast[side]; }
	bool IsTriggerReleased(int side) const { return !triggerState[side] && triggerLast[side]; }
	bool IsThumbstickPressed(int side) const { return thumbstickState[side] && !thumbstickLast[side]; }
	bool IsXButtonPressed(int side) const { return xBtnState[side] && !xBtnLast[side]; }
	bool IsAppMenuPressed(int side) const { return appMenuState[side] && !appMenuLast[side]; }
	bool IsAppMenuReleased(int side) const { return !appMenuState[side] && appMenuLast[side]; }

	// Ray data for calibration/diagnostics
	XrVector3f GetRayOrigin(int side) const { return lastRayOrigin[side]; }
	XrVector3f GetRayDir(int side) const { return lastRayDir[side]; }
	float GetHitT(int side) const { return lastHitT[side]; }

	// Thumbstick axis values (for in-VR quad adjustment)
	float GetThumbstickX(int side) const { return thumbstickX[side]; }
	float GetThumbstickY(int side) const { return thumbstickY[side]; }

private:
	ID3D11Device* dev;
	ID3D11DeviceContext* ctx = nullptr;

	// Beam layers (one per hand)
	XrSwapchain beamChain[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE };
	XrCompositionLayerQuad beamLayer[2] = {};

	// Cursor dot layers (one per hand)
	XrSwapchain dotChain[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE };
	XrCompositionLayerQuad dotLayer[2] = {};

	// Debug quad overlay — semi-transparent rectangle showing the menu hit area
	XrSwapchain debugQuadChain = XR_NULL_HANDLE;
	XrCompositionLayerQuad debugQuadLayer = {};
	std::vector<XrSwapchainImageD3D11KHR> debugSwapImages;
	int debugLastBakedOpacity = -1; // track to avoid redundant re-uploads
	void RebakeDebugQuadTexture(int opacityPercent);

	// Menu quad definition
	XrPosef menuPose = { { 0, 0, 0, 1 }, { 0, 0, 0 } };
	XrExtent2Df menuSize = { 0, 0 };
	bool menuValid = false;
	bool showDebugQuad = true;
	int debugOpacityPercent = 20;
	int debugColorR = 30, debugColorG = 100, debugColorB = 30; // fill color (default green)

	// Per-hand hit state
	bool hitActive[2] = {};
	float hitU[2] = {}, hitV[2] = {};

	// Trigger tracking
	bool triggerState[2] = {}, triggerLast[2] = {};

	// Thumbstick click tracking (for calibration)
	bool thumbstickState[2] = {}, thumbstickLast[2] = {};

	// X/A button tracking (for calibration depth probes)
	bool xBtnState[2] = {}, xBtnLast[2] = {};

	// ApplicationMenu button tracking (Y on left, B on right — used for tab cycling)
	bool appMenuState[2] = {}, appMenuLast[2] = {};

	// Thumbstick axis values (updated each frame)
	float thumbstickX[2] = {}, thumbstickY[2] = {};

	// Per-hand ray data from last Update() — for calibration logging
	XrVector3f lastRayOrigin[2] = {};
	XrVector3f lastRayDir[2] = {};
	float lastHitT[2] = {};

	// Returned each frame
	std::vector<XrCompositionLayerBaseHeader*> activeLayers;

	// Ray-plane intersection against menu quad. Returns true if hit, fills u/v/t.
	bool RayIntersectQuad(const XrVector3f& origin, const XrVector3f& dir,
	    float& u, float& v, float& t);

	// Position the beam composition layer from origin toward hitPoint
	void UpdateBeam(int side, const XrVector3f& origin, const XrVector3f& dir,
	    float t, const XrVector3f& headPos);

	// Position cursor dot at the hit point on the menu surface
	void UpdateDot(int side, const XrVector3f& hitPoint);

	// Billboard orientation — Y-axis along beam, facing viewer
	static XrQuaternionf BeamOrientation(const XrVector3f& dir,
	    const XrVector3f& mid, const XrVector3f& viewerPos);
};
