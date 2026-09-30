#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace RE {
struct GFxMovieView {
    enum class HitTestType { kButtonEvents };
    bool corruptTree = true;
    bool hit = false;
    int probes = 0;
    bool HitTest(float, float, HitTestType, unsigned) {
        ++probes;
        if (corruptTree) throw std::runtime_error("unsafe full-movie traversal");
        return hit;
    }
};
}
#include "../src/LaserMenuHit.inl"

int main() {
    int checks = 0;
    auto check = [&](bool value) {
        ++checks;
        if (!value) throw std::runtime_error("check " + std::to_string(checks));
    };
    RE::GFxMovieView movie;
    // Model the reported movie: its full-tree probe faults, but an in-quad ray
    // must remain eligible for normal Crafting input at every supported scale.
    for (float scale : {1.0f, 1.5f, 2.0f, 3.0f}) {
        const float w = 1280 * scale, h = 720 * scale;
        for (float u : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f})
            for (float v : {0.0f, 0.5f, 1.0f}) {
                check(ProbeLaserPointerTarget(movie, "Crafting Menu", u*w, v*h, w, h, true));
                check(!ProbeLaserPointerTarget(movie, "Crafting Menu", u*w, v*h, w, h, false));
            }
        check(!ProbeLaserPointerTarget(movie, "Crafting Menu", -1, 0, w, h, true));
        check(!ProbeLaserPointerTarget(movie, "Crafting Menu", w+1, 0, w, h, true));
        check(!ProbeLaserPointerTarget(movie, "Crafting Menu", 0, -1, w, h, true));
        check(!ProbeLaserPointerTarget(movie, "Crafting Menu", 0, h+1, w, h, true));
    }
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    for (const char* menu : {"Crafting Menu", "Journal Menu", "InventoryMenu"}) {
        check(!ProbeLaserPointerTarget(movie, menu, nan, 0, 1280, 720, true));
        check(!ProbeLaserPointerTarget(movie, menu, 0, inf, 1280, 720, true));
        check(!ProbeLaserPointerTarget(movie, menu, 0, 0, inf, 720, true));
        check(!ProbeLaserPointerTarget(movie, menu, 0, 0, 1280, nan, true));
        check(!ProbeLaserPointerTarget(movie, menu, 0, 0, 0, 720, true));
        check(!ProbeLaserPointerTarget(movie, menu, 0, 0, 1280, -1, true));
        check(!ProbeLaserPointerTarget(movie, menu, 0, 0, 1280, 720, false));
    }
    check(!ProbeLaserPointerTarget(movie, nullptr, 0, 0, 1280, 720, true));
    check(movie.probes == 0);
    // Unaffected menu routes retain the actual button-hit result, including misses.
    movie.corruptTree = false;
    for (const char* menu : {"Journal Menu", "InventoryMenu", "MessageBoxMenu", "TweenMenu"}) {
        movie.hit = false;
        check(!ProbeLaserPointerTarget(movie, menu, 100, 100, 1280, 720, true));
        movie.hit = true;
        check(ProbeLaserPointerTarget(movie, menu, 100, 100, 1280, 720, true));
    }
    check(movie.probes == 8);
    std::cout << "PASS: " << checks << " production laser-probe checks; Crafting never traverses the corrupt movie tree\n";
}
