#pragma once

#if defined(SUPPORT_DX11)
#include <openxr/openxr.h>
#include "../OpenOVR/Misc/EffectFoveationAPI.h"
#include <memory>

struct ID3D11Device;

class FoveationDebugOverlay {
public:
    FoveationDebugOverlay();
    ~FoveationDebugOverlay();
    FoveationDebugOverlay(const FoveationDebugOverlay&) = delete;
    FoveationDebugOverlay& operator=(const FoveationDebugOverlay&) = delete;
    bool Update(XrSession session, ID3D11Device* device,
        const ocu_effect_foveation::Snapshot& profile, float horizontalScale = 1.0f,
        bool drawRings = true, bool peripheralMask = false, float maskRadius = 1.0f,
        bool middleBlackout = false, bool outerBlackout = false);
    static constexpr uint32_t LayerCount = 2;
    bool PositionOverScene(const XrCompositionLayerProjection& scene);
    const XrCompositionLayerBaseHeader* Layer(uint32_t eye) const;
    const char* Status() const;
    void Reset();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
#endif
