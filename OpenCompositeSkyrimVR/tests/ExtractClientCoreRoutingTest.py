"""Generate a test around the exact shipping factory body; never load SteamVR."""
from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text(encoding='utf-8-sig')
start = source.index('VR_INTERFACE void* VRClientCoreFactory(')
body = source.index('{', start)
depth = 1
end = body + 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
factory = source[start:end]
route_state = source[source.index('static alternativeCoreFactory_t alternativeCoreFactory = nullptr;'):
                     source.index('OC_NORETURN void ERR(')]
init_guard_start = source.index('if (!SelectOCUClientCoreForInit()) {')
init_guard_end = source.index('\n\t}', init_guard_start) + len('\n\t}')
init_guard = source[init_guard_start:init_guard_end]
preamble = r'''
#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <cstdio>
#include <thread>
#include <vector>
using std::string;
#define VR_INTERFACE
#define OOVR_LOGF(...) ((void)0)
#define OOVR_LOG(...) ((void)0)
#define OOVR_MESSAGE(...) ((void)0)
#define OOVR_FALSE_ABORT(ok) if (!(ok)) throw std::runtime_error("unexpected abort")
[[noreturn]] void ERR(string error) { throw std::runtime_error(error); }
constexpr int VRInitError_None = 0;
constexpr int VRInitError_Init_AlreadyRunning = 143;
using alternativeCoreFactory_t = void*(*)(const char*, int*);
'''
preamble += route_state
preamble += r'''
bool running = false;
std::atomic<int> preferenceReads{0}, alternativeLoads{0}, alternativeCalls{0};
bool preferOCU = true;
bool racePreferences = false;
std::promise<void> firstPreferenceEntered;
std::promise<void> secondFactoryDone;
std::shared_future<void> secondFactoryCompletion = secondFactoryDone.get_future().share();
int alternateObject;
bool reenterOnLoad = false;
bool reenterOnCall = false;
void* VRClientCoreFactory(const char*, int*);
int InitRoutingOnly(int* peError);
struct BaseClientCore { static bool CheckAppEnabled() {
    const int read = ++preferenceReads;
    if (racePreferences && read == 1) {
        firstPreferenceEntered.set_value();
        // An earlier preference read can finish after another caller selected
        // OCU. Serialization must give this process just one runtime choice.
        secondFactoryCompletion.wait_for(std::chrono::milliseconds(200));
        return false;
    }
    return preferOCU;
} };
struct CVRClientCore_002 {};
struct CVRClientCore_003 {};
namespace IVRClientCore_002 { const string IVRClientCore_Version = "IVRClientCore_002"; }
namespace IVRClientCore_003 { const string IVRClientCore_Version = "IVRClientCore_003"; }
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void* FakeSteamVR(const char*, int* status) {
    ++alternativeCalls;
    if (reenterOnCall) {
        reenterOnCall = false;
        int recursiveStatus = -1;
        Check(VRClientCoreFactory("IVRClientCore_003", &recursiveStatus) == &alternateObject && recursiveStatus == 0,
              "native factory reentry failed after route selection");
    }
    *status = 0; return &alternateObject;
}
alternativeCoreFactory_t PlatformGetAlternativeCoreFactory() {
    ++alternativeLoads;
    if (reenterOnLoad) {
        int recursiveStatus = -1;
        Check(!VRClientCoreFactory("IVRClientCore_003", &recursiveStatus) && recursiveStatus == VRInitError_Init_AlreadyRunning,
              "recursive native load was not rejected with a defined error");
        recursiveStatus = -1;
        Check(InitRoutingOnly(&recursiveStatus) == 0 && recursiveStatus == VRInitError_Init_AlreadyRunning,
              "recursive OCU init during native load was not rejected");
    }
    return &FakeSteamVR;
}
'''
preamble += 'int InitRoutingOnly(int* peError) {\n' + init_guard + '\n*peError = VRInitError_None; return 1;\n}\n'
main = r'''
int main(int argc, char** argv) {
    try {
        int status = -1;
        const string scenario = argc > 1 ? argv[1] : "late-preference";
        if (scenario == "late-preference") {
            void* original = VRClientCoreFactory("IVRClientCore_003", &status);
            Check(original && original != &alternateObject, "first OCU selection failed");
            preferOCU = false;
            Check(VRClientCoreFactory("IVRClientCore_003", &status) == original,
                "late factory request loaded SteamVR after selecting OCU");
            Check(alternativeLoads == 0 && preferenceReads == 1, "late request reconsidered runtime preference");
        } else if (scenario == "active-ocu") {
            // VR_InitInternal2 pins ownership before creating its backend.
            running = true; ocuClientCoreSelected.store(true); preferOCU = false;
            Check(VRClientCoreFactory("IVRClientCore_003", &status) != &alternateObject,
                "active OCU scene was handed to SteamVR");
            Check(preferenceReads == 0 && alternativeLoads == 0, "active scene consulted an alternate runtime");
        } else if (scenario == "startup-steamvr") {
            preferOCU = false;
            Check(VRClientCoreFactory("IVRClientCore_003", &status) == &alternateObject, "explicit startup SteamVR selection lost");
            Check(VRClientCoreFactory("IVRClientCore_003", &status) == &alternateObject, "cached alternative factory lost");
            Check(alternativeLoads == 1 && alternativeCalls == 2, "alternative loaded more than once");
        } else if (scenario == "concurrent-selection") {
            racePreferences = true;
            void* first = nullptr;
            void* second = nullptr;
            std::thread delayed([&] { int status = -1; first = VRClientCoreFactory("IVRClientCore_003", &status); });
            firstPreferenceEntered.get_future().wait();
            std::thread later([&] { int status = -1; second = VRClientCoreFactory("IVRClientCore_003", &status); secondFactoryDone.set_value(); });
            delayed.join(); later.join();
            Check(first == second, "concurrent requests returned OCU and SteamVR cores in one process");
            Check(preferenceReads == 1 && alternativeLoads <= 1, "runtime selection was not serialized");
        } else if (scenario == "concurrent-ocu") {
            constexpr int count = 32;
            std::vector<void*> returned(count);
            std::vector<std::thread> callers;
            std::promise<void> start;
            auto ready = start.get_future().share();
            for (int i = 0; i < count; ++i)
                callers.emplace_back([&, i] { ready.wait(); int status = -1; returned[i] = VRClientCoreFactory("IVRClientCore_003", &status); });
            start.set_value();
            for (auto& caller : callers) caller.join();
            for (void* result : returned) Check(result && result == returned[0] && result != &alternateObject, "concurrent OCU factory results differ");
            Check(preferenceReads == 1 && alternativeLoads == 0, "concurrent OCU requests reread runtime preference");
        } else if (scenario == "native-then-direct-init") {
            preferOCU = false;
            Check(VRClientCoreFactory("IVRClientCore_003", &status) == &alternateObject, "native selection failed");
            Check(InitRoutingOnly(&status) == 0 && status == VRInitError_Init_AlreadyRunning,
                  "direct init started OCU after handing out a native core");
            Check(!ocuClientCoreSelected.load(), "rejected direct init changed runtime ownership");
        } else if (scenario == "direct-init-shutdown-init") {
            Check(InitRoutingOnly(&status) == 1 && status == 0, "direct OCU selection failed");
            preferOCU = false;
            running = false; // Shutdown clears running, but must not clear process ownership.
            Check(InitRoutingOnly(&status) == 1 && status == 0, "OCU reinit lost its runtime ownership");
            Check(VRClientCoreFactory("IVRClientCore_003", &status) != &alternateObject && alternativeLoads == 0,
                  "shutdown allowed a switch to native in the same process");
        } else if (scenario == "native-load-reentry") {
            preferOCU = false; reenterOnLoad = true;
            Check(VRClientCoreFactory("IVRClientCore_003", &status) == &alternateObject && status == 0,
                  "outer native request failed after rejected loader reentry");
            Check(alternativeLoads == 1 && alternativeCalls == 1, "native loader recursion loaded duplicate runtimes");
        } else if (scenario == "native-call-reentry") {
            preferOCU = false; reenterOnCall = true;
            Check(VRClientCoreFactory("IVRClientCore_003", &status) == &alternateObject && status == 0,
                  "native factory reentry blocked by selection lock");
            Check(alternativeLoads == 1 && alternativeCalls == 2, "native call reentry lost cached factory");
        } else throw std::runtime_error("unknown scenario");
        std::printf("PASS: %s; exact shipping factory body, alternate runtime mocked.\n", scenario.c_str());
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
}
'''
Path(sys.argv[2]).write_text(preamble + factory + main, encoding='utf-8')
