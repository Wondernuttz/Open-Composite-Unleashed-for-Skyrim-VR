#pragma once

#ifdef OC_HAS_DLSS

#include <d3d11.h>
#include <cstdint>

// NVIDIA DLSS NGX SDK headers (present only when libs/dlss/ is installed)
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>

/// DLSS 4 Super Resolution upscaler — native DX11 NGX API.
/// Simpler than Fsr3Upscaler: no DX12 device, no shared textures, no cross-API fence.
/// NGX manages its own internal DX12 device; all input/output is plain DX11 textures.
class DlssUpscaler {
public:
	DlssUpscaler() = default;
	~DlssUpscaler();

	DlssUpscaler(const DlssUpscaler&) = delete;
	DlssUpscaler& operator=(const DlssUpscaler&) = delete;

	/// Initialize NGX on the given DX11 device. Returns false on non-NVIDIA GPU or old driver.
	bool Initialize(ID3D11Device* d3d11Device);
	void Shutdown();
	bool IsReady() const { return m_ready; }

	/// Parameters for a single DLSS dispatch (mirrors Fsr3Upscaler::DispatchParams).
	struct DispatchParams {
		ID3D11Texture2D* color;              ///< Game's rendered color at render resolution
		const D3D11_BOX* colorSourceRegion;  ///< Per-eye sub-region (nullptr = full texture)
		ID3D11Texture2D* motionVectors;      ///< MV texture (R16G16_FLOAT, may be stereo-combined)
		const D3D11_BOX* mvSourceRegion;     ///< Per-eye sub-region (nullptr = full texture)
		ID3D11Texture2D* depth;              ///< Depth texture (R32_FLOAT), may be nullptr
		const D3D11_BOX* depthSourceRegion;  ///< Per-eye sub-region (nullptr = full texture)
		float jitterX, jitterY;              ///< Sub-pixel jitter applied to camera this frame
		float deltaTimeMs;                   ///< Frame time in milliseconds
		uint32_t renderWidth, renderHeight;  ///< Input (game) resolution per eye
		uint32_t outputWidth, outputHeight;  ///< Target output (display) resolution per eye
		float cameraNear, cameraFar;         ///< Camera planes
		float sharpness;                     ///< Post-upscale sharpness (0.0-1.0)
		bool reset;                          ///< True on camera teleport / scene change
		float mvScaleX, mvScaleY;            ///< UV-to-pixel conversion (renderW/H for UV-space MVs, 1.0 for pixel-space)
		ID3D11Texture2D* biasMask;           ///< Bias current color mask (R8_UNORM, render-res). Higher = prefer current frame over history.
		const D3D11_BOX* biasMaskSourceRegion; ///< Per-eye sub-region (nullptr = full texture)
		int debugMode;                       ///< 0=off, 2=bypass (raw game image)
	};

	/// Override the DLSS quality preset for this upscaler instance.
	/// -1 = use global config (default), 0-4 = force specific preset.
	void SetPresetOverride(int preset) { m_presetOverride = preset; }

	/// Dispatch DLSS for one eye (0=left, 1=right). Synchronous — no 1-frame delay.
	/// Returns false on error; GetOutputDX11() returns the upscaled result.
	bool Dispatch(int eyeIdx, ID3D11DeviceContext* d3d11Ctx, const DispatchParams& params);

	/// Dispatch DLSS for ASW warp output (separate history; caller controls reset).
	/// Uses separate NGX handles (indices 2-3) to preserve game temporal history.
	bool DispatchWarp(int eyeIdx, ID3D11DeviceContext* d3d11Ctx, const DispatchParams& params);

	/// Get the DX11 texture containing the upscaled output for the given eye.
	ID3D11Texture2D* GetOutputDX11(int eyeIdx) const;

	/// Get the DX11 texture containing the upscaled warp output for the given eye.
	ID3D11Texture2D* GetWarpOutputDX11(int eyeIdx) const;

	// Jitter utilities — same Halton[2,3] sequence as FSR3.
	static void GetJitterOffset(float* outX, float* outY, int frameIndex, int phaseCount);
	static int  GetJitterPhaseCount(uint32_t renderWidth, uint32_t displayWidth);

private:
	bool EnsureFeature(int eyeIdx, ID3D11DeviceContext* ctx,
	    uint32_t renderW, uint32_t renderH, uint32_t outputW, uint32_t outputH);
	void DestroyFeature(int eyeIdx, ID3D11DeviceContext* ctx);
	bool EnsureOutputTexture(int eyeIdx, uint32_t outputW, uint32_t outputH);
	void DestroyOutputTexture(int eyeIdx);
	bool EnsureStagingTextures(uint32_t renderW, uint32_t renderH,
	    ID3D11Texture2D* colorSrc, ID3D11Texture2D* mvSrc, ID3D11Texture2D* depthSrc);
	void DestroyStagingTextures();

	int m_presetOverride = -1; // -1 = use global config
	bool m_ready = false;
	ID3D11Device* m_device = nullptr;   // NOT owned

	// NGX handles and shared parameter block
	// [0-1] = game eyes (temporal accumulation), [2-3] = warp eyes (separate ASW history)
	static constexpr int kHandleCount = 4;
	NVSDK_NGX_Handle*    m_handle[kHandleCount]  = {};
	NVSDK_NGX_Parameter* m_params     = nullptr;

	// Per-eye output texture (display resolution, UA-bindable for DLSS write)
	// [0-1] = game output, [2-3] = warp output
	ID3D11Texture2D* m_output[kHandleCount] = {};

	// Staging textures for per-eye copy from stereo-combined inputs
	ID3D11Texture2D* m_stagingColor = nullptr;
	ID3D11Texture2D* m_stagingMV    = nullptr;
	ID3D11Texture2D* m_stagingDepth = nullptr;
	ID3D11Texture2D* m_stagingBias  = nullptr;
	uint32_t m_stagingW = 0, m_stagingH = 0;
	uint32_t m_stagingBiasW = 0, m_stagingBiasH = 0;
	DXGI_FORMAT m_stagingColorFmt  = DXGI_FORMAT_UNKNOWN;
	DXGI_FORMAT m_stagingMVFmt     = DXGI_FORMAT_UNKNOWN;
	DXGI_FORMAT m_stagingDepthFmt  = DXGI_FORMAT_UNKNOWN;

	// Cached dimensions for lazy re-creation
	uint32_t m_renderW[kHandleCount]  = {}, m_renderH[kHandleCount]  = {};
	uint32_t m_outputW[kHandleCount]  = {}, m_outputH[kHandleCount]  = {};
};

#endif // OC_HAS_DLSS
