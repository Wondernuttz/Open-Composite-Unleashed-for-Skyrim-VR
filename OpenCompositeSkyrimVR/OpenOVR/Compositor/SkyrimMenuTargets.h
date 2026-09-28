#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace ocu_menu {
inline constexpr std::uint32_t Version = 1;
inline constexpr char AcquireExportName[] = "OCU_AcquireMenuTargets";

// Same-process render-thread query. Pass an empty, initialized Targets object.
// Success transfers one COM reference per non-null slot, including duplicates;
// the caller releases every returned slot and context. Failure leaves it intact.
// These are resource identities, not permission to capture an arbitrary draw.
struct Targets {
    std::uint32_t size = sizeof(Targets);
    std::uint32_t version = Version;
    ID3D11DeviceContext* context = nullptr;
    ID3D11Texture2D* sources[4]{};
    ID3D11Texture2D* destinations[4]{};
};

using AcquireFn = bool(__cdecl*)(Targets*) noexcept;
static_assert(std::is_standard_layout_v<Targets>);
static_assert(sizeof(void*) == 8);
static_assert(sizeof(Targets) == 80);
static_assert(offsetof(Targets, context) == 8);
static_assert(offsetof(Targets, sources) == 16);
static_assert(offsetof(Targets, destinations) == 48);
} // namespace ocu_menu
