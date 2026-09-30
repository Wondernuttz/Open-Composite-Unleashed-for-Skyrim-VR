#pragma once
#include <atomic>

namespace ocu_menu_policy {
// Published by the game-thread menu watcher; queried without UI locks on the
// render thread. Unknown startup state always leaves native rendering intact.
inline std::atomic<bool> separationAllowed{false};
inline void Publish(bool allowed) noexcept
{
    separationAllowed.store(allowed, std::memory_order_release);
}
inline bool Allowed() noexcept
{
    return separationAllowed.load(std::memory_order_acquire);
}
} // namespace ocu_menu_policy
