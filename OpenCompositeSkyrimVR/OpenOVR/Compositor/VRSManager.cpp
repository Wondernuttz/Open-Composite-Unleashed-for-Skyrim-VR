#include "stdafx.h"

#ifdef OC_HAS_NVAPI

#include "VRSManager.h"
#include "VRSPattern.h"
#include "../Misc/Config.h"
#include "../logging.h"

#include <nvapi.h>
#include <dxgi.h>
#include <cmath>
#include <cstring>

VRSManager::~VRSManager()
{
	Shutdown();
}

bool VRSManager::Initialize(ID3D11Device* dev)
{
	if (available)
		return true;
	if (initializationAttempted)
		return false;

	if (!dev) {
		OOVR_LOG("VRSManager: No D3D11 device provided");
		return false;
	}

	// A valid device identifies this graphics session. From this point on, any
	// unsupported result is final for the session; do not hammer NVAPI from the
	// per-frame compositor path.
	initializationAttempted = true;

	IDXGIDevice* dxgiDevice = nullptr;
	IDXGIAdapter* adapter = nullptr;
	DXGI_ADAPTER_DESC adapterDesc = {};
	if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice)))) {
		if (SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) &&
		    SUCCEEDED(adapter->GetDesc(&adapterDesc)) &&
		    adapterDesc.VendorId != 0x10DE) {
			OOVR_LOGF("VRSManager: NVAPI VRS disabled for non-NVIDIA adapter vendor 0x%04X", adapterDesc.VendorId);
			adapter->Release();
			dxgiDevice->Release();
			return false;
		}
	}
	if (adapter)
		adapter->Release();
	if (dxgiDevice)
		dxgiDevice->Release();

	OOVR_LOG("VRSManager: Initializing NVAPI...");

	{
		// NVAPI wraps game-owned D3D objects which outlive individual compositors.
		// Keep one process-lifetime initialization; unloading it mid-device can
		// leave those objects calling into an unloaded driver wrapper at Release.
		static const NvAPI_Status result = NvAPI_Initialize();
		if (result != NVAPI_OK) {
			OOVR_LOG("VRSManager: NvAPI_Initialize failed — not an NVIDIA GPU or driver issue");
			return false;
		}
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
	const float nextX[2] = { leftPX, rightPX };
	const float nextY[2] = { leftPY, rightPY };
	for (int eye = 0; eye < 2; ++eye) {
		if (std::fabs(uploadedProjX[eye] - nextX[eye]) > 0.0001f ||
		    std::fabs(uploadedProjY[eye] - nextY[eye]) > 0.0001f)
			patternDirty = true;
		projX[eye] = nextX[eye];
		projY[eye] = nextY[eye];
	}
}

static bool SameRegion(const VRSManager::EyeRegion& a, const VRSManager::EyeRegion& b)
{
	return a.left == b.left && a.top == b.top &&
	    a.width == b.width && a.height == b.height;
}

bool VRSManager::UpdateStereoPattern(int nextRenderWidth, int nextRenderHeight,
    const EyeRegion& leftEye, const EyeRegion& rightEye, float innerR, float midR,
    const ocu_foveation::RingRates& rates)
{
	if (!available)
		return false;
	if (nextRenderWidth <= 0 || nextRenderHeight <= 0 ||
	    leftEye.width <= 0 || leftEye.height <= 0 ||
	    rightEye.width <= 0 || rightEye.height <= 0) {
		OOVR_LOG("VRSManager: rejected invalid stereo render-target geometry");
		Disable();
		return false;
	}
	auto regionFits = [nextRenderWidth, nextRenderHeight](const EyeRegion& region) {
		return region.left >= 0 && region.top >= 0 &&
		    region.left + region.width <= nextRenderWidth &&
		    region.top + region.height <= nextRenderHeight;
	};
	const bool regionsOverlap = leftEye.left < rightEye.left + rightEye.width &&
	    leftEye.left + leftEye.width > rightEye.left &&
	    leftEye.top < rightEye.top + rightEye.height &&
	    leftEye.top + leftEye.height > rightEye.top;
	if (!regionFits(leftEye) || !regionFits(rightEye) || regionsOverlap) {
		OOVR_LOG("VRSManager: stereo eye regions are overlapping or outside the bound render target; VRS withheld");
		Disable();
		return false;
	}

	// Mode-specific radii are selected once by the compositor for both backends.
	bool configChanged = (innerR != cachedInnerRadius || midR != cachedMidRadius ||
	    rates != cachedRates);

	cachedInnerRadius = innerR;
	cachedMidRadius = midR;
	cachedRates = rates;
	if (configChanged) {
		OOVR_LOGF("VRS pattern: inner=%.2f mid=%.2f effective rates=%s/%s/%s",
		    innerR, midR, ocu_foveation::RateName(rates.inner),
		    ocu_foveation::RateName(rates.mid), ocu_foveation::RateName(rates.outer));
	}

	// Force shading rate table re-upload on next ApplyStereo if config changed.
	if (configChanged)
		shadingRatesSet = false;

	const int tileW = ocu_vrs_pattern::TileCount(
	    nextRenderWidth, NV_VARIABLE_PIXEL_SHADING_TILE_WIDTH);
	const int tileH = ocu_vrs_pattern::TileCount(
	    nextRenderHeight, NV_VARIABLE_PIXEL_SHADING_TILE_HEIGHT);
	const bool geometryChanged = nextRenderWidth != renderWidth || nextRenderHeight != renderHeight ||
	    !SameRegion(leftEye, eyeRegions[0]) || !SameRegion(rightEye, eyeRegions[1]);
	const bool sizeChanged = tileW != patternWidth || tileH != patternHeight;

	renderWidth = nextRenderWidth;
	renderHeight = nextRenderHeight;
	eyeRegions[0] = leftEye;
	eyeRegions[1] = rightEye;
	if (geometryChanged) {
		patternDirty = true;
		activeViewportValid = true;
		for (int eye = 0; eye < 2; ++eye)
			activeEyeRegions[eye] = {float(eyeRegions[eye].left), float(eyeRegions[eye].top),
			    float(eyeRegions[eye].width), float(eyeRegions[eye].height)};
	}

	if (sizeChanged || !vrsTex || !vrsView)
		SetupStereoPattern();
	else if (configChanged || patternDirty)
		UploadStereoPattern();

	return available && vrsTex != nullptr && vrsView != nullptr && !patternDirty;
}

bool VRSManager::UpdateActiveViewports(UINT count, const D3D11_VIEWPORT* viewports)
{
	if (!available || !vrsTex || !vrsView) return false;
	ocu_vrs_scope::ViewportEyeRegion next[2];
	activeViewportValid = ocu_vrs_scope::MapStereoViewports(renderWidth, renderHeight,
	    eyeRegions, count, viewports, next);
	if (!activeViewportValid) {
		const D3D11_VIEWPORT first = count && viewports ? viewports[0] : D3D11_VIEWPORT{};
		OOVR_LOG_LIMITEDF(5000,
		    "VRS active viewport mapping: full rate for unrecognized/ambiguous layout; atlas=%dx%d count=%u first=(%.2f,%.2f %.2fx%.2f)",
		    renderWidth, renderHeight, count, first.TopLeftX, first.TopLeftY, first.Width, first.Height);
		Disable();
		return false;
	}
	for (int eye = 0; eye < 2; ++eye) {
		const auto& old = activeEyeRegions[eye];
		if (old.left != next[eye].left || old.top != next[eye].top ||
		    old.width != next[eye].width || old.height != next[eye].height)
			patternDirty = true;
		activeEyeRegions[eye] = next[eye];
	}
	if (patternDirty) UploadStereoPattern();
	return !patternDirty;
}

std::vector<uint8_t> VRSManager::CreateStereoPattern() const
{
	float innerR = cachedInnerRadius;
	float midR = cachedMidRadius;

	std::vector<uint8_t> data(patternWidth * patternHeight,
	    static_cast<uint8_t>(ocu_vrs_pattern::Level::Full));

	for (int y = 0; y < patternHeight; ++y) {
		for (int x = 0; x < patternWidth; ++x) {
			// Sample the center of the hardware VRS tile in render-target pixels.
			// The submitted eye bounds may be horizontal, vertical, or asymmetric.
			const float pixelX = (float)(x * NV_VARIABLE_PIXEL_SHADING_TILE_WIDTH) +
			    NV_VARIABLE_PIXEL_SHADING_TILE_WIDTH * 0.5f;
			const float pixelY = (float)(y * NV_VARIABLE_PIXEL_SHADING_TILE_HEIGHT) +
			    NV_VARIABLE_PIXEL_SHADING_TILE_HEIGHT * 0.5f;

			for (int eye = 0; eye < 2; ++eye) {
				const auto& region = activeEyeRegions[eye];
				float fx = 0.0f;
				float fy = 0.0f;
				// One hardware tile cannot serve different eyes or an eye and
				// atlas padding. Boundary tiles stay full rate, including offsets.
				if (pixelX - NV_VARIABLE_PIXEL_SHADING_TILE_WIDTH * 0.5f < region.left ||
				    pixelY - NV_VARIABLE_PIXEL_SHADING_TILE_HEIGHT * 0.5f < region.top ||
				    pixelX + NV_VARIABLE_PIXEL_SHADING_TILE_WIDTH * 0.5f > region.left + region.width ||
				    pixelY + NV_VARIABLE_PIXEL_SHADING_TILE_HEIGHT * 0.5f > region.top + region.height)
					continue;
				fx = (pixelX - region.left) / region.width;
				fy = (pixelY - region.top) / region.height;

				// Distance from that eye's projection/gaze center, scaled so a
				// radius of 1.0 reaches the edge from a centered gaze.
				const float dx = fx - projX[eye];
				const float dy = fy - projY[eye];
				const float distance = 2.0f * std::sqrt(dx * dx + dy * dy);
				data[y * patternWidth + x] = static_cast<uint8_t>(
				    1 + static_cast<unsigned>(ocu_vrs_pattern::SelectLevel(distance, innerR, midR, false)));
				break;
			}
		}
	}

	return data;
}

void VRSManager::SetupStereoPattern()
{
	if (!available || !device)
		return;

	ReleasePatternResources();

	patternWidth = ocu_vrs_pattern::TileCount(
	    renderWidth, NV_VARIABLE_PIXEL_SHADING_TILE_WIDTH);
	patternHeight = ocu_vrs_pattern::TileCount(
	    renderHeight, NV_VARIABLE_PIXEL_SHADING_TILE_HEIGHT);

	OOVR_LOGF(
	    "VRSManager: Creating stereo-atlas VRS pattern: %dx%d tiles for %dx%d target; L=(%d,%d %dx%d @ %.3f,%.3f) R=(%d,%d %dx%d @ %.3f,%.3f)",
	    patternWidth, patternHeight, renderWidth, renderHeight,
	    eyeRegions[0].left, eyeRegions[0].top, eyeRegions[0].width, eyeRegions[0].height,
	    projX[0], projY[0], eyeRegions[1].left, eyeRegions[1].top,
	    eyeRegions[1].width, eyeRegions[1].height, projX[1], projY[1]);

	auto data = CreateStereoPattern();

	// Create R8_UINT texture
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = patternWidth;
	td.Height = patternHeight;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8_UINT;
	td.SampleDesc.Count = 1;
	td.SampleDesc.Quality = 0;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	td.CPUAccessFlags = 0;
	td.MiscFlags = 0;
	td.MipLevels = 1;

	HRESULT hr = device->CreateTexture2D(&td, nullptr, &vrsTex);
	if (FAILED(hr)) {
		OOVR_LOGF("VRSManager: Failed to create stereo-atlas VRS texture: 0x%08X", hr);
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
	NvAPI_Status status = NvAPI_D3D11_CreateShadingRateResourceView(device, vrsTex, &vd, &view);
	vrsView = view;
	if (status != NVAPI_OK) {
		OOVR_LOGF("VRSManager: Failed to create stereo-atlas VRS resource view: %d", status);
		ReleasePatternResources();
		available = false;
		return;
	}
	// NVAPI establishes its shading-rate resource tracking when the view is
	// created. Publish the first pattern afterwards, just like later gaze updates;
	// CreateTexture2D initial data alone can leave fixed foveation at full rate.
	context->UpdateSubresource(vrsTex, 0, nullptr, data.data(), patternWidth, 0);
	++patternUpdates.resourceCreations;
	++patternUpdates.uploads;
	for (int eye = 0; eye < 2; ++eye) {
		uploadedProjX[eye] = projX[eye];
		uploadedProjY[eye] = projY[eye];
	}
	patternDirty = false;
}

void VRSManager::UploadStereoPattern()
{
	if (!available || !context || !vrsTex || patternWidth <= 0 || patternHeight <= 0)
		return;

	auto data = CreateStereoPattern();
	context->UpdateSubresource(vrsTex, 0, nullptr, data.data(), patternWidth, 0);
	++patternUpdates.uploads;
	for (int eye = 0; eye < 2; ++eye) {
		uploadedProjX[eye] = projX[eye];
		uploadedProjY[eye] = projY[eye];
	}
	patternDirty = false;
}

bool VRSManager::EnableShadingRates()
{
	auto nativeRate = [](ocu_foveation::Rate rate) {
		using ocu_foveation::Rate;
		switch (rate) {
		case Rate::X1x2: return NV_PIXEL_X1_PER_1X2_RASTER_PIXELS;
		case Rate::X2x1: return NV_PIXEL_X1_PER_2X1_RASTER_PIXELS;
		case Rate::X2x2: return NV_PIXEL_X1_PER_2X2_RASTER_PIXELS;
		case Rate::X2x4: return NV_PIXEL_X1_PER_2X4_RASTER_PIXELS;
		case Rate::X4x2: return NV_PIXEL_X1_PER_4X2_RASTER_PIXELS;
		case Rate::X4x4: return NV_PIXEL_X1_PER_4X4_RASTER_PIXELS;
		default: return NV_PIXEL_X1_PER_RASTER_PIXEL;
		}
	};
	NV_D3D11_VIEWPORT_SHADING_RATE_DESC vsrd[NV_MAX_NUM_VIEWPORTS] = {};
	for (int i = 0; i < NV_MAX_NUM_VIEWPORTS; ++i) {
		vsrd[i].enableVariablePixelShadingRate = true;
		// This is an enum array, so byte-wise memset would create invalid values.
		for (int rate = 0; rate < NV_MAX_PIXEL_SHADING_RATES; ++rate)
			vsrd[i].shadingRateTable[rate] = NV_PIXEL_X1_PER_RASTER_PIXEL;
		// Set the rings: full → half → quarter
		vsrd[i].shadingRateTable[0] = NV_PIXEL_X1_PER_RASTER_PIXEL;
		// Index zero remains full rate for pixels outside either eye region.
		vsrd[i].shadingRateTable[1] = nativeRate(cachedRates.inner);
		vsrd[i].shadingRateTable[2] = nativeRate(cachedRates.mid);
		vsrd[i].shadingRateTable[3] = nativeRate(cachedRates.outer);
	}

	NV_D3D11_VIEWPORTS_SHADING_RATE_DESC srd = {};
	srd.version = NV_D3D11_VIEWPORTS_SHADING_RATE_DESC_VER;
	// Skyrim and runtime wrappers vary between one, two, and array viewports.
	// Configure every D3D11 viewport slot so VRS remains active when the game
	// changes RS viewport count after WaitGetPoses.
	srd.numViewports = NV_MAX_NUM_VIEWPORTS;
	srd.pViewports = vsrd;

	NvAPI_Status status = NvAPI_D3D11_RSSetViewportsPixelShadingRates(context, &srd);
	if (status != NVAPI_OK) {
		OOVR_LOGF("VRSManager: Failed to set viewport shading rates: %d", status);
		Shutdown();
		return false;
	}
	return true;
}

bool VRSManager::ApplyStereo()
{
	if (!activeViewportValid || patternDirty) return false;
	if (!available) {
		OOVR_LOG_LIMITEDF(5000, "VRSManager::ApplyStereo: skipped (available=%d)", (int)available);
		return false;
	}
	if (!vrsView) {
		OOVR_LOG_LIMITEDF(5000, "VRSManager::ApplyStereo: skipped — resource view is null");
		return false;
	}

	// Set the viewport shading rate table once (persists until config changes)
	if (!shadingRatesSet) {
		if (!EnableShadingRates())
			return false;
		shadingRatesSet = true;
	}

	auto* view = static_cast<ID3D11NvShadingRateResourceView*>(vrsView);
	NvAPI_Status status = NvAPI_D3D11_RSSetShadingRateResourceView(context, view);
	if (status != NVAPI_OK) {
		OOVR_LOGF("VRSManager: Failed to set stereo-atlas shading rate resource view: %d", status);
		Shutdown();
		return false;
	}

	static int logCount = 0;
	if (logCount < 3) {
		OOVR_LOGF("VRSManager: Applied stereo-atlas VRS before scene render (L=%.3f,%.3f R=%.3f,%.3f tiles=%dx%d)",
		    projX[0], projY[0], projX[1], projY[1], patternWidth, patternHeight);
		++logCount;
	}
	return true;
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

void VRSManager::ReleasePatternResources()
{
	if (vrsView) {
		// ID3D11NvShadingRateResourceView derives from ID3D11View/IUnknown.
		static_cast<ID3D11NvShadingRateResourceView*>(vrsView)->Release();
		vrsView = nullptr;
	}
	if (vrsTex) {
		vrsTex->Release();
		vrsTex = nullptr;
	}
	patternWidth = 0;
	patternHeight = 0;
	patternDirty = true;
}

void VRSManager::Shutdown()
{
	if (available && context) {
		// Full cleanup: clear resource view AND viewport shading rates
		NvAPI_D3D11_RSSetShadingRateResourceView(context, nullptr);

		// NVAPI specifies numViewports=0 as the global VRS disable operation.
		NV_D3D11_VIEWPORTS_SHADING_RATE_DESC srd = {};
		srd.version = NV_D3D11_VIEWPORTS_SHADING_RATE_DESC_VER;
		srd.numViewports = 0;
		srd.pViewports = nullptr;
		NvAPI_D3D11_RSSetViewportsPixelShadingRates(context, &srd);
	}
	shadingRatesSet = false;
	activeViewportValid = true;

	ReleasePatternResources();

	if (context) {
		context->Release();
		context = nullptr;
	}
	device = nullptr;

	available = false;
}

#endif // OC_HAS_NVAPI
