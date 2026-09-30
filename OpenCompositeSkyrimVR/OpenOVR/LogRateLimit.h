#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>

namespace OcuLogging {
// Per-call-site limiter. First failure is immediate, even with diagnostics off.
// CAS also bounds writes when multiple threads encounter the same failure.
class RateLimit {
    std::atomic<uint64_t> next{0};
public:
    bool Allow(uint64_t nowMs, uint64_t intervalMs) {
        auto due = next.load(std::memory_order_relaxed);
        while (nowMs >= due) {
            if (next.compare_exchange_weak(due, nowMs + intervalMs,
                    std::memory_order_relaxed)) return true;
        }
        return false;
    }
};
inline uint64_t NowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
}
