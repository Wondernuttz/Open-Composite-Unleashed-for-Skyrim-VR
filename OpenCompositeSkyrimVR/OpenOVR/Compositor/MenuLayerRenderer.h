#pragma once

#include <cstdint>
#include <d3d11_1.h>
#include <wrl/client.h>

namespace ocu_menu { struct Targets; }

// Captures recognized UI composition draws, not the construction of their source
// textures. The caller must keep scene foveation disarmed during these draws and
// suppress the original draw only when Capture returns true.
class MenuLayerRenderer {
public:
    struct Statistics {
        std::uint32_t draws = 0, candidates = 0, captures = 0, restores = 0;
        const char* lastRejection = "none";
    };
    bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);
    bool BeginFrame(std::uint64_t frame, UINT renderWidth, UINT renderHeight,
        UINT outputWidth, UINT outputHeight, const ocu_menu::Targets& targets);
    bool Capture(ID3D11DeviceContext* context, void (*nativeDraw)(void*), void* drawToken);
    bool Composite(ID3D11DeviceContext* context, ID3D11RenderTargetView* target,
        int eye, const D3D11_VIEWPORT& viewport, bool flipY = false,
        const float* eyeUVBounds = nullptr, bool restoreNativeAlpha = false);
    bool CanComposite(ID3D11DeviceContext* context, ID3D11RenderTargetView* target,
        const D3D11_VIEWPORT& viewport) const;
    // Restore accumulated UI before an unsupported native draw or before the
    // caller processes a frame whose final presentation contract changed.
    bool RestoreToTarget(ID3D11DeviceContext* context, ID3D11RenderTargetView* target);
    // Invoke only during game rendering, before the native copy executes.
    // Whole compatible copies retain menu-free lineage; other copies restore
    // the UI first so the native operation receives complete source pixels.
    void BeforeCopy(ID3D11DeviceContext* context, ID3D11Resource* destination,
        ID3D11Resource* source, bool wholeResource);
    // Also usable for ClearView, DiscardView/Resource and resource updates: the
    // native operation runs after deferred UI has been restored to its lineage.
    // Null resource flushes the complete batch for unclassified GPU work.
    void BeforeClear(ID3D11DeviceContext* context, ID3D11Resource* resource);
    void ResetFrame(bool releaseLayer = false);
    bool HasLayer() const { return frame_ != 0 && captured_; }
    std::uint64_t Frame() const { return frame_; }
    const Statistics& Stats() const { return statistics_; }

private:
    bool ReserveLayer(UINT width, UINT height);
    bool DrawLayer(ID3D11DeviceContext* context, ID3D11RenderTargetView* target,
        int eye, const D3D11_VIEWPORT& viewport, bool flipY, const float* eyeUVBounds = nullptr,
        bool restoreAlpha = false);
    bool RestoreCaptured(ID3D11DeviceContext* context, ID3D11RenderTargetView* target);
    bool RestoreAllCaptured(ID3D11DeviceContext* context);
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> compositionContext_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> straightCaptureBlend_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> premultipliedBlend_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> compositeBlend_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> layer_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> layerRTV_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> layerSRV_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> originalTarget_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> copiedTargets_[4];
    Microsoft::WRL::ComPtr<ID3D11Texture2D> sources_[4], destinations_[4];
    UINT layerWidth_ = 0, layerHeight_ = 0, renderWidth_ = 0, renderHeight_ = 0;
    std::uint64_t frame_ = 0, lastBegunFrame_ = 0;
    unsigned compositedEyes_ = 0;
    bool captured_ = false, sealed_ = false, busy_ = false;
    bool nativeAlphaOver_ = false;
    Statistics statistics_;
};
