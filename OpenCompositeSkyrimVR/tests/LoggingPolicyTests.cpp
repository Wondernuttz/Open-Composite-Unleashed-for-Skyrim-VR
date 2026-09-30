#include "OpenOVR/logging.h"
#include <cstdio>
#include <thread>
#include <vector>
static bool detailed = false;
static int messages = 0, evaluations = 0;
bool oovr_debug_logging_enabled() { return detailed; }
void oovr_log_raw(const char*, long, const char*, const char*) { ++messages; }
void oovr_log_raw_format(const char*, long, const char*, const char*, ...) { ++messages; }
int main() {
    OOVR_DEBUG_LOG("hidden");
    OOVR_DEBUG_LOGF("hidden %d", ++evaluations);
    if (messages || evaluations) return 1;
    OOVR_LOG("basic startup/errors retained");
    if (messages != 1) return 2;
    detailed = true;
    OOVR_DEBUG_LOG("enabled");
    OOVR_DEBUG_LOGF("enabled %d", ++evaluations);
    if (messages != 3 || evaluations != 1) return 3;
    detailed = false;
    auto fault = [] { OOVR_LOG_LIMITEDF(5000, "failure remains visible %d", ++evaluations); };
    fault(); fault();
    if (messages != 4 || evaluations != 2) return 4;
    OcuLogging::RateLimit limit;
    if (!limit.Allow(0, 5000) || limit.Allow(1, 5000) || limit.Allow(4999, 5000)
        || !limit.Allow(5000, 5000) || limit.Allow(5000, 5000)) return 5;
    OcuLogging::RateLimit concurrent;
    std::atomic<int> accepted{0};
    std::vector<std::thread> threads;
    for (int i=0; i<16; ++i) threads.emplace_back([&] { if(concurrent.Allow(100,5000)) ++accepted; });
    for(auto& thread:threads) thread.join();
    if(accepted != 1) return 6;
    std::puts("PASS: failures visible with diagnostics OFF; repeats skip argument work; exact interval and 16-thread limiter.");
    std::puts("PASS: detailed logging OFF skips calls and argument work; ON emits; basic logs retained.");
}
