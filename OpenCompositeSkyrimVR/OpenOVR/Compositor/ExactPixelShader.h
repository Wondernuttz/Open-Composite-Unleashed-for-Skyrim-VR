#pragma once
#ifdef _WIN32
#include <d3d11.h>
#include <cstdint>

// COM-object metadata shared by the SKSE producer and the OCU renderer DLL.
// This is separate from versioned shader classifications; tagging an internal
// shader after CreatePixelShader must not overwrite its captured metadata.
namespace ocu_exact_pixels {
inline constexpr GUID MetadataId{0x09eaf2d7, 0x2adf, 0x41a1, {0x91,0xb5,0x4b,0x11,0xa3,0xf5,0x6c,0x62}};
inline bool Mark(ID3D11PixelShader* shader) noexcept
{
    constexpr std::uint32_t version = 1;
    return shader && SUCCEEDED(shader->SetPrivateData(MetadataId, sizeof(version), &version));
}
inline bool IsMarked(ID3D11PixelShader* shader) noexcept
{
    if (!shader) return false;
    // Presence protects even an unfamiliar payload/version. Ignoring a future
    // exact-pixel request could silently coarsen a correctness-critical shader.
    UINT size = 0;
    const auto result = shader->GetPrivateData(MetadataId, &size, nullptr);
    return (SUCCEEDED(result) || result == DXGI_ERROR_MORE_DATA) && size > 0;
}
}
#endif
