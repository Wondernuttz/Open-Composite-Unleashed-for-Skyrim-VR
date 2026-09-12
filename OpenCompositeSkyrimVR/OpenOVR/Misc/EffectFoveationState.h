#pragma once

#include "EffectFoveationAPI.h"
#include <mutex>

namespace ocu_effect_foveation {

// Internal producer/store. All lock scopes contain only a small POD copy; no
// runtime, driver, callback or GPU call executes while holding this mutex.
class State {
public:
    void BeginFrame(std::int64_t publicationQpc, std::int64_t qpcFrequency);
    bool Publish(const Snapshot& value);
    void Clear(std::int64_t publicationQpc);
    Result Query(std::uint32_t requestedVersion, std::uint32_t outputBytes,
        Snapshot* output) const;
    // Diagnostic presentation only; never exposed through the renderer API.
    // Fixed profiles last until the next frame/publication decision; tracked
    // profiles also expire by age so old gaze is not displayed as current.
    Snapshot ReadForPresentation(std::int64_t now) const;

private:
    mutable std::mutex mutex;
    Snapshot snapshot{};
    Snapshot presentation{};
    bool frameOpen = false;
};

State& GetState();
std::int64_t ClockTicks();
std::int64_t ClockFrequency();
void BeginFrame();
void Clear();

} // namespace ocu_effect_foveation
