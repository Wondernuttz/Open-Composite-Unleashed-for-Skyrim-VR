#pragma once

#include <d3d11_1.h>
#include <wrl/client.h>
#include "../Misc/FoveationBlackout.h"

// Final presentation fill for a frame whose scene cull geometry was latched
// before rendering. This does not establish validity for earlier effect passes.
class FoveationBlackoutRenderer {
public:
    // Prepare every resource before allowing a scene frame to be culled.
    bool Initialize(ID3D11Device* device);

    // Call on the device's immediate context after scene culling is disarmed,
    // after all image filters and before releasing the acquired image. Visible
    // pixels and all incoming pipeline bindings are preserved. Disabled frames
    // are successful no-ops; invalid input fails before writing the target.
    bool Apply(ID3D11DeviceContext* context, ID3D11RenderTargetView* target,
        const ocu_foveation::BlackoutFrame& frame, int eye,
        const D3D11_VIEWPORT& viewport, bool flipY = false);

private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context1_;
    Microsoft::WRL::ComPtr<ID3DDeviceContextState> state_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> depth_;
    Microsoft::WRL::ComPtr<ID3D11BlendState> blend_;
};
