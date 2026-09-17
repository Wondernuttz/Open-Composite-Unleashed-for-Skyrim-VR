#pragma once

#ifdef _WIN32
#include <d3d11.h>
#include <cstdint>

namespace ocu_vrs_guard {
enum Reason : std::uint32_t {
    Compatible = 0,
    Unclassified = 1u << 0,
    Discard = 1u << 1,
    DepthOrCoverage = 1u << 2,
    UnorderedAccess = 1u << 3,
    ClassLinkage = 1u << 4,
    AlphaToCoverage = 1u << 5,
    CommandList = 1u << 6,
    NoPixelShader = 1u << 7,
    ExactPixels = 1u << 8,
};

// Hardware coarse shading changes the SV_Position interpolation location.
// RDM retains fine-pixel interpolation and must not use these extra hazards.
enum CoarseShadingHazard : std::uint32_t {
    CoarseCompatible = 0,
    CoarseUnclassified = 1u << 0,
    RasterDepthTextureLoad = 1u << 1,
};

struct SampledTexture2DSlots {
    std::uint32_t words[4]{};
    bool Contains(UINT slot) const noexcept {
        return slot < 128 && (words[slot / 32] & (1u << (slot % 32)));
    }
};

// Capture bytecode at creation, including stripped shaders. Metadata belongs
// to the shader COM object, so shader destruction/address reuse cannot leave
// stale pointer-keyed classifications. Install before game shader loading.
bool IsNvidiaDevice(ID3D11Device* device) noexcept;
bool InstallShaderCapture(ID3D11Device* device);
std::uint32_t ClassifyBytecode(const void* bytecode, SIZE_T size) noexcept;
std::uint32_t ClassifyCoarseShadingBytecode(const void* bytecode, SIZE_T size) noexcept;
std::uint32_t ShaderReasons(ID3D11PixelShader* shader) noexcept;
std::uint32_t ShaderCoarseHazards(ID3D11PixelShader* shader) noexcept;
// Actual sample instructions using exactly one Texture2D resource. Returns
// -1 for absent/ambiguous metadata or zero/multiple sampled Texture2D slots.
int ShaderSingleSampledTexture2D(ID3D11PixelShader* shader) noexcept;
SampledTexture2DSlots ShaderSampledTexture2DSlots(ID3D11PixelShader* shader) noexcept;
std::uint32_t ShaderColorOutputs(ID3D11PixelShader* shader) noexcept;

using StateChanged = void (*)(ID3D11DeviceContext*, bool renderTargetsChanged);
using BeforeContextMutation = void (*)(ID3D11DeviceContext*);
// The owner serializes this immediate context, as required by D3D11. Deferred
// contexts and unrelated devices never update its current shading decision.
// Optional owner flush before ClearState or a context-state capture/swap.
// Hardware VRS has no private render-target state and does not need this.
bool WatchContext(ID3D11DeviceContext* context, StateChanged callback,
    BeforeContextMutation beforeMutation = nullptr);
void UnwatchContext(ID3D11DeviceContext* context = nullptr);
std::uint32_t CurrentReasons(ID3D11DeviceContext* context) noexcept;
std::uint32_t CurrentCoarseHazards(ID3D11DeviceContext* context) noexcept;
int CurrentSingleSampledTexture2D(ID3D11DeviceContext* context) noexcept;
SampledTexture2DSlots CurrentSampledTexture2DSlots(ID3D11DeviceContext* context) noexcept;
struct ViewportState {
    UINT count = 0;
    D3D11_VIEWPORT values[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
};
// Cached by RSSetViewports/ClearState/command-list/state-swap observation;
// avoids an RSGetViewports driver query for every pixel-shader change.
const ViewportState* CurrentViewports(ID3D11DeviceContext* context) noexcept;

struct CaptureCounts {
    std::uint64_t compatible, protectedShaders, unclassified;
    std::uint64_t rasterDepthTextureLoad;
};
CaptureCounts Counts() noexcept;
}
#endif
