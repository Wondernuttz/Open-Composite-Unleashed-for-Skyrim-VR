#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>

namespace ocu_index_grip {
// Starting points for opt-in tuning, not controller calibration values.
struct Thresholds {
    float grab = 0.50f;
    float release = 0.25f;
};
inline Thresholds Normalize(float grab, float release)
{
    if (!std::isfinite(grab)) grab = 0.50f;
    if (!std::isfinite(release)) release = 0.25f;
    grab = std::clamp(grab, 0.02f, 1.0f);
    return { grab, std::clamp(release, 0.01f, (std::max)(0.01f, grab - 0.01f)) };
}
class HoldState {
    mutable std::mutex mutex;
    bool held = false, pending = false, sampled = false;
    uint64_t generation = 0, releaseSinceMs = 0, lastSampleMs = 0;
    void Clear() { held = pending = sampled = false; }
public:
    // A single missed report should not end a HIGGS grab. Confirm release over
    // elapsed time, not polling count: several mods may read one hand per frame.
    static constexpr uint64_t releaseConfirmMs = 20;
    static constexpr uint64_t staleGapMs = 100;
    struct Decision {
        bool held;
        bool report;
        const char* reason;
    };
    void Reset() { std::lock_guard<std::mutex> lock(mutex); ++generation; Clear(); }
    uint64_t Generation() const { std::lock_guard<std::mutex> lock(mutex); return generation; }
    Decision Update(bool eligible, bool active, float value, bool digital,
        bool custom, Thresholds thresholds, uint64_t nowMs, uint64_t sampleGeneration)
    {
        std::lock_guard<std::mutex> lock(mutex);
        // A reset during the OpenXR reads invalidates those reads. Never revive
        // an old session's hold or alter a newer sample's state.
        if (sampleGeneration != generation) return {false, false, "stale-generation"};
        const bool wasHeld = held;
        if (!eligible) { Clear(); return {false, wasHeld, "unfocused-or-detached"}; }
        // Multiple consumers can finish their reads out of order. An older
        // timestamp must not clear a newer hold or restart its release timer.
        if (sampled && nowMs < lastSampleMs) return {held, false, "out-of-order"};
        const bool stale = sampled && nowMs-lastSampleMs > staleGapMs;
        if (stale) Clear();
        sampled = true;
        lastSampleMs = nowMs;
        // Expire before evaluating recovery as well as while input stays low.
        // Otherwise a late relaxed sample could silently extend the grace.
        const bool expired = pending && nowMs-releaseSinceMs >= releaseConfirmMs;
        if (expired) held = pending = false;
        const bool valid = active && (!custom || std::isfinite(value));
        // Custom mode owns hysteresis even when the runtime supports Valve's
        // threshold extension. A transient inactive runtime boolean must not
        // force a second grab crossing after the analogue input recovers.
        const bool down = valid && (custom
            ? value >= (held ? thresholds.release : thresholds.grab) : digital);
        if (down) {
            const bool recovered = pending;
            held = true;
            pending = false;
            return {true, !wasHeld || recovered, recovered ? "dropout-recovered" : "grab"};
        }
        if (!held) return {false, wasHeld, stale ? "stale-gap" : expired
            ? (!valid ? "input-unavailable" : "released") : "idle"};
        if (!pending) {
            pending = true;
            releaseSinceMs = nowMs;
            return {true, true, "confirming-release"};
        }
        return {true, false, "confirming-release"};
    }
};
}
