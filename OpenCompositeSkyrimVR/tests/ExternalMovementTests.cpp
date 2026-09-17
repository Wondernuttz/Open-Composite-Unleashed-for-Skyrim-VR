#include <atomic>
#include <cmath>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <thread>
#include "OpenOVR/Misc/Input/LocomotionFrame.h"
#include "OpenOVR/Misc/Input/LocomotionHeading.h"

// Deliberately no BackendManager/HMD/runtime mocks: the production input
// entry point must compile and execute without accessing those lifetimes.
struct { bool enabled = true; bool TreadmillEnabled() const { return enabled; } } oovr_global_configuration;
int menuState = 0;
int OCBridge_LocomotionState() { return menuState; }
std::atomic<bool> g_ocuKeyboardActive{false};
namespace CameraLegCalibration { bool capture = false; bool CapturesInput() { return capture; } }
struct OscLocomotion {
    locomotion::Receiver receiver{{250, 3, .05}};
    std::uint64_t now = 1000;
    unsigned reads = 0;
    static OscLocomotion& Instance() { static OscLocomotion instance; return instance; }
    locomotion::Output Read(locomotion::Stick physical, std::optional<double> yaw, bool focused, bool paused,
        std::uint64_t referenceEpoch = 0, std::uint64_t token = 0, bool calibrationAllowed = true) {
        ++reads;
        return receiver.Mix(physical, now, yaw, focused, paused);
    }
};
class BaseInput {
public:
    static std::uint64_t InputNowMs() { return OscLocomotion::Instance().now; }
    struct ExternalMovement { float x = 0, y = 0; bool ownsAxis = false, treadmillSelected = false, allowSynthetic = false; };
    ExternalMovement ReadExternalMovement(float, float) const;
};
#include "ExternalMovementProduction.inc"

unsigned checks = 0;
void Check(bool condition, const char* label) {
    ++checks;
    if (!condition) throw std::runtime_error(label);
}
bool Near(double a, double b) { return std::abs(a-b) < .00001; }
int main() {
    try {
        BaseInput input;
        auto& osc = OscLocomotion::Instance();
        auto& heading = OcuLocomotionHeading::Instance();
        const auto publish = [&](double yaw = 0) {
            heading.Publish(heading.CaptureToken(), osc.now, true, 0, std::sin(yaw / 2), 0, std::cos(yaw / 2));
        };
        auto output = input.ReadExternalMovement(0, 0);
        Check(!output.allowSynthetic && Near(output.y, 0), "pre-publication startup is neutral without backend access");
        Check(osc.reads == 1, "receiver serviced before first head sample");
        output = input.ReadExternalMovement(.2f, -.3f);
        Check(Near(output.x, .2) && Near(output.y, -.3), "physical input preserved during startup");
        heading.SetFocused(true);
        Check(!input.ReadExternalMovement(0, 0).allowSynthetic, "focus alone cannot invent head pose");

        for (unsigned invalid = 0; invalid != 5; ++invalid) {
            publish();
            Check(input.ReadExternalMovement(0, 0).allowSynthetic, "valid head sample is eligible");
            const double tilt = std::sqrt(.5);
            heading.Publish(heading.CaptureToken(), osc.now, invalid != 0,
                invalid == 4 ? tilt : 0, invalid == 1 ? std::numeric_limits<double>::quiet_NaN() : 0,
                0, invalid == 2 ? 0 : invalid == 3 ? 2 : invalid == 4 ? tilt : 1);
            Check(!input.ReadExternalMovement(0, 0).allowSynthetic,
                "invalid flags, nonfinite, zero/nonunit or vertical head invalidates old sample");
        }
        for (const double yaw : {0.0, .45, 1.5707963267948966, -1.1, 3.0}) {
            publish(yaw);
            const auto sample = heading.Read(osc.now, false);
            Check(sample.yaw && Near(*sample.yaw, yaw), "head quaternion preserves signed standing-space heading");
        }
        publish();

        locomotion::Frame frame{};
        frame.epoch = 1; frame.flags = locomotion::Connected; frame.vz = -1.5f;
        const auto send = [&] {
            ++frame.sequence; ++osc.now;
            Check(osc.receiver.Ingest(frame, osc.now, heading.Read(osc.now, false).yaw) == locomotion::Error::None,
                "accepted fresh treadmill sample");
        };
        send(); send(); frame.calibrationSerial = 1; send();
        output = input.ReadExternalMovement(0, 0);
        Check(output.allowSynthetic && output.treadmillSelected && Near(output.y, .5), "calibrated movement recovers after startup");
        output = input.ReadExternalMovement(-.2f, -.1f);
        Check(Near(output.x, -.2) && Near(output.y, -.1) && !output.treadmillSelected, "physical stick priority retained");
        publish(1.5707963267948966);
        output = input.ReadExternalMovement(0, 0);
        Check(Near(output.x, .5) && Near(output.y, 0), "head rotation converts treadmill forward to correct lateral game axis");
        publish();

        for (unsigned gate = 0; gate != 6; ++gate) {
            const auto previousToken = heading.CaptureToken();
            if (gate == 0) menuState = 1;
            if (gate == 1) g_ocuKeyboardActive = true;
            if (gate == 2) CameraLegCalibration::capture = true;
            if (gate == 3) heading.SetFocused(false);
            if (gate == 4) heading.Reset();
            if (gate == 5) heading.Invalidate();
            output = input.ReadExternalMovement(0, 0);
            Check(!output.allowSynthetic && Near(output.y, 0), "menu/focus/session/reference transition stops movement");
            menuState = 0; g_ocuKeyboardActive = false; CameraLegCalibration::capture = false;
            heading.SetFocused(true);
            Check(!input.ReadExternalMovement(0, 0).allowSynthetic, "resume requires new head sample");
            heading.Publish(previousToken, osc.now, true, 0, 0, 0, 1);
            Check(!input.ReadExternalMovement(0, 0).allowSynthetic, "in-flight sample from before transition is rejected");
            publish();
            Check(Near(input.ReadExternalMovement(0, 0).y, 0), "new head sample alone cannot replay old treadmill packet");
            send();
            Check(Near(input.ReadExternalMovement(0, 0).y, .5), "fresh head and treadmill samples restore movement");
        }
        osc.now += OcuLocomotionHeading::MaxAgeMs + 1;
        send();
        Check(!input.ReadExternalMovement(0, 0).allowSynthetic, "fresh treadmill cannot keep a stale head pose alive");
        publish();
        Check(Near(input.ReadExternalMovement(0, 0).y, 0), "stale head recovery waits for new treadmill data");
        send();
        Check(Near(input.ReadExternalMovement(0, 0).y, .5), "stale head recovers without restart or artificial pose");
        osc.now += 251;
        publish();
        Check(Near(input.ReadExternalMovement(0, 0).y, 0), "sender timeout stops movement with fresh head pose");

        std::atomic<bool> done{false}, corrupt{false};
        std::thread writer([&] {
            for (unsigned i = 0; i != 50000; ++i) {
                const auto token = heading.CaptureToken();
                heading.Publish(token, osc.now, true, 0, std::sin(.25), 0, std::cos(.25));
            }
            done.store(true);
        });
        while (!done.load()) {
            const auto sample = heading.Read(osc.now, false);
            if (sample.yaw && !Near(*sample.yaw, 0) && !Near(*sample.yaw, .5)) corrupt.store(true);
            heading.Invalidate();
        }
        writer.join();
        Check(!corrupt.load(), "concurrent read/publication/invalidation returns only coherent samples");
        oovr_global_configuration.enabled = false;
        const auto reads = osc.reads;
        output = input.ReadExternalMovement(0, 0);
        Check(!output.ownsAxis && !output.allowSynthetic && osc.reads == reads, "disabled feature bypasses receiver and heading cache");
        std::cout << checks << " production movement/cache checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
