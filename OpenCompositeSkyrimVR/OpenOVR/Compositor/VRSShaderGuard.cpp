#ifdef _WIN32
#include "VRSShaderGuard.h"
#include "ExactPixelShader.h"
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <MinHook.h>
#include <array>
#include <atomic>
#include <mutex>
#include <string_view>
#include <utility>

namespace ocu_vrs_guard {
namespace {
using Microsoft::WRL::ComPtr;
constexpr GUID shaderMetadataId{0x8ee350a6, 0xb547, 0x4869, {0xa3,0xc1,0x65,0x5a,0x0f,0x95,0x62,0x41}};
struct Metadata { std::uint32_t version = 2, reasons = Unclassified, colorOutputs = 0; };
// Keep the shared v2 metadata/API unchanged for RDM. Missing, old or unknown
// coarse metadata protects only hardware VRS; it cannot imply compatibility.
constexpr GUID coarseMetadataId{0xbc076f82, 0x91d0, 0x464d, {0x9b,0xce,0x97,0x11,0xa8,0xe9,0x03,0x71}};
// One atomic metadata write contains safety and material identity together.
// Missing material metadata must never leave a shader coarse-compatible.
struct CoarseMetadata {
    std::uint32_t version = 2, hazards = CoarseUnclassified;
    SampledTexture2DSlots sampledTexture2DSlots;
};
std::atomic<std::uint64_t> compatibleCount{0}, protectedCount{0}, unknownCount{0};
std::atomic<std::uint64_t> rasterDepthTextureLoadCount{0};
std::atomic<ID3D11DeviceContext*> watched{nullptr};
StateChanged changed = nullptr;
std::uint32_t shaderReasons = NoPixelShader, blendReasons = 0;
std::uint32_t coarseHazards = CoarseUnclassified;
int singleSampledTexture2D = -1;
SampledTexture2DSlots sampledTexture2DSlots;
unsigned commandDepth = 0;
ViewportState viewportState;

bool Starts(std::string_view value, std::string_view prefix) noexcept
{
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

template<class Visit> void Instructions(std::string_view text, Visit visit) noexcept
{
    while (!text.empty()) {
        const auto end = text.find('\n');
        auto line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        line = line.substr(0, line.find("//"));
        const auto start = line.find_first_not_of(" \t\r");
        if (start == std::string_view::npos) continue;
        line.remove_prefix(start);
        visit(line.substr(0, line.find_first_of(" \t\r\0", 0, 4)), line);
    }
}

bool WordCharacter(char c) noexcept
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_';
}

// Match complete register operands, including negation/abs and swizzles. A
// v1 declaration must never match an executable v10 read (or vice versa).
template<class Visit> void Registers(std::string_view line, char kind, Visit visit) noexcept
{
    for (std::size_t i = 0; i < line.size(); ++i) {
        if (line[i] != kind || (i && WordCharacter(line[i - 1]))) continue;
        auto end = i + 1;
        if (end == line.size() || line[end] < '0' || line[end] > '9') continue;
        unsigned reg = 0;
        while (end < line.size() && line[end] >= '0' && line[end] <= '9') {
            // Registers outside the D3D11 limits are never candidates.
            reg = reg < 1024 ? reg * 10 + unsigned(line[end] - '0') : 1024;
            ++end;
        }
        if (end < line.size() && WordCharacter(line[end])) continue;
        unsigned mask = 15; // An unmasked operand reads all components.
        if (end < line.size() && line[end] == '.') {
            mask = 0;
            for (++end; end < line.size(); ++end) {
                const auto component = std::string_view("xyzw").find(line[end]);
                if (component == std::string_view::npos) break;
                mask |= 1u << component;
            }
        }
        visit(reg, mask);
        i = end ? end - 1 : end;
    }
}

struct Classification {
    std::uint32_t reasons = Unclassified, coarse = CoarseUnclassified;
    SampledTexture2DSlots sampledTexture2DSlots;
};

int SingleSlot(const SampledTexture2DSlots& slots) noexcept
{
    int found = -1;
    for (unsigned slot = 0; slot < 128; ++slot) {
        if (!slots.Contains(slot)) continue;
        if (found != -1) return -1;
        found = int(slot);
    }
    return found;
}

CoarseMetadata ShaderCoarseMetadata(ID3D11PixelShader* shader) noexcept
{
    CoarseMetadata metadata;
    UINT size = sizeof(metadata);
    if (!shader || FAILED(shader->GetPrivateData(coarseMetadataId, &size, &metadata)) ||
        size != sizeof(metadata) || metadata.version != 2) return {};
    return metadata;
}

Classification ClassifyAssembly(std::string_view text) noexcept
{
    bool pixelProgram = false, readsRasterDepth = false, loadsTexture2D = false;
    std::array<bool, D3D11_PS_INPUT_REGISTER_COUNT> positionInputs{};
    std::array<bool, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> textures2D{};
    std::array<bool, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> sampledTextures2D{};
    std::uint32_t reasons = Compatible;
    Instructions(text, [&](std::string_view instruction, std::string_view line) {
        if (Starts(instruction, "ps_")) pixelProgram = true;
        if (Starts(instruction, "discard")) reasons |= Discard;
        if (Starts(instruction, "dcl_output") &&
            (line.find("oDepth") != std::string_view::npos || line.find("oMask") != std::string_view::npos))
            reasons |= DepthOrCoverage;
        if (Starts(instruction, "dcl_uav_")) reasons |= UnorderedAccess;
        if (Starts(instruction, "dcl_interface")) reasons |= ClassLinkage;
        if (instruction == "dcl_input_ps_siv") {
            const auto comma = line.rfind(',');
            auto semantic = comma == std::string_view::npos ? std::string_view{} : line.substr(comma + 1);
            const auto first = semantic.find_first_not_of(" \t");
            if (first != std::string_view::npos) semantic.remove_prefix(first);
            semantic = semantic.substr(0, semantic.find_first_of(" \t\r\0", 0, 4));
            if (semantic == "position") Registers(line, 'v', [&](unsigned reg, unsigned mask) {
                if (reg < positionInputs.size() && (mask & 4)) positionInputs[reg] = true;
            });
        }
        if (instruction == "dcl_resource_texture2d") Registers(line, 't', [&](unsigned reg, unsigned) {
            if (reg < textures2D.size()) textures2D[reg] = true;
        });
    });
    if (!pixelProgram) return {};
    Instructions(text, [&](std::string_view instruction, std::string_view line) {
        if (Starts(instruction, "dcl_") || Starts(instruction, "ps_")) return;
        Registers(line, 'v', [&](unsigned reg, unsigned mask) {
            if (reg < positionInputs.size() && positionInputs[reg] && (mask & 4)) readsRasterDepth = true;
        });
        if (instruction == "ld" || Starts(instruction, "ld_indexable(texture2d)"))
            Registers(line, 't', [&](unsigned reg, unsigned) {
                if (reg < textures2D.size() && textures2D[reg]) loadsTexture2D = true;
            });
        auto sampleOpcode = instruction.substr(0, instruction.find('('));
        constexpr std::string_view indexable = "_indexable";
        if (sampleOpcode.size() >= indexable.size() &&
            sampleOpcode.substr(sampleOpcode.size() - indexable.size()) == indexable)
            sampleOpcode.remove_suffix(indexable.size());
        if (sampleOpcode == "sample" || sampleOpcode == "sample_b" || sampleOpcode == "sample_l" ||
            sampleOpcode == "sample_d" || sampleOpcode == "sample_c" || sampleOpcode == "sample_c_lz")
            Registers(line, 't', [&](unsigned reg, unsigned) {
                if (reg < textures2D.size() && textures2D[reg]) sampledTextures2D[reg] = true;
            });
    });
    SampledTexture2DSlots materialSlots;
    for (unsigned slot = 0; slot < sampledTextures2D.size(); ++slot) {
        if (!sampledTextures2D[slot]) continue;
        materialSlots.words[slot / 32] |= 1u << (slot % 32);
    }
    // This conservative conjunction does not infer a dependency between the
    // depth input and load. It protects terrain equality/blend operations
    // without treating every screen-space load or position declaration as unsafe.
    return {reasons, readsRasterDepth && loadsTexture2D ? RasterDepthTextureLoad : CoarseCompatible, materialSlots};
}

Classification Classify(const void* bytecode, SIZE_T size) noexcept
{
    if (!bytecode || !size) return {};
    ComPtr<ID3DBlob> assembly;
    if (FAILED(D3DDisassemble(bytecode, size, D3D_DISASM_DISABLE_DEBUG_INFO,
            nullptr, &assembly)) || !assembly) return {};
    return ClassifyAssembly({static_cast<const char*>(assembly->GetBufferPointer()), assembly->GetBufferSize()});
}

// Each D3D implementation/wrapper gets its own trampoline. Do not assume the
// temporary device and the game's device expose the same method addresses.
template<class Function, class Handler> struct MethodHook;
template<class Handler, class Result, class... Args>
struct MethodHook<Result(STDMETHODCALLTYPE*)(Args...), Handler> {
    using Function = Result(STDMETHODCALLTYPE*)(Args...);
    static inline std::array<void*, 8> targets{};
    static inline std::array<Function, 8> originals{};
    static inline std::mutex mutex;
    template<std::size_t Index>
    static Result STDMETHODCALLTYPE Detour(Args... args)
    {
        return Handler::Call(originals[Index], args...);
    }
    template<std::size_t... Index>
    static constexpr auto Detours(std::index_sequence<Index...>)
    {
        return std::array<Function, sizeof...(Index)>{ &Detour<Index>... };
    }
    static bool Install(void* target)
    {
        if (!target) return false;
        std::lock_guard<std::mutex> lock(mutex);
        for (auto existing : targets) if (existing == target) return true;
        constexpr auto detours = Detours(std::make_index_sequence<8>{});
        for (std::size_t i = 0; i < targets.size(); ++i) {
            if (targets[i]) continue;
            if (MH_CreateHook(target, reinterpret_cast<void*>(detours[i]),
                    reinterpret_cast<void**>(&originals[i])) != MH_OK) return false;
            if (MH_EnableHook(target) != MH_OK) {
                MH_RemoveHook(target);
                originals[i] = nullptr;
                return false;
            }
            targets[i] = target;
            return true;
        }
        return false;
    }
};

bool InitializeHooks()
{
    const auto status = MH_Initialize();
    return status == MH_OK || status == MH_ERROR_ALREADY_INITIALIZED;
}

void Notify(ID3D11DeviceContext* context, bool targetsChanged)
{
    if (context == watched.load(std::memory_order_acquire) && changed)
        changed(context, targetsChanged);
}

std::uint32_t BlendReasons(ID3D11BlendState* state)
{
    D3D11_BLEND_DESC desc{};
    if (state) state->GetDesc(&desc);
    return desc.AlphaToCoverageEnable ? AlphaToCoverage : Compatible;
}

void Refresh(ID3D11DeviceContext* context)
{
    viewportState.count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    context->RSGetViewports(&viewportState.count, viewportState.values);
    ComPtr<ID3D11PixelShader> shader;
    context->PSGetShader(&shader, nullptr, nullptr);
    shaderReasons = ShaderReasons(shader.Get());
    const auto coarse = ShaderCoarseMetadata(shader.Get());
    coarseHazards = coarse.hazards;
    sampledTexture2DSlots = coarse.sampledTexture2DSlots;
    singleSampledTexture2D = SingleSlot(sampledTexture2DSlots);
    ComPtr<ID3D11BlendState> blend;
    context->OMGetBlendState(&blend, nullptr, nullptr);
    blendReasons = BlendReasons(blend.Get());
}

using CreatePS = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*, SIZE_T,
    ID3D11ClassLinkage*, ID3D11PixelShader**);
struct CreateHandler {
    static HRESULT Call(CreatePS original, ID3D11Device* device, const void* bytes,
        SIZE_T size, ID3D11ClassLinkage* linkage, ID3D11PixelShader** shader)
    {
        const auto result = original(device, bytes, size, linkage, shader);
        if (SUCCEEDED(result) && shader && *shader) {
            Metadata metadata;
            const auto classification = Classify(bytes, size);
            metadata.reasons = classification.reasons | (linkage ? ClassLinkage : Compatible);
            CoarseMetadata coarseMetadata;
            coarseMetadata.hazards = classification.coarse;
            coarseMetadata.sampledTexture2DSlots = classification.sampledTexture2DSlots;
            if (SUCCEEDED((*shader)->SetPrivateData(coarseMetadataId, sizeof(coarseMetadata), &coarseMetadata)) &&
                (coarseMetadata.hazards & RasterDepthTextureLoad)) ++rasterDepthTextureLoadCount;
            ComPtr<ID3D11ShaderReflection> reflection;
            if (SUCCEEDED(D3DReflect(bytes, size, IID_PPV_ARGS(&reflection)))) {
                D3D11_SHADER_DESC desc{};
                if (SUCCEEDED(reflection->GetDesc(&desc)))
                    for (UINT i = 0; i < desc.OutputParameters; ++i) {
                        D3D11_SIGNATURE_PARAMETER_DESC output{};
                        if (SUCCEEDED(reflection->GetOutputParameterDesc(i, &output)) &&
                            output.SystemValueType == D3D_NAME_TARGET && output.SemanticIndex < 8)
                            metadata.colorOutputs |= 1u << output.SemanticIndex;
                    }
            }
            const bool stored = SUCCEEDED((*shader)->SetPrivateData(shaderMetadataId, sizeof(metadata), &metadata));
            if (!stored || (metadata.reasons & Unclassified)) ++unknownCount;
            else if (metadata.reasons) ++protectedCount;
            else ++compatibleCount;
        }
        return result;
    }
};
using SetPS = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*, UINT);
struct ShaderHandler {
    static void Call(SetPS original, ID3D11DeviceContext* context, ID3D11PixelShader* shader,
        ID3D11ClassInstance* const* instances, UINT count)
    {
        original(context, shader, instances, count);
        if (context != watched.load(std::memory_order_acquire)) return;
        shaderReasons = ShaderReasons(shader) | (count ? ClassLinkage : Compatible);
        const auto coarse = ShaderCoarseMetadata(shader);
        coarseHazards = coarse.hazards;
        sampledTexture2DSlots = coarse.sampledTexture2DSlots;
        singleSampledTexture2D = SingleSlot(sampledTexture2DSlots);
        Notify(context, false);
    }
};
using SetBlend = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11BlendState*, const FLOAT*, UINT);
struct BlendHandler {
    static void Call(SetBlend original, ID3D11DeviceContext* context, ID3D11BlendState* state,
        const FLOAT* factor, UINT mask)
    {
        original(context, state, factor, mask);
        if (context != watched.load(std::memory_order_acquire)) return;
        blendReasons = BlendReasons(state);
        Notify(context, false);
    }
};
using Clear = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*);
using SetViewports = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
struct ViewportHandler {
    static void Call(SetViewports original, ID3D11DeviceContext* context, UINT count,
        const D3D11_VIEWPORT* viewports)
    {
        original(context, count, viewports);
        if (context != watched.load(std::memory_order_acquire)) return;
        // Invalid API arguments are ignored by D3D; refresh actual state in
        // that case rather than cache an update the runtime did not accept.
        if (count > D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE || (count && !viewports)) {
            Refresh(context);
        } else {
            viewportState.count = count;
            for (UINT i = 0; i < count; ++i) viewportState.values[i] = viewports[i];
        }
        Notify(context, false);
    }
};
struct ClearHandler {
    static void Call(Clear original, ID3D11DeviceContext* context)
    {
        original(context);
        if (context != watched.load(std::memory_order_acquire)) return;
        shaderReasons = NoPixelShader;
        coarseHazards = CoarseUnclassified;
        singleSampledTexture2D = -1;
        sampledTexture2DSlots = {};
        blendReasons = 0;
        viewportState.count = 0;
        Notify(context, true);
    }
};
using Execute = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11CommandList*, BOOL);
struct ExecuteHandler {
    static void Call(Execute original, ID3D11DeviceContext* context, ID3D11CommandList* list, BOOL restore)
    {
        if (context != watched.load(std::memory_order_acquire)) {
            original(context, list, restore);
            return;
        }
        // Recorded shader changes do not pass through immediate-context hooks
        // during playback. Protect this command list, then recover immediately.
        ++commandDepth;
        Notify(context, false);
        original(context, list, restore);
        --commandDepth;
        Refresh(context);
        Notify(context, true);
    }
};
using Swap = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext1*, ID3DDeviceContextState*, ID3DDeviceContextState**);
struct SwapHandler {
    static void Call(Swap original, ID3D11DeviceContext1* context, ID3DDeviceContextState* state,
        ID3DDeviceContextState** previous)
    {
        original(context, state, previous);
        if (context != watched.load(std::memory_order_acquire)) return;
        Refresh(context);
        Notify(context, true);
    }
};
}

std::uint32_t ClassifyBytecode(const void* bytecode, SIZE_T size) noexcept
{
    return Classify(bytecode, size).reasons;
}

std::uint32_t ClassifyCoarseShadingBytecode(const void* bytecode, SIZE_T size) noexcept
{
    return Classify(bytecode, size).coarse;
}

#ifdef OCU_VRS_GUARD_SELF_TEST
std::uint32_t ClassifyCoarseShadingAssemblyForTest(std::string_view assembly) noexcept
{
    return ClassifyAssembly(assembly).coarse;
}
#endif

std::uint32_t ShaderReasons(ID3D11PixelShader* shader) noexcept
{
    if (!shader) return NoPixelShader;
    const auto exact = ocu_exact_pixels::IsMarked(shader) ? ExactPixels : Compatible;
    Metadata metadata;
    UINT size = sizeof(metadata);
    if (FAILED(shader->GetPrivateData(shaderMetadataId, &size, &metadata)) ||
        size != sizeof(metadata) || metadata.version != 2) return Unclassified | exact;
    return metadata.reasons | exact;
}

std::uint32_t ShaderColorOutputs(ID3D11PixelShader* shader) noexcept
{
    if (!shader) return 0;
    Metadata metadata; UINT size = sizeof(metadata);
    if (FAILED(shader->GetPrivateData(shaderMetadataId, &size, &metadata)) ||
        size != sizeof(metadata) || metadata.version != 2) return 0;
    return metadata.colorOutputs;
}

std::uint32_t ShaderCoarseHazards(ID3D11PixelShader* shader) noexcept
{
    return ShaderCoarseMetadata(shader).hazards;
}

int ShaderSingleSampledTexture2D(ID3D11PixelShader* shader) noexcept
{
    return SingleSlot(ShaderCoarseMetadata(shader).sampledTexture2DSlots);
}

SampledTexture2DSlots ShaderSampledTexture2DSlots(ID3D11PixelShader* shader) noexcept
{
    return ShaderCoarseMetadata(shader).sampledTexture2DSlots;
}

bool IsNvidiaDevice(ID3D11Device* device) noexcept
{
    if (!device) return false;
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    return SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) &&
        SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) &&
        SUCCEEDED(adapter->GetDesc(&desc)) && desc.VendorId == 0x10de;
}

bool InstallShaderCapture(ID3D11Device* device)
{
    if (!device || !InitializeHooks()) return false;
    auto table = *reinterpret_cast<void***>(device);
    return MethodHook<CreatePS, CreateHandler>::Install(table[15]);
}

bool WatchContext(ID3D11DeviceContext* context, StateChanged callback)
{
    if (!context || context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE || !callback || !InitializeHooks())
        return false;
    auto table = *reinterpret_cast<void***>(context);
    if (!MethodHook<SetPS, ShaderHandler>::Install(table[9]) ||
        !MethodHook<SetBlend, BlendHandler>::Install(table[35]) ||
        !MethodHook<SetViewports, ViewportHandler>::Install(table[44]) ||
        !MethodHook<Clear, ClearHandler>::Install(table[110]) ||
        !MethodHook<Execute, ExecuteHandler>::Install(table[58])) return false;
    ComPtr<ID3D11DeviceContext1> context1;
    if (SUCCEEDED(context->QueryInterface(IID_PPV_ARGS(&context1)))) {
        auto table1 = *reinterpret_cast<void***>(context1.Get());
        if (!MethodHook<Swap, SwapHandler>::Install(table1[131])) return false;
    }
    watched.store(nullptr, std::memory_order_release);
    changed = callback;
    commandDepth = 0;
    Refresh(context);
    watched.store(context, std::memory_order_release);
    return true;
}

void UnwatchContext(ID3D11DeviceContext* context)
{
    if (context && watched.load(std::memory_order_acquire) != context) return;
    watched.store(nullptr, std::memory_order_release);
    changed = nullptr;
    shaderReasons = NoPixelShader;
    coarseHazards = CoarseUnclassified;
    singleSampledTexture2D = -1;
    sampledTexture2DSlots = {};
    blendReasons = commandDepth = 0;
    viewportState.count = 0;
}

const ViewportState* CurrentViewports(ID3D11DeviceContext* context) noexcept
{
    return context == watched.load(std::memory_order_acquire) ? &viewportState : nullptr;
}

std::uint32_t CurrentReasons(ID3D11DeviceContext* context) noexcept
{
    if (context != watched.load(std::memory_order_acquire)) return Unclassified;
    return shaderReasons | blendReasons | (commandDepth ? CommandList : Compatible);
}

std::uint32_t CurrentCoarseHazards(ID3D11DeviceContext* context) noexcept
{
    return context == watched.load(std::memory_order_acquire) ? coarseHazards : CoarseUnclassified;
}

int CurrentSingleSampledTexture2D(ID3D11DeviceContext* context) noexcept
{
    return context == watched.load(std::memory_order_acquire) ? singleSampledTexture2D : -1;
}

SampledTexture2DSlots CurrentSampledTexture2DSlots(ID3D11DeviceContext* context) noexcept
{
    return context == watched.load(std::memory_order_acquire) ? sampledTexture2DSlots : SampledTexture2DSlots{};
}

CaptureCounts Counts() noexcept
{
    return { compatibleCount.load(), protectedCount.load(), unknownCount.load(), rasterDepthTextureLoadCount.load() };
}
}
#endif
