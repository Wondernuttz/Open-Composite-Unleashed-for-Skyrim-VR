#pragma once
#if defined(SUPPORT_DX11)
#include <memory>
#include <openxr/openxr.h>
#include "../OpenOVR/Misc/CableTracking.h"
struct ID3D11Device;
class CableTrackingOverlay {
public:
    CableTrackingOverlay();
    ~CableTrackingOverlay();
    void ReferenceChange(XrTime time);
    const XrCompositionLayerBaseHeader* Update(XrSession session, XrSpace view,
        XrTime time, bool focused, bool canDraw, ID3D11Device* device, const ocu_cable::Settings& settings);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
#endif
