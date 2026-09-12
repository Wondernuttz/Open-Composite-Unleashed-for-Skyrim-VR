#pragma once

#include <d3d11.h>
#include <memory>
#include "../Misc/FoveationRates.h"

// Cross-vendor radial-density masking for D3D11. Unlike hardware VRS, this
// backend reduces pixel-shader work by writing a sparse pattern into the
// caller-owned private depth buffer, then reconstructing skipped color samples.
// RDMRenderScope owns pass selection and resolves before downstream consumers.
class DensityMaskManager {
public:
	struct EyeRegion {
		int left = 0;
		int top = 0;
		int width = 0;
		int height = 0;
	};
	struct PatternSettings {
		float innerRadius = 0.60f;
		float midRadius = 0.80f;
		bool compatibilityMode = true;
		bool customEyeRates = false;
		ocu_foveation::RingRates rates;
	};
	void SetPatternSettings(const PatternSettings& settings) { patternSettings = settings; }

	DensityMaskManager();
	~DensityMaskManager();

	bool Initialize(ID3D11Device* device);
	bool PrepareStereoTarget(ID3D11Texture2D* target, int renderWidth, int renderHeight,
	    const EyeRegion& leftEye, const EyeRegion& rightEye);
	// Prepare before masking: allocation/format failures must leave the draw full-rate.
	bool PrepareMRTTargets(ID3D11Texture2D* const* targets, unsigned count,
	    int renderWidth, int renderHeight, const EyeRegion& leftEye, const EyeRegion& rightEye);
	bool ResolveMRTs(ID3D11ShaderResourceView* coverage, unsigned eyes);
	bool PrepareDepthGuide(ID3D11Texture2D* depth);
	void ClearDepthGuide();
	bool BeginDepthGuide(ID3D11DepthStencilView* depth, bool invalidate);
	void EndDepthGuide();
	ID3D11ShaderResourceView* DepthGuide() const { return guideSRV; }
	bool HasDepthGuideDraws() const { return guideDraws != 0; }
	void SetProjectionCenters(float leftX, float leftY, float rightX, float rightY);

	// The production caller supplies its private depth copy, never game depth.
	bool ApplyDepthMask(ID3D11DepthStencilView* dsv, float clearDepth,
        ID3D11RenderTargetView* coverage = nullptr, ID3D11ShaderResourceView* sceneDepth = nullptr);
    // Resolve only pixels actually masked in a private color pass. Game depth
    // is never an output; callers commit color before any downstream consumer.
    bool ResolveColor(ID3D11Texture2D* source, ID3D11ShaderResourceView* coverage, unsigned eyes);

	// Reconstructs the sparse stereo color target. The returned texture is owned
	// by this manager and remains valid until the target changes or Shutdown().
	ID3D11Texture2D* ReconstructStereo(ID3D11Texture2D* source, int submittedEye);

	void BeginFrame();
	void EndFrameMasking();
	void Shutdown();

	bool IsAvailable() const { return available; }
	bool IsArmed() const { return armed; }
	bool WasMaskAppliedThisFrame() const { return maskAppliedThisFrame; }
	bool WasInitializationAttempted() const { return initializationAttempted; }

private:
	struct MaskConstants {
		float depthOut;
		float radius[3];
		float invClusterResolution[2];
		float projectionCenter[2];
		float eyeOrigin[2];
		float compatibilityMode;
		float padding;
		unsigned ringRates[4]; // inner/mid/outer stable Rate IDs, custom enabled
	};

	struct ReconstructConstants {
		float eyeOrigin[2];
		float eyeSize[2];
		float projectionCenter[2];
		float projectionPadding[2];
		float radius[3];
		float compatibilityMode;
		unsigned ringRates[4];
	};
	static_assert(sizeof(MaskConstants) % 16 == 0,
	    "Density-mask constants must obey D3D11 constant-buffer alignment");
	static_assert(sizeof(ReconstructConstants) % 16 == 0,
	    "Density-mask reconstruction constants must obey D3D11 constant-buffer alignment");

	bool CreateShadersAndStates();
	bool CreateColorResources(const D3D11_TEXTURE2D_DESC& sourceDesc);
	bool ValidateGeometry(int width, int height,
	    const EyeRegion& leftEye, const EyeRegion& rightEye) const;
	bool DrawMaskForEye(ID3D11DepthStencilView* dsv, int eye, float clearDepth,
        ID3D11RenderTargetView* coverage, ID3D11ShaderResourceView* sceneDepth);
	bool DrawReconstructionForEye(int eye, ID3D11ShaderResourceView* coverage = nullptr);
	void ReleaseColorResources();
	void ReleasePackedResources();
	bool DrawPackedEye(int eye, bool gather, ID3D11ShaderResourceView* coverage);

	bool available = false;
	bool initializationAttempted = false;
	bool armed = false;
	bool maskAppliedThisFrame = false;
	bool reconstructedThisFrame = false;
	bool unsupportedTargetLogged = false;
	PatternSettings patternSettings;

	ID3D11Device* device = nullptr;
	ID3D11DeviceContext* context = nullptr;
	ID3D11Texture2D* sceneTarget = nullptr; // observed game resource; not owned
	int renderWidth = 0;
	int renderHeight = 0;
	EyeRegion eyeRegions[2];
	float projX[2] = { 0.5f, 0.5f };
	float projY[2] = { 0.5f, 0.5f };

	ID3D11VertexShader* maskVS = nullptr;
	ID3D11VertexShader* reconstructVS = nullptr;
	ID3D11PixelShader* maskPS = nullptr;
	ID3D11PixelShader* reconstructPS = nullptr;
	ID3D11Buffer* maskCB = nullptr;
	ID3D11Buffer* reconstructCB = nullptr;
	ID3D11DepthStencilState* maskDepthState = nullptr;
	ID3D11DepthStencilState* noDepthState = nullptr;
	ID3D11RasterizerState* rasterizerState = nullptr;
	ID3D11SamplerState* samplerState = nullptr;

	ID3D11Texture2D* sourceCopy = nullptr;
	ID3D11ShaderResourceView* sourceSRV = nullptr;
	ID3D11Texture2D* reconstructed = nullptr;
	ID3D11RenderTargetView* reconstructedRTV = nullptr;
	DXGI_FORMAT resourceFormat = DXGI_FORMAT_UNKNOWN;
	ID3D11Texture2D* packedTextures[8]{};
	ID3D11ShaderResourceView* packedSRVs[8]{};
	ID3D11RenderTargetView* packedRTVs[8]{};
	ID3D11ShaderResourceView* originalSRVs[8]{};
	ID3D11RenderTargetView* originalRTVs[8]{};
	ID3D11Texture2D* originalOwners[8]{}; // Retained by original SRVs/RTVs.
	DXGI_FORMAT packedFormats[8]{};
	ID3D11PixelShader* gatherShaders[256]{};
	ID3D11PixelShader* scatterShaders[256]{};
	unsigned packedWidth = 0, packedHeight = 0, packedCount = 0, packedMask = 0;
	struct GuidePass;
	std::unique_ptr<GuidePass> guidePass;
	ID3D11Texture2D* guideTexture = nullptr;
	ID3D11RenderTargetView* guideRTV = nullptr;
	ID3D11ShaderResourceView* guideSRV = nullptr;
	ID3D11PixelShader* guidePS = nullptr;
	ID3D11Buffer* guideCB = nullptr;
	ID3D11DepthStencilState* guideInvalidateDepth = nullptr;
	unsigned guideWidth = 0, guideHeight = 0, guideDraws = 0;
};
