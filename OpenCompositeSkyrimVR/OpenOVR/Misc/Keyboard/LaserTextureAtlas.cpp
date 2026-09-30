#include "stdafx.h"

#include "LaserTextureAtlas.h"

#include <atlbase.h>
#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <vector>

#include "BeamTexture.h"
#include "LaserDotTexture.h"
#include "generated/static_bases.gen.h"

namespace {
	constexpr int kPadding = 4;
	constexpr int kAtlasWidth = 160;
	constexpr int kAtlasHeight = 272;

	// Keep at least eight transparent pixels between sprites. The swapchain has
	// one mip level, and the padding also protects runtimes that filter just
	// outside an imageRect edge.
	constexpr XrRect2Di kRegions[] = {
		{ { 4, 4 }, { beamtex::kW, beamtex::kH } },       // BeamIdle
		{ { 44, 4 }, { beamtex::kW, beamtex::kH } },      // BeamClicked
		{ { 84, 4 }, { laserdot::kSize, laserdot::kSize } },  // DotIdle
		{ { 84, 76 }, { laserdot::kSize, laserdot::kSize } }, // DotClicked
	};

	static_assert(kRegions[1].offset.x + kRegions[1].extent.width + kPadding <= kAtlasWidth);
	static_assert(kRegions[0].offset.y + kRegions[0].extent.height + kPadding <= kAtlasHeight);
	static_assert(kRegions[3].offset.y + kRegions[3].extent.height + kPadding <= kAtlasHeight);

	std::mutex g_atlasMutex;
	std::weak_ptr<LaserTextureAtlas> g_sharedAtlas;

	bool IsBgraFormat(DXGI_FORMAT candidate)
	{
		return candidate == DXGI_FORMAT_B8G8R8A8_UNORM ||
		       candidate == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
	}

	DXGI_FORMAT PickSupportedFormat(const std::vector<int64_t>& supported,
	    std::initializer_list<DXGI_FORMAT> preferences)
	{
		for (DXGI_FORMAT candidate : preferences) {
			if (std::find(supported.begin(), supported.end(), static_cast<int64_t>(candidate)) != supported.end())
				return candidate;
		}
		return DXGI_FORMAT_UNKNOWN;
	}

	const char* FormatName(DXGI_FORMAT candidate)
	{
		switch (candidate) {
		case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
		case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
		case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
		case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
		default: return "UNKNOWN";
		}
	}

	void Blit(std::vector<uint32_t>& atlas, const std::vector<uint32_t>& source,
	    const XrRect2Di& destination)
	{
		const size_t rowBytes = static_cast<size_t>(destination.extent.width) * sizeof(uint32_t);
		for (int y = 0; y < destination.extent.height; ++y) {
			uint32_t* dst = atlas.data() +
			    static_cast<size_t>(destination.offset.y + y) * kAtlasWidth + destination.offset.x;
			const uint32_t* src = source.data() +
			    static_cast<size_t>(y) * destination.extent.width;
			std::memcpy(dst, src, rowBytes);
		}
	}
}

std::shared_ptr<LaserTextureAtlas> LaserTextureAtlas::Acquire(ID3D11Device* dev)
{
	if (!dev)
		OOVR_ABORT("Laser texture atlas requires a D3D11 device");

	const XrSession currentSession = xr_session.get();
	std::lock_guard<std::mutex> lock(g_atlasMutex);
	if (auto existing = g_sharedAtlas.lock()) {
		if (existing->GetSession() == currentSession)
			return existing;
	}

	auto created = std::shared_ptr<LaserTextureAtlas>(
	    new LaserTextureAtlas(dev, currentSession));
	g_sharedAtlas = created;
	return created;
}

LaserTextureAtlas::LaserTextureAtlas(ID3D11Device* dev, XrSession activeSession)
    : session(activeSession)
{
	uint32_t formatCount = 0;
	OOVR_FAILED_XR_ABORT(xrEnumerateSwapchainFormats(session, 0, &formatCount, nullptr));
	std::vector<int64_t> runtimeFormats(formatCount);
	OOVR_FAILED_XR_ABORT(xrEnumerateSwapchainFormats(
	    session, formatCount, &formatCount, runtimeFormats.data()));

	format = PickSupportedFormat(runtimeFormats, {
	    DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
	    DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
	    DXGI_FORMAT_R8G8B8A8_UNORM,
	    DXGI_FORMAT_B8G8R8A8_UNORM,
	});
	if (format == DXGI_FORMAT_UNKNOWN)
		OOVR_ABORT("Laser texture atlas requires a supported 8-bit RGBA/BGRA swapchain format");

	XrSwapchainCreateInfo sci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
	sci.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
	    XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
	    XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
	sci.format = static_cast<int64_t>(format);
	sci.sampleCount = 1;
	sci.width = kAtlasWidth;
	sci.height = kAtlasHeight;
	sci.faceCount = 1;
	sci.arraySize = 1;
	sci.mipCount = 1;
	OOVR_FAILED_XR_ABORT(xrCreateSwapchain(session, &sci, &chain));

	uint32_t imageCount = 0;
	OOVR_FAILED_XR_ABORT(xrEnumerateSwapchainImages(chain, 0, &imageCount, nullptr));
	std::vector<XrSwapchainImageD3D11KHR> images(
	    imageCount, { XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR });
	OOVR_FAILED_XR_ABORT(xrEnumerateSwapchainImages(chain, imageCount, &imageCount,
	    reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())));

	std::vector<uint32_t> atlas(static_cast<size_t>(kAtlasWidth) * kAtlasHeight, 0);
	std::vector<uint32_t> sprite;
	const bool bgra = IsBgraFormat(format);

	uint8_t idleR = 255, idleG = 240, idleB = 220;
	uint8_t clickR = 55, clickG = 145, clickB = 255;
	if (bgra) {
		std::swap(idleR, idleB);
		std::swap(clickR, clickB);
	}
	beamtex::Fill(sprite, idleR, idleG, idleB, 200);
	Blit(atlas, sprite, kRegions[static_cast<int>(LaserAtlasRegion::BeamIdle)]);
	beamtex::Fill(sprite, clickR, clickG, clickB, 220);
	Blit(atlas, sprite, kRegions[static_cast<int>(LaserAtlasRegion::BeamClicked)]);
	laserdot::Fill(sprite, bgra);
	Blit(atlas, sprite, kRegions[static_cast<int>(LaserAtlasRegion::DotIdle)]);
	laserdot::Fill(sprite, bgra,
	    { 225, 245, 255 }, { 90, 175, 255 }, { 35, 105, 255 });
	Blit(atlas, sprite, kRegions[static_cast<int>(LaserAtlasRegion::DotClicked)]);

	D3D11_TEXTURE2D_DESC textureDesc = {};
	textureDesc.Width = kAtlasWidth;
	textureDesc.Height = kAtlasHeight;
	textureDesc.MipLevels = 1;
	textureDesc.ArraySize = 1;
	textureDesc.Format = format;
	textureDesc.SampleDesc = { 1, 0 };
	textureDesc.Usage = D3D11_USAGE_DEFAULT;
	D3D11_SUBRESOURCE_DATA initialData = {
	    atlas.data(), sizeof(uint32_t) * kAtlasWidth,
	    sizeof(uint32_t) * kAtlasWidth * kAtlasHeight
	};
	CComPtr<ID3D11Texture2D> texture;
	OOVR_FAILED_DX_ABORT(dev->CreateTexture2D(&textureDesc, &initialData, &texture));

	XrSwapchainImageAcquireInfo acquireInfo = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	uint32_t imageIndex = 0;
	OOVR_FAILED_XR_ABORT(xrAcquireSwapchainImage(chain, &acquireInfo, &imageIndex));
	XrSwapchainImageWaitInfo waitInfo = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	waitInfo.timeout = 500000000;
	OOVR_FAILED_XR_ABORT(xrWaitSwapchainImage(chain, &waitInfo));
	CComPtr<ID3D11DeviceContext> context;
	dev->GetImmediateContext(&context);
	if (!context)
		OOVR_ABORT("Laser texture atlas could not acquire the D3D11 immediate context");
	context->CopyResource(images[imageIndex].texture, texture);
	XrSwapchainImageReleaseInfo releaseInfo = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	OOVR_FAILED_XR_ABORT(xrReleaseSwapchainImage(chain, &releaseInfo));

	OOVR_LOGF("Laser texture atlas created: 1 shared swapchain, 4 sprites, %dx%d, %s",
	    kAtlasWidth, kAtlasHeight, FormatName(format));
}

LaserTextureAtlas::~LaserTextureAtlas()
{
	if (chain != XR_NULL_HANDLE) {
		xrDestroySwapchain(chain);
		chain = XR_NULL_HANDLE;
	}
}

XrRect2Di LaserTextureAtlas::GetImageRect(LaserAtlasRegion region) const
{
	const int index = static_cast<int>(region);
	if (index < 0 || index >= static_cast<int>(std::size(kRegions)))
		return {};
	return kRegions[index];
}
