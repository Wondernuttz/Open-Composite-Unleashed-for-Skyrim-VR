#pragma once

#ifdef _WIN32
#include <d3d11.h>
#include <cstdint>

namespace ocu_menu {
namespace detail {
// Menu scaling has a stricter contract than either VRS or RDM. Keep its
// permission on the shader object without changing either existing policy.
inline constexpr GUID ShaderScaleMetadataId{
    0x43e4f7df, 0x136d, 0x4d4c, {0xb4,0x41,0x63,0xd1,0x9f,0x31,0x2a,0x27}};
struct ShaderScaleMetadata {
    std::uint32_t version = 1;
    std::uint32_t allowed = 0;
};
inline bool StoreShaderScalePermission(ID3D11PixelShader* shader, bool allowed) noexcept
{
    const ShaderScaleMetadata metadata{1, allowed ? 1u : 0u};
    return shader && SUCCEEDED(shader->SetPrivateData(
        ShaderScaleMetadataId, sizeof(metadata), &metadata));
}
} // namespace detail

inline bool ShaderCanScale(ID3D11PixelShader* shader) noexcept
{
    detail::ShaderScaleMetadata metadata;
    UINT size = sizeof(metadata);
    return shader && SUCCEEDED(shader->GetPrivateData(
        detail::ShaderScaleMetadataId, &size, &metadata)) &&
        size == sizeof(metadata) && metadata.version == 1 && metadata.allowed == 1;
}
} // namespace ocu_menu
#endif
