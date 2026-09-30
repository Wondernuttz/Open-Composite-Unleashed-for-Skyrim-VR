#pragma once
#include <d3d11.h>
#include <memory>

// Some foliage shaders reject alpha only in the depth pass. Their later RGB
// shader cannot itself reveal that its material still needs per-pixel shading.
class VRSAlphaCoverageScope {
public:
    using ProtectionChanged = void (*)(ID3D11DeviceContext*);
    VRSAlphaCoverageScope();
    ~VRSAlphaCoverageScope();
    VRSAlphaCoverageScope(const VRSAlphaCoverageScope&) = delete;
    VRSAlphaCoverageScope& operator=(const VRSAlphaCoverageScope&) = delete;
    bool Arm(ID3D11DeviceContext* context, ID3D11Texture2D* canonicalSceneDepth,
        ProtectionChanged callback);
    void EndFrame();
    // bindingsReplaced is for ClearState, context-state swaps, and command-list
    // restoration, not an ordinary OM target setter (the broker observes that).
    void ShaderStateChanged(ID3D11DeviceContext* context, bool bindingsReplaced);
    bool ProtectsCurrentDraw(ID3D11DeviceContext* context) const;
    struct Statistics {
        unsigned depthDraws = 0, materials = 0, protectedDraws = 0, stateQueries = 0;
        unsigned resets = 0, unknownCommandLists = 0, untrackedDepthDraws = 0;
        unsigned ambiguousDraws = 0;
    };
    Statistics Stats() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
