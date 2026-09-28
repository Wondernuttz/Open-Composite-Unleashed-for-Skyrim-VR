#pragma once
#include <atomic>
#include <cmath>
#include <cstdint>

namespace ocu_cable {
constexpr int ShowAction = 0x7f01, ResetAction = 0x7f02;
inline std::atomic<unsigned> requests{0};
inline bool Shortcut(int action, bool up) {
    if (action != ShowAction && action != ResetAction) return false;
    if (!up) requests.fetch_or(action == ShowAction ? 1u : 2u, std::memory_order_relaxed);
    return true;
}
struct Settings {
    bool enabled = false;
    int showKey = 119, resetKey = 119; // F8; reset additionally requires Ctrl+Shift.
    int showModifiers = 0, resetModifiers = 3; // Ctrl=1, Shift=2, Alt=4
    int displaySeconds = 5;
    float warningTurns = 2.f; // zero disables automatic warnings
};

// Only physical orientation in a runtime reference space enters this counter.
// A discontinuity preserves the count but invalidates the next delta.
class Counter {
public:
    void Gap() { if (started) uncertain = true; baseline = false; }
    void Rebase() { baseline = false; }
    void Reset() { radians = 0; uncertain = false; }
    void Sample(double x, double y, double z, double w, double seconds, bool tracked) {
        const double norm = x*x+y*y+z*z+w*w;
        if (!tracked || !std::isfinite(norm) || std::abs(norm-1.) > .02 || !std::isfinite(seconds)) { Gap(); return; }
        // Project the headset's forward direction onto the physical floor.
        const double a = 2*(w*y+x*z), b = 1-2*(x*x+y*y);
        if (a*a+b*b < .04) { Gap(); return; } // heading undefined when looking vertically
        const double yaw = std::atan2(a,b);
        if (baseline) {
            const double dt = seconds-lastTime;
            if (dt == 0) return; // same predicted display time, no double counting
            const double delta = std::remainder(yaw-lastYaw, 2*Pi);
            if (dt <= 0 || dt > .5 || std::abs(delta) > .15+12*dt) Gap();
            else radians += delta;
        }
        lastYaw = yaw; lastTime = seconds; baseline = started = true;
    }
    double Turns() const { return radians/(2*Pi); }
    bool Uncertain() const { return uncertain; }
    bool Tracked() const { return baseline; }
private:
    static constexpr double Pi = 3.14159265358979323846;
    double radians = 0, lastYaw = 0, lastTime = 0;
    bool baseline = false, uncertain = false, started = false;
};
}
