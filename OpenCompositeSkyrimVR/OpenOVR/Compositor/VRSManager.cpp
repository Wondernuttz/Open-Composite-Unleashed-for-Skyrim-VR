#include "stdafx.h"

#ifdef OC_HAS_NVAPI

#include "VRSManager.h"
#include "../Misc/Config.h"
#include "../logging.h"

#include <nvapi.h>
#include <cmath>
#include <cstring>

static uint8_t DistanceToVRSLevel(float distance, float innerR, float midR, float outerR)
{
	if (distance < innerR)
		return 0; // Full rate (1x1)
	if (distance < midR)
		return 1; // Half rate (2x1 or 1x2)
	return 2; // Quarter rate (2x2)
}

VRSManager::~VRSManager()
{
	Shutdown();
}

bool VRSManager::Initialize(ID3D11Device* dev)
{
	if (available)
		return true;

	if (!dev) {
		OOVR_LOG("VRSManager: No D3D11 device provided");
		return false;
	}

	OOVR_LOG("VRSManager: Initializing NVAPI...");

	if (!nvapiLoaded) {
		NvAPI_Status result = NvAPI_Initialize();
		if (result != NVAPI_OK) {
			OOVR_LOG("VRSManager: NvAPI_Initialize failed — not an NVIDIA GPU or driver issue");
			return false;
		}
		nvapiLoaded = true;
	}

	// Check if this GPU supports Variable Pixel Rate Shading
	NV_D3D1x_GRAPHICS_CAPS caps;
	memset(&caps, 0, sizeof(caps));
	NvAPI_Status status = NvAPI_D3D1x_GetGraphicsCapabilities(dev, NV_D3D1x_GRAPHICS_CAPS_VER, &caps);
	if (status != NVAPI_OK || !caps.bVariablePixelRateShadingSupported) {
		OOVR_LOG("VRSManager: Variable Rate Shading is NOT supported on this GPU");
		return false;
	}

	device = dev;
	device->GetImmediateContext(&context);
	available = true;

	OOVR_LOG("VRSManager: VRS is available and initialized successfully");
	return true;
}

void VRSManager::SetProjectionCenters(float leftPX, float leftPY, float rightPX, float rightPY)
{
	projX[0] = leftPX;
	projY[0] = leftPY;
	projX[1] = rightPX;
	projY[1] = rightPY;
}

void VRSManager::UpdatePatterns(int eyeWidth, int eyeHeight)
{
	if (!available)
		return;

	// Check if config changed
	float innerR = oovr_global_configuration.VrsInnerRadius();
	float midR = oovr_global_configuration.VrsMidRadius();
	float outerR = oovr_global_configuration.VrsOuterRadius();
	bool favorH = oovr_global_configuration.VrsFavorHorizontal();

	bool configChanged = (innerR != cachedInnerRadius || midR != cachedMidRadius ||
	    outerR != cachedOuterRadius || favorH != cachedFavorHorizontal);

	cachedInnerRadius = innerR;
	cachedMidRadius = midR;
	cachedOuterRadius = outerR;
	cachedFavorHorizontal = favorH;

	// Force shading rate table re-upload on next ApplyForEye if config changed
	if (configChanged)
		shadingRatesSet = false;

	// Recreate patterns for each eye if needed
	int tileW = eyeWidth / NV_VARIABLE_PIXEL_SHADING_TILE_WIDTH;
	int tileH = eyeHeight / NV_VARIABLE_PIXEL_SHADING_TILE_HEIGHT;

	for (int eye = 0; eye < 2; ++eye) {
		bool sizeChanged = (tileW != patternWidth[eye] || tileH != patternHeight[eye]);
		if (!sizeChanged && !configChanged && vrsTex[eye])
			continue;

		SetupEyePattern(eye, eyeWidth, eyeHeight);
	}
}

std::vector<uint8_t> VRSManager::CreatePattern(int tileWidth, int tileHeight, float pX, float pY)
{
	float innerR = oovr_global_configuration.VrsInnerRadius();
	float midR = oovr_global_configuration.VrsMidRadius();
	float outerR = oovr_global_configuration.VrsOuterRadius();

	std::vector<uint8_t> data(tileWidth * tileHeight);

	for (int y = 0; y < tileHeight; ++y) {
		for (int x = 0; x < tileWidth; ++x) {
			float fx = (float)x / (float)tileWidth;
			float fy = (float)y / (float)tileHeight;
			// Distance from projection center, scaled by 2 so radius 1.0 = edge of screen
			float distance = 2.0f * sqrtf((fx - pX) * (fx - pX) + (fy - pY) * (fy - pY));

			data[y * tileWidth + x] = DistanceToVRSLevel(distance, innerR, midR, outerR);
		}
	}

	return data;
}

void VRSManager::SetupEyePattern(int eye, int eyeWidth, int eyeHeight)
{
	if (!available || !device)
		return;

	ReleaseEyeResources(eye);

	int tileW = eyeWidth / NV_VARIABLE_PIXEL_SHADING_TILE_WIDTH;
	int tileH = eyeHeight / NV_VARIABLE_PIXEL_SHADING_TILE_HEIGHT;

	patternWidth[eye] = tileW;
	patternHeight[eye] = tileH;

	OOVR_LOGF("VRSManager: Creating VRS pattern for eye %d: %dx%d tiles (eye res %dx%d, projCenter %.2f,%.2f)",
	    eye, tileW, tileH, eyeWidth, eyeHeight, projX[eye], projY[eye]);

	// Create the pattern data
	auto data = CreatePattern(tileW, tileH, projX[eye], projY[eye]);

	// Create R8_UINT texture
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = tileW;
	td.Height = tileH;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8_UINT;
	td.SampleDesc.Count = 1;
	td.SampleDesc.Quality = 0;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	td.CPUAccessFlags = 0;
	td.MiscFlags = 0;
	td.MipLevels = 1;

	D3D11_SUBRESOURCE_DATA srd = {};
	srd.pSysMem = data.data();
	srd.SysMemPitch = tileW;
	srd.SysMemSlicePitch = 0;

	HRESULT hr = device->CreateTexture2D(&td, &srd, &vrsTex[eye]);
	if (FAILED(hr)) {
		OOVR_LOGF("VRSManager: Failed to create VRS texture for eye %d: 0x%08X", eye, hr);
		available = false;
		return;
	}

	// Create NVAPI shading rate resource view
	NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC vd = {};
	vd.version = NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC_VER;
	vd.Format = DXGI_FORMAT_R8_UINT;
	vd.ViewDimension = NV_SRRV_DIMENSION_TEXTURE2D;
	vd.Texture2D.MipSlice = 0;

	ID3D11NvShadingRateResourceView* view = nullptr;
	NvAPI_Status status = NvAPI_D3D11_CreateShadingRateResourceView(device, vrsTex[eye], &vd, &view);
	vrsView[eye] = view;
	if (status != NVAPI_OK) {
		OOVR_LOGF("VRSManager: Failed to create VRS resource view for eye %d: %d", eye, status);
		available = false;
		return;
	}
}

void VRSManager::EnableShadingRates()
{
	NV_D3D11_VIEWPORT_SHADING_RATE_DESC vsrd[2];
	for (int i = 0; i < 2; ++i) {
		vsrd[i].enableVariablePixelShadingRate = true;
		// Fill table: default to max rate (2x2)
		memset(vsrd[i].shadingRateTable, NV_PIXEL_X1_PER_2X2_RASTER_PIXELS, sizeof(vsrd[i].shadingRateTable));
		// Set the rings: full → half → quarter
		vsrd[i].shadingRateTable[0] = NV_PIXEL_X1_PER_RASTER_PIXEL;
		vsrd[i].shadingRateTable[1] = cachedFavorHorizontal
		    ? NV_PIXEL_X1_PER_2X1_RASTER_PIXELS
		    : NV_PIXEL_X1_PER_1X2_RASTER_PIXELS;
		vsrd[i].shadingRateTable[2] = NV_PIXEL_X1_PER_2X2_RASTER_PIXELS;
	}

	NV_D3D11_VIEWPORTS_SHADING_RATE_DESC srd;
	srd.version = NV_D3D11_VIEWPORTS_SHADING_RATE_DESC_VER;
	srd.numViewports = 2;
	srd.pViewports = vsrd;

	NvAPI_Status status = NvAPI_D3D11_RSSetViewportsPixelShadingRates(context, &srd);
	if (status != NVAPI_OK) {
		OOVR_LOGF("VRSManager: Failed to set viewport shading rates: %d", status);
		Shutdown();
	}
}

void VRSManager::ApplyForEye(int eye)
{
	if (!available || eye < 0 || eye > 1) {
		OOVR_LOGF("VRSManager::ApplyForEye(%d): skipped (available=%d)", eye, (int)available);
		return;
	}
	if (!vrsView[eye]) {
		OOVR_LOGF("VRSManager::ApplyForEye(%d): skipped — vrsView is null (view[0]=%p, view[1]=%p)",
		    eye, vrsView[0], vrsView[1]);
		return;
	}

	// Set the viewport shading rate table once (persists until config changes)
	if (!shadingRatesSet) {
		EnableShadingRates();
		shadingRatesSet = true;
	}

	// Only NVAPI call per-frame: swap the per-tile resource view for this eye
	auto* view = static_cast<ID3D11NvShadingRateResourceView*>(vrsView[eye]);
	NvAPI_Status status = NvAPI_D3D11_RSSetShadingRateResourceView(context, view);
	if (status != NVAPI_OK) {
		OOVR_LOGF("VRSManager: Failed to set shading rate resource view for eye %d: %d", eye, status);
		Shutdown();
		return;
	}

	// Log first few applications per eye to confirm both eyes get VRS
	static int logCount[2] = { 0, 0 };
	if (logCount[eye] < 3) {
		OOVR_LOGF("VRSManager: Applied VRS pattern for eye %d (projCenter=%.3f,%.3f, tiles=%dx%d)",
		    eye, projX[eye], projY[eye], patternWidth[eye], patternHeight[eye]);
		logCount[eye]++;
	}
}

void VRSManager::Disable()
{
	if (!available || !context)
		return;

	// Just clear the resource view — without a per-tile texture, VRS has no effect
	// even if the viewport shading rates are still configured. This avoids redundant
	// NVAPI calls (was 2 calls, now 1 — saves ~4 driver calls per frame).
	NvAPI_D3D11_RSSetShadingRateResourceView(context, nullptr);
}

void VRSManager::ReleaseEyeResources(int eye)
{
	if (vrsView[eye]) {
		// NVAPI views don't use COM Release — they're freed when the texture is released
		vrsView[eye] = nullptr;
	}
	if (vrsTex[eye]) {
		vrsTex[eye]->Release();
		vrsTex[eye] = nullptr;
	}
	patternWidth[eye] = 0;
	patternHeight[eye] = 0;
}

void VRSManager::Shutdown()
{
	if (available && context) {
		// Full cleanup: clear resource view AND viewport shading rates
		NvAPI_D3D11_RSSetShadingRateResourceView(context, nullptr);

		NV_D3D11_VIEWPORT_SHADING_RATE_DESC vsrd[2];
		vsrd[0].enableVariablePixelShadingRate = false;
		vsrd[1].enableVariablePixelShadingRate = false;
		memset(vsrd[0].shadingRateTable, 0, sizeof(vsrd[0].shadingRateTable));
		memset(vsrd[1].shadingRateTable, 0, sizeof(vsrd[1].shadingRateTable));

		NV_D3D11_VIEWPORTS_SHADING_RATE_DESC srd;
		srd.version = NV_D3D11_VIEWPORTS_SHADING_RATE_DESC_VER;
		srd.numViewports = 2;
		srd.pViewports = vsrd;
		NvAPI_D3D11_RSSetViewportsPixelShadingRates(context, &srd);
	}
	shadingRatesSet = false;

	for (int i = 0; i < 2; ++i) {
		ReleaseEyeResources(i);
	}

	if (context) {
		context->Release();
		context = nullptr;
	}
	device = nullptr;

	if (nvapiLoaded) {
		NvAPI_Unload();
		nvapiLoaded = false;
	}

	available = false;
}

#endif // OC_HAS_NVAPI
