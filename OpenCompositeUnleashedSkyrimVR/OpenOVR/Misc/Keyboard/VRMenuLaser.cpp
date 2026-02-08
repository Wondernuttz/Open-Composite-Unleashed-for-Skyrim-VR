#include "stdafx.h"

#include "VRMenuLaser.h"

#include <d3d11.h>
#include <cmath>

#include "Reimpl/BaseInput.h"
#include "Reimpl/BaseSystem.h"
#include "generated/static_bases.gen.h"

#ifdef _WIN32
#pragma comment(lib, "d3d11.lib")
#endif

// ── Math helpers (same as VRKeyboard) ──

static inline float ml_dot(const XrVector3f& a, const XrVector3f& b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

// ── VRMenuLaser implementation ──

VRMenuLaser::VRMenuLaser(ID3D11Device* dev)
    : dev(dev)
{
	dev->GetImmediateContext(&ctx);

	// Create beam swapchains — identical to VRKeyboard laser swapchains
	for (int i = 0; i < 2; i++) {
		XrSwapchainCreateInfo sci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
		sci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
		sci.format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		sci.sampleCount = 1;
		sci.width = 4;
		sci.height = 4;
		sci.faceCount = 1;
		sci.arraySize = 1;
		sci.mipCount = 1;

		OOVR_FAILED_XR_ABORT(xrCreateSwapchain(xr_session.get(), &sci, &beamChain[i]));

		uint32_t imgCount = 0;
		OOVR_FAILED_XR_ABORT(xrEnumerateSwapchainImages(beamChain[i], 0, &imgCount, nullptr));
		std::vector<XrSwapchainImageD3D11KHR> imgs(imgCount, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
		OOVR_FAILED_XR_ABORT(xrEnumerateSwapchainImages(beamChain[i], imgCount, &imgCount,
		    (XrSwapchainImageBaseHeader*)imgs.data()));

		// Warm white beam — RGB(255,240,220) alpha 180
		uint8_t cr = 255, cg = 240, cb = 220, ca = 180;
		uint32_t packed = cr | (cg << 8) | (cb << 16) | (ca << 24);
		uint32_t colorPixels[16];
		for (int j = 0; j < 16; j++)
			colorPixels[j] = packed;

		D3D11_TEXTURE2D_DESC td = {};
		td.Width = 4;
		td.Height = 4;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		td.SampleDesc = { 1, 0 };
		td.Usage = D3D11_USAGE_DEFAULT;

		D3D11_SUBRESOURCE_DATA init = { colorPixels, sizeof(uint32_t) * 4, sizeof(uint32_t) * 16 };
		CComPtr<ID3D11Texture2D> tex;
		OOVR_FAILED_DX_ABORT(dev->CreateTexture2D(&td, &init, &tex));

		XrSwapchainImageAcquireInfo acq = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
		uint32_t idx = 0;
		OOVR_FAILED_XR_ABORT(xrAcquireSwapchainImage(beamChain[i], &acq, &idx));
		XrSwapchainImageWaitInfo wait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
		wait.timeout = 500000000;
		OOVR_FAILED_XR_ABORT(xrWaitSwapchainImage(beamChain[i], &wait));
		ctx->CopyResource(imgs[idx].texture, tex);
		XrSwapchainImageReleaseInfo rel = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		OOVR_FAILED_XR_ABORT(xrReleaseSwapchainImage(beamChain[i], &rel));

		memset(&beamLayer[i], 0, sizeof(beamLayer[i]));
		beamLayer[i].type = XR_TYPE_COMPOSITION_LAYER_QUAD;
		beamLayer[i].layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		beamLayer[i].space = xr_gbl->floorSpace;
		beamLayer[i].eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		beamLayer[i].subImage.swapchain = beamChain[i];
		beamLayer[i].subImage.imageRect.offset = { 0, 0 };
		beamLayer[i].subImage.imageRect.extent = { 4, 4 };
		beamLayer[i].subImage.imageArrayIndex = 0;
	}

	// Create cursor dot swapchains — same color, full alpha, 8x8 pixels
	for (int i = 0; i < 2; i++) {
		XrSwapchainCreateInfo sci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
		sci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
		sci.format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		sci.sampleCount = 1;
		sci.width = 8;
		sci.height = 8;
		sci.faceCount = 1;
		sci.arraySize = 1;
		sci.mipCount = 1;

		OOVR_FAILED_XR_ABORT(xrCreateSwapchain(xr_session.get(), &sci, &dotChain[i]));

		uint32_t imgCount = 0;
		OOVR_FAILED_XR_ABORT(xrEnumerateSwapchainImages(dotChain[i], 0, &imgCount, nullptr));
		std::vector<XrSwapchainImageD3D11KHR> imgs(imgCount, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
		OOVR_FAILED_XR_ABORT(xrEnumerateSwapchainImages(dotChain[i], imgCount, &imgCount,
		    (XrSwapchainImageBaseHeader*)imgs.data()));

		// Warm white dot — full alpha for visibility
		uint8_t cr = 255, cg = 240, cb = 220, ca = 255;
		uint32_t packed = cr | (cg << 8) | (cb << 16) | (ca << 24);
		uint32_t dotPixels[64];
		// Create a circle mask in the 8x8 texture
		for (int py = 0; py < 8; py++) {
			for (int px = 0; px < 8; px++) {
				float dx = px - 3.5f, dy = py - 3.5f;
				dotPixels[py * 8 + px] = (dx * dx + dy * dy <= 12.25f) ? packed : 0;
			}
		}

		D3D11_TEXTURE2D_DESC td = {};
		td.Width = 8;
		td.Height = 8;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
		td.SampleDesc = { 1, 0 };
		td.Usage = D3D11_USAGE_DEFAULT;

		D3D11_SUBRESOURCE_DATA init = { dotPixels, sizeof(uint32_t) * 8, sizeof(uint32_t) * 64 };
		CComPtr<ID3D11Texture2D> tex;
		OOVR_FAILED_DX_ABORT(dev->CreateTexture2D(&td, &init, &tex));

		XrSwapchainImageAcquireInfo acq = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
		uint32_t idx = 0;
		OOVR_FAILED_XR_ABORT(xrAcquireSwapchainImage(dotChain[i], &acq, &idx));
		XrSwapchainImageWaitInfo wait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
		wait.timeout = 500000000;
		OOVR_FAILED_XR_ABORT(xrWaitSwapchainImage(dotChain[i], &wait));
		ctx->CopyResource(imgs[idx].texture, tex);
		XrSwapchainImageReleaseInfo rel = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		OOVR_FAILED_XR_ABORT(xrReleaseSwapchainImage(dotChain[i], &rel));

		memset(&dotLayer[i], 0, sizeof(dotLayer[i]));
		dotLayer[i].type = XR_TYPE_COMPOSITION_LAYER_QUAD;
		dotLayer[i].layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		dotLayer[i].space = xr_gbl->floorSpace;
		dotLayer[i].eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		dotLayer[i].subImage.swapchain = dotChain[i];
		dotLayer[i].subImage.imageRect.offset = { 0, 0 };
		dotLayer[i].subImage.imageRect.extent = { 8, 8 };
		dotLayer[i].subImage.imageArrayIndex = 0;
	}

	// Create debug quad overlay — semi-transparent green rectangle showing the menu hit area
	// Use linear UNORM (not SRGB) so raw alpha values map 1:1 to transparency —
	// sRGB gamma expansion makes even low-alpha colors appear far too bright.
	{
		XrSwapchainCreateInfo sci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
		sci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
		sci.format = DXGI_FORMAT_R8G8B8A8_UNORM;
		sci.sampleCount = 1;
		sci.width = 16;
		sci.height = 16;
		sci.faceCount = 1;
		sci.arraySize = 1;
		sci.mipCount = 1;

		OOVR_FAILED_XR_ABORT(xrCreateSwapchain(xr_session.get(), &sci, &debugQuadChain));

		uint32_t imgCount = 0;
		OOVR_FAILED_XR_ABORT(xrEnumerateSwapchainImages(debugQuadChain, 0, &imgCount, nullptr));
		debugSwapImages.resize(imgCount, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
		OOVR_FAILED_XR_ABORT(xrEnumerateSwapchainImages(debugQuadChain, imgCount, &imgCount,
		    (XrSwapchainImageBaseHeader*)debugSwapImages.data()));

		// Initial bake at default opacity
		RebakeDebugQuadTexture(20);

		memset(&debugQuadLayer, 0, sizeof(debugQuadLayer));
		debugQuadLayer.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
		debugQuadLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		debugQuadLayer.space = xr_gbl->floorSpace;
		debugQuadLayer.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		debugQuadLayer.subImage.swapchain = debugQuadChain;
		debugQuadLayer.subImage.imageRect.offset = { 0, 0 };
		debugQuadLayer.subImage.imageRect.extent = { 16, 16 };
		debugQuadLayer.subImage.imageArrayIndex = 0;
	}
}

VRMenuLaser::~VRMenuLaser()
{
	for (int i = 0; i < 2; i++) {
		if (beamChain[i] != XR_NULL_HANDLE)
			xrDestroySwapchain(beamChain[i]);
		if (dotChain[i] != XR_NULL_HANDLE)
			xrDestroySwapchain(dotChain[i]);
	}
	if (debugQuadChain != XR_NULL_HANDLE)
		xrDestroySwapchain(debugQuadChain);
	if (ctx)
		ctx->Release();
}

void VRMenuLaser::RebakeDebugQuadTexture(int opacityPercent)
{
	if (debugQuadChain == XR_NULL_HANDLE || debugSwapImages.empty())
		return;
	if (opacityPercent == debugLastBakedOpacity)
		return;
	debugLastBakedOpacity = opacityPercent;

	// Premultiplied alpha with linear UNORM — slider % maps directly to alpha.
	// SRGB→UNORM fix already solved the "too bright" problem, so no need for
	// aggressive curves. Just straight linear mapping.
	float alphaF = opacityPercent / 100.0f;
	uint8_t fillAlpha = (uint8_t)(alphaF * 255.0f);
	float borderAF = std::min(1.0f, alphaF + 0.05f); // border slightly more visible
	uint8_t borderAlpha = (uint8_t)(borderAF * 255.0f);
	// Fill color from SetDebugQuadColor, premultiplied: RGB scaled by alpha
	uint8_t fR = (uint8_t)(debugColorR * alphaF), fG = (uint8_t)(debugColorG * alphaF), fB = (uint8_t)(debugColorB * alphaF);
	// Border: 40% brighter than fill, clamped to 255
	int bri = (int)(debugColorR * 1.4f); if (bri > 255) bri = 255;
	int bgi = (int)(debugColorG * 1.4f); if (bgi > 255) bgi = 255;
	int bbi = (int)(debugColorB * 1.4f); if (bbi > 255) bbi = 255;
	uint8_t bR = (uint8_t)(bri * borderAF), bG = (uint8_t)(bgi * borderAF), bB = (uint8_t)(bbi * borderAF);
	uint32_t fillColor = fR | (fG << 8) | (fB << 16) | ((uint32_t)fillAlpha << 24);
	uint32_t borderColor = bR | (bG << 8) | (bB << 16) | ((uint32_t)borderAlpha << 24);

	uint32_t pixels[16 * 16];
	for (int py = 0; py < 16; py++) {
		for (int px = 0; px < 16; px++) {
			bool isBorder = (px == 0 || px == 15 || py == 0 || py == 15);
			pixels[py * 16 + px] = isBorder ? borderColor : fillColor;
		}
	}

	D3D11_TEXTURE2D_DESC td = {};
	td.Width = 16;
	td.Height = 16;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; // linear — matches swapchain
	td.SampleDesc = { 1, 0 };
	td.Usage = D3D11_USAGE_DEFAULT;

	D3D11_SUBRESOURCE_DATA init = { pixels, sizeof(uint32_t) * 16, sizeof(uint32_t) * 16 * 16 };
	CComPtr<ID3D11Texture2D> tex;
	HRESULT hr = dev->CreateTexture2D(&td, &init, &tex);
	if (FAILED(hr)) return;

	XrSwapchainImageAcquireInfo acq = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	uint32_t idx = 0;
	if (XR_FAILED(xrAcquireSwapchainImage(debugQuadChain, &acq, &idx))) return;
	XrSwapchainImageWaitInfo wait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	wait.timeout = 500000000;
	if (XR_FAILED(xrWaitSwapchainImage(debugQuadChain, &wait))) return;
	ctx->CopyResource(debugSwapImages[idx].texture, tex);
	XrSwapchainImageReleaseInfo rel = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	xrReleaseSwapchainImage(debugQuadChain, &rel);
}

void VRMenuLaser::SetMenuQuad(XrPosef pose, XrExtent2Df size)
{
	menuPose = pose;
	menuSize = size;
	menuValid = true;
}

bool VRMenuLaser::RayIntersectQuad(const XrVector3f& origin, const XrVector3f& dir,
    float& u, float& v, float& t)
{
	if (!menuValid)
		return false;

	XrVector3f planeNormal, localRight, localUp;
	rotate_vector_by_quaternion({ 0, 0, 1 }, menuPose.orientation, planeNormal);
	rotate_vector_by_quaternion({ 1, 0, 0 }, menuPose.orientation, localRight);
	rotate_vector_by_quaternion({ 0, 1, 0 }, menuPose.orientation, localUp);

	float denom = ml_dot(dir, planeNormal);
	if (fabsf(denom) < 1e-6f)
		return false;

	XrVector3f PO = {
		menuPose.position.x - origin.x,
		menuPose.position.y - origin.y,
		menuPose.position.z - origin.z
	};
	t = ml_dot(PO, planeNormal) / denom;
	if (t <= 0.0f)
		return false;

	XrVector3f hitPoint = {
		origin.x + t * dir.x,
		origin.y + t * dir.y,
		origin.z + t * dir.z
	};

	XrVector3f HP = {
		hitPoint.x - menuPose.position.x,
		hitPoint.y - menuPose.position.y,
		hitPoint.z - menuPose.position.z
	};
	float localX = ml_dot(HP, localRight);
	float localY = ml_dot(HP, localUp);

	u = (localX + menuSize.width * 0.5f) / menuSize.width;
	v = (localY + menuSize.height * 0.5f) / menuSize.height;

	if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f)
		return false;

	return true;
}

XrQuaternionf VRMenuLaser::BeamOrientation(const XrVector3f& beamDir,
    const XrVector3f& midpoint, const XrVector3f& viewerPos)
{
	XrVector3f up = beamDir;

	XrVector3f toViewer = {
		viewerPos.x - midpoint.x,
		viewerPos.y - midpoint.y,
		viewerPos.z - midpoint.z
	};
	float ml = sqrtf(toViewer.x * toViewer.x + toViewer.y * toViewer.y + toViewer.z * toViewer.z);
	XrVector3f fwd;
	if (ml > 0.001f)
		fwd = { toViewer.x / ml, toViewer.y / ml, toViewer.z / ml };
	else
		fwd = { 0, 0, 1 };

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
	right.x /= rl;
	right.y /= rl;
	right.z /= rl;

	fwd = {
		right.y * up.z - right.z * up.y,
		right.z * up.x - right.x * up.z,
		right.x * up.y - right.y * up.x
	};

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

void VRMenuLaser::UpdateBeam(int side, const XrVector3f& origin, const XrVector3f& dir,
    float beamLen, const XrVector3f& headPos)
{
	XrVector3f beamEnd = {
		origin.x + dir.x * beamLen,
		origin.y + dir.y * beamLen,
		origin.z + dir.z * beamLen
	};

	XrVector3f mid = {
		(origin.x + beamEnd.x) * 0.5f,
		(origin.y + beamEnd.y) * 0.5f,
		(origin.z + beamEnd.z) * 0.5f
	};

	beamLayer[side].pose.position = mid;
	beamLayer[side].size.width = 0.003f; // 3mm thin
	beamLayer[side].size.height = beamLen;
	beamLayer[side].pose.orientation = BeamOrientation(dir, mid, headPos);
}

void VRMenuLaser::UpdateDot(int side, const XrVector3f& hitPoint)
{
	// Place a small dot at the hit point on the menu surface, slightly in front
	XrVector3f planeNormal;
	rotate_vector_by_quaternion({ 0, 0, 1 }, menuPose.orientation, planeNormal);

	dotLayer[side].pose.position = {
		hitPoint.x + planeNormal.x * 0.001f, // 1mm in front to avoid z-fighting
		hitPoint.y + planeNormal.y * 0.001f,
		hitPoint.z + planeNormal.z * 0.001f
	};
	dotLayer[side].pose.orientation = menuPose.orientation;
	dotLayer[side].size.width = 0.012f;  // 12mm dot
	dotLayer[side].size.height = 0.012f;
}

const std::vector<XrCompositionLayerBaseHeader*>& VRMenuLaser::Update(
    XrTime predictedTime, const bool keyboardHitSide[2])
{
	activeLayers.clear();

	if (!menuValid)
		return activeLayers;

	// Debug quad — show the menu hit area as a semi-transparent overlay
	if (debugQuadChain != XR_NULL_HANDLE && showDebugQuad) {
		RebakeDebugQuadTexture(debugOpacityPercent);
		debugQuadLayer.pose = menuPose;
		debugQuadLayer.size = menuSize;
		activeLayers.push_back((XrCompositionLayerBaseHeader*)&debugQuadLayer);
	}

	// Get head position for beam billboard orientation
	XrSpaceLocation headLoc = { XR_TYPE_SPACE_LOCATION };
	xrLocateSpace(xr_gbl->viewSpace, xr_gbl->floorSpace, predictedTime, &headLoc);
	XrVector3f headPos = headLoc.pose.position;

	// Get input system for controller poses
	std::shared_ptr<BaseInput> input = GetBaseInput();
	if (!input || !input->AreActionsLoaded())
		return activeLayers;

	// Get controller states for trigger tracking
	BaseSystem* sys = GetUnsafeBaseSystem();

	// Short default beam when not hitting the menu — just enough to
	// show pointing direction.  When the ray hits the menu surface
	// the beam extends exactly to the dot (stops at the menu).
	constexpr float DEFAULT_BEAM = 0.5f; // meters (visual hint only)

	for (int side = 0; side < 2; side++) {
		// Save previous trigger/thumbstick/X-button state
		triggerLast[side] = triggerState[side];
		thumbstickLast[side] = thumbstickState[side];
		xBtnLast[side] = xBtnState[side];
		appMenuLast[side] = appMenuState[side];
		hitActive[side] = false;

		// Skip this hand if keyboard has it
		if (keyboardHitSide[side])
			continue;

		// Get controller aim space
		XrSpace aimSpace = XR_NULL_HANDLE;
		input->GetHandSpace((vr::TrackedDeviceIndex_t)(side + 1), aimSpace, true);
		if (aimSpace == XR_NULL_HANDLE)
			continue;

		XrSpaceLocation location = { XR_TYPE_SPACE_LOCATION };
		XrResult result = xrLocateSpace(aimSpace, xr_gbl->floorSpace, predictedTime, &location);
		if (XR_FAILED(result) || !(location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) || !(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
			continue;

		XrVector3f rayOrigin = location.pose.position;
		XrVector3f fwd = { 0.0f, 0.0f, -1.0f };
		XrVector3f rayDir;
		rotate_vector_by_quaternion(fwd, location.pose.orientation, rayDir);

		// Store ray data for calibration/diagnostics
		lastRayOrigin[side] = rayOrigin;
		lastRayDir[side] = rayDir;

		// Ray-plane intersection for hit detection and dot placement
		float u, v, t;
		bool hit = RayIntersectQuad(rayOrigin, rayDir, u, v, t);

		if (hit) {
			lastHitT[side] = t;
			hitActive[side] = true;
			hitU[side] = u;
			// Flip V so 0=top, 1=bottom (Scaleform convention: 0,0 is top-left)
			hitV[side] = 1.0f - v;

			XrVector3f hitPoint = {
				rayOrigin.x + t * rayDir.x,
				rayOrigin.y + t * rayDir.y,
				rayOrigin.z + t * rayDir.z
			};
			UpdateDot(side, hitPoint);
			activeLayers.push_back((XrCompositionLayerBaseHeader*)&dotLayer[side]);
		}

		// Beam stops at menu surface when hitting, short hint otherwise
		float beamLen = hit ? t : DEFAULT_BEAM;
		UpdateBeam(side, rayOrigin, rayDir, beamLen, headPos);
		activeLayers.push_back((XrCompositionLayerBaseHeader*)&beamLayer[side]);

		// Track trigger state with hysteresis to prevent bouncing
		if (sys) {
			vr::TrackedDeviceIndex_t devIdx = sys->GetTrackedDeviceIndexForControllerRole(
			    side == 0 ? vr::TrackedControllerRole_LeftHand : vr::TrackedControllerRole_RightHand);
			if (devIdx != vr::k_unTrackedDeviceIndexInvalid) {
				vr::VRControllerState_t ctrlState = {};
				sys->GetControllerState(devIdx, &ctrlState, sizeof(ctrlState));
				float trigVal = ctrlState.rAxis[1].x;
				bool btnPressed = (ctrlState.ulButtonPressed & vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Trigger)) != 0;
				// Hysteresis: press at 0.7, release at 0.3 (prevents bouncing near threshold)
				if (triggerState[side])
					triggerState[side] = (trigVal >= 0.3f) || btnPressed;
				else
					triggerState[side] = (trigVal >= 0.7f) || btnPressed;

				// Thumbstick click — simple digital button for calibration
				bool stickClick = (ctrlState.ulButtonPressed & vr::ButtonMaskFromId(vr::k_EButton_SteamVR_Touchpad)) != 0;
				thumbstickState[side] = stickClick;

				// Thumbstick axis values for in-VR quad adjustment
				thumbstickX[side] = ctrlState.rAxis[0].x;
				thumbstickY[side] = ctrlState.rAxis[0].y;

				// X/A button — for calibration depth probes
				bool xBtn = (ctrlState.ulButtonPressed & vr::ButtonMaskFromId(vr::k_EButton_A)) != 0;
				xBtnState[side] = xBtn;

				// ApplicationMenu button (Y on left, B on right) — for tab cycling
				bool appMenu = (ctrlState.ulButtonPressed & vr::ButtonMaskFromId(vr::k_EButton_ApplicationMenu)) != 0;
				appMenuState[side] = appMenu;
			}
		}
	}

	return activeLayers;
}
