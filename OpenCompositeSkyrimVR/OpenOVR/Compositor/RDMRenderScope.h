#pragma once
#include <d3d11.h>
#include <memory>
#include "DensityMaskManager.h"

// A color-pass transaction. The game's depth/stencil resource is never masked.
// Pending sparse MRTs are resolved before another pass can consume them.
class RDMRenderScope {
public:
    RDMRenderScope();
    ~RDMRenderScope();
    RDMRenderScope(const RDMRenderScope&) = delete;
    RDMRenderScope& operator=(const RDMRenderScope&) = delete;
    bool Arm(ID3D11DeviceContext* context, ID3D11Texture2D* depth,
        ID3D11Texture2D* submitted, int width, int height,
        const DensityMaskManager::EyeRegion& left, const DensityMaskManager::EyeRegion& right,
        const DensityMaskManager::PatternSettings& pattern, const float centers[4]);
    void EndFrame();
    void Shutdown();
    void StateChanged(ID3D11DeviceContext* context);
    static void NotifyTargets(ID3D11DeviceContext* context);
    static void RegisterTargetObserver(void* renderTargets, void* renderTargetsAndUavs);

    // Optional VRS draw observer shares these native D3D detours. It is never
    // called for RDM rendering, including RDM's internal replay/resolve draws.
    // The immediate-context owner serializes registration and callbacks.
    struct DrawObserver {
        void* owner = nullptr;
        void (*beforeDraw)(void*, ID3D11DeviceContext*) = nullptr;
        void (*afterDraw)(void*, ID3D11DeviceContext*) = nullptr;
        void (*stateChanged)(void*, ID3D11DeviceContext*) = nullptr;
        void (*depthCleared)(void*, ID3D11DeviceContext*, ID3D11DepthStencilView*) = nullptr;
    };
    static bool RegisterDrawObserver(ID3D11DeviceContext* context, const DrawObserver* observer);
    static void RemoveDrawObserver(ID3D11DeviceContext* context);

    struct Statistics {
        unsigned draws = 0, maskedDraws = 0, batches = 0, resolves = 0;
        unsigned protectedDraws = 0, consumerBoundaries = 0, stateQueries = 0;
        unsigned guideDraws = 0, guideInvalidationDraws = 0, guideResets = 0;
        unsigned noGuideStates = 0;
    };
    Statistics Stats() const;

    // Entry points shared by the D3D11 detours and the compositor's existing
    // render-target observer. All internal rendering is reentrancy guarded.
    static RDMRenderScope* Active(ID3D11DeviceContext* context);
    bool BeforeDraw();
    void AfterDraw(bool masked);
    void BeforeRead(ID3D11Resource* resource);
    void BeforeWrite(ID3D11Resource* resource);
    bool BeginDepthGuide(bool invalidate);
    void EndDepthGuide();
    void InvalidateDepthGuide();
    void BeforeReads(UINT count, ID3D11ShaderResourceView* const* views);
    void BeforeCompute();
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
