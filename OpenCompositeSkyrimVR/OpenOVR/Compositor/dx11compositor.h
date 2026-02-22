#pragma once

#include "dxcompositor.h"
#include "VRSManager.h"

class DX11Compositor : public Compositor {
public:
	DX11Compositor(ID3D11Texture2D* td);

	virtual ~DX11Compositor() override;

	// Override
	virtual void Invoke(const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds) override;

	virtual void InvokeCubemap(const vr::Texture_t* textures) override;
	virtual bool SupportsCubemap() override { return true; }

	virtual void Invoke(XruEye eye, const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds,
	    vr::EVRSubmitFlags submitFlags, XrCompositionLayerProjectionView& viewport) override;

	ID3D11Device* GetDevice() { return device; }

	// VRS manager — accessible from BaseCompositor for per-eye control
	VRSManager* GetVRSManager() { return &vrsManager; }

protected:
	void CheckCreateSwapChain(const vr::Texture_t* texture, const vr::VRTextureBounds_t* bounds, bool cube);

	void ThrowIfFailed(HRESULT test);

	bool CheckChainCompatible(D3D11_TEXTURE2D_DESC& inputDesc, vr::EColorSpace colourSpace);

	ID3D11Device* device = nullptr;
	ID3D11DeviceContext* context = nullptr;

	ID3D11ShaderResourceView* quad_texture_view;
	ID3D11SamplerState* quad_sampleState;
	ID3D11VertexShader* fs_vshader;
	ID3D11PixelShader* fs_pshader;

	// DLAA anti-aliasing (two-pass: pre-filter + directional AA)
	ID3D11VertexShader* dlaa_vshader = nullptr;
	ID3D11PixelShader* dlaa_pre_pshader = nullptr;   // Pre-filter (edge detection)
	ID3D11PixelShader* dlaa_main_pshader = nullptr;  // Main AA pass
	ID3D11Buffer* dlaa_cbuffer = nullptr;
	ID3D11Texture2D* dlaaIntermediate = nullptr;     // RGBA8 intermediate (RGB=color, A=edge luma)
	ID3D11ShaderResourceView* dlaaIntermediateSRV = nullptr;
	ID3D11RenderTargetView* dlaaIntermediateRTV = nullptr;
	ID3D11Texture2D* dlaaOutput = nullptr;           // AA'd result (same fmt as game texture)
	ID3D11ShaderResourceView* dlaaOutputSRV = nullptr;
	ID3D11RenderTargetView* dlaaOutputRTV = nullptr;
	ID3D11SamplerState* dlaa_pointSampler = nullptr;
	bool dlaaReady = false;
	uint32_t dlaaWidth = 0;
	uint32_t dlaaHeight = 0;

	// Cached game texture SRV (avoids per-frame CreateShaderResourceView/Release)
	ID3D11ShaderResourceView* cachedSrcSRV = nullptr;
	ID3D11Texture2D* cachedSrcTex = nullptr;
	// Pre-created SRVs for MSAA resolved textures
	std::vector<ID3D11ShaderResourceView*> resolvedMSAA_SRVs;

	// FSR/CAS shaders (EASU compiled but unused after FSR1 removal; RCAS used by CAS-only + FSR3+CAS)
	ID3D11VertexShader* fsr_vshader = nullptr;
	ID3D11PixelShader* fsr_pshader = nullptr;   // EASU (upscale) — compiled, currently unused
	ID3D11PixelShader* cas_pshader = nullptr;   // RCAS (sharpen)
	ID3D11Buffer* fsr_cbuffer = nullptr;
	bool fsrReady = false;
	uint32_t fsrInputWidth = 0;   // Expected game texture size (for chain compat check)
	uint32_t fsrInputHeight = 0;

	// Alpha-fix: forces alpha=1.0 after FSR3 output (VD-OpenXR doesn't clear alpha for layer 0)
	ID3D11PixelShader* alphaFix_pshader = nullptr;
	ID3D11BlendState* alphaFix_blendState = nullptr;

	// NVIDIA Variable Rate Shading
	VRSManager vrsManager;

	std::vector<XrSwapchainImageD3D11KHR> imagesHandles;
	std::vector<ID3D11RenderTargetView*> swapchain_rtvs;
	std::vector<ID3D11Texture2D*> resolvedMSAATextures;

	struct DxgiFormatInfo {
		/// The different versions of this format, set to DXGI_FORMAT_UNKNOWN if absent.
		/// Both the SRGB and linear formats should be UNORM.
		DXGI_FORMAT srgb, linear, typeless;

		/// THe bits per pixel, bits per channel, and the number of channels
		int bpp, bpc, channels;
	};

	/**
	 * Gets information about a given format into the output variable. Returns true if the texture was
	 * found, if not it returns false and leaves out in an undefined state.
	 */
	static bool GetFormatInfo(DXGI_FORMAT format, DxgiFormatInfo& out);
};
