#pragma once
#include <cstdint>

// Deliberate, non-consuming shortcut. Both controls must be released before
// arming and after any interruption; normal game click bindings are untouched.
class OcuLocomotionCalibrationGesture {
public:
    bool Update(std::uint64_t now, bool available, bool left, bool right, bool centered) {
        if (!available || !centered || (lastSample && (now < lastSample || now - lastSample > 250))) {
            armed = holding = false; lastSample = now; return false;
        }
        lastSample = now;
        if (!left && !right) { armed = true; holding = false; return false; }
        if (!left || !right) { if (holding) armed = false; holding = false; return false; }
        if (!armed) return false;
        if (!holding) { holding = true; started = now; return false; }
        if (now - started < 2000) return false;
        armed = holding = false;
        return true;
    }
private:
    bool armed = false, holding = false;
    std::uint64_t started = 0, lastSample = 0;
};
