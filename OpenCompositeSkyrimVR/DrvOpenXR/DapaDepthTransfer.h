#pragma once
#include <d3d11.h>
#include <wrl/client.h>

// Assemble ordinary R32 textures first. D32 destinations must receive a whole
// subresource, never the old pair of partial, offset eye copies.
class DapaDepthTransfer {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> atlas;
public:
    void Reset() { atlas.Reset(); }
    bool Copy(ID3D11DeviceContext* context, ID3D11Texture2D* target,
        ID3D11Texture2D* left, ID3D11Texture2D* right)
    {
        if (!context || !target || !left || !right) return false;
        D3D11_TEXTURE2D_DESC dst{}, a{}, b{};
        target->GetDesc(&dst); left->GetDesc(&a); right->GetDesc(&b);
        auto flat = [](const D3D11_TEXTURE2D_DESC& d) {
            return d.Width && d.Height && d.ArraySize == 1 && d.MipLevels == 1 && d.SampleDesc.Count == 1;
        };
        if (!flat(dst) || !flat(a) || !flat(b) || a.Format != DXGI_FORMAT_R32_FLOAT ||
            b.Format != DXGI_FORMAT_R32_FLOAT || a.Width != b.Width || a.Height != b.Height ||
            dst.Width != a.Width * 2 || dst.Height != a.Height ||
            (dst.Format != DXGI_FORMAT_D32_FLOAT && dst.Format != DXGI_FORMAT_R32_FLOAT)) return false;
        Microsoft::WRL::ComPtr<ID3D11Device> device, targetDevice, leftDevice, rightDevice;
        context->GetDevice(&device); target->GetDevice(&targetDevice);
        left->GetDevice(&leftDevice); right->GetDevice(&rightDevice);
        if (device != targetDevice || device != leftDevice || device != rightDevice) return false;
        // Preserve the two-copy path for ordinary color-format depth textures.
        if (dst.Format == DXGI_FORMAT_R32_FLOAT && !(dst.BindFlags & D3D11_BIND_DEPTH_STENCIL)) {
            context->CopySubresourceRegion(target, 0, 0, 0, 0, left, 0, nullptr);
            context->CopySubresourceRegion(target, 0, a.Width, 0, 0, right, 0, nullptr);
            return true;
        }
        if (atlas) {
            D3D11_TEXTURE2D_DESC old{}; atlas->GetDesc(&old);
            Microsoft::WRL::ComPtr<ID3D11Device> oldDevice; atlas->GetDevice(&oldDevice);
            if (old.Width != dst.Width || old.Height != dst.Height || oldDevice != device) atlas.Reset();
        }
        if (!atlas) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = dst.Width; desc.Height = dst.Height;
            desc.ArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
            desc.Format = DXGI_FORMAT_R32_FLOAT;
            if (FAILED(device->CreateTexture2D(&desc, nullptr, &atlas))) return false;
        }
        context->CopySubresourceRegion(atlas.Get(), 0, 0, 0, 0, left, 0, nullptr);
        context->CopySubresourceRegion(atlas.Get(), 0, a.Width, 0, 0, right, 0, nullptr);
        context->CopyResource(target, atlas.Get());
        return true;
    }
};
