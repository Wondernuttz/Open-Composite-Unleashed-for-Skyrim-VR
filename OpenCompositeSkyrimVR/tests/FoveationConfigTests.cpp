#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace vr { struct HmdColor_t { float r, g, b, a; }; }
using namespace std;
using vr::HmdColor_t;
#define private public
#include "OpenOVR/Misc/Config.h"
#undef private
#define ABORT(message) throw std::runtime_error(message)
#define OOVR_LOGF(...) ((void)0)
#include "FoveationConfigProduction.inl"

// Configuration file discovery and external-upscaler publication are outside
// this fixture; member initialization and INI parsing are production code.
Config::Config() = default;
Config::~Config() = default;
static unsigned checks = 0;
static void Check(bool value, const char* message)
{
    ++checks;
    if (!value) throw std::runtime_error(message);
}
static void Set(Config& config, const char* section, const char* key, const char* value)
{
    Check(Config::ini_handler(&config, section, key, value, 1) != 0, "INI option accepted");
}
static bool Near(float x, float y) { return std::fabs(x - y) < 0.00001f; }

int main()
{
    try {
        using namespace ocu_foveation;
        Config fresh;
        const auto eye = fresh.FoveationRadii(true);
        const auto fixed = fresh.FoveationRadii(false);
        Check(Near(eye.inner, .20f) && Near(eye.mid, .40f), "fresh gaze uses Performance radii");
        Check(fresh.VrsEyeCustomRates() && !fresh.VrsEyeCompatibilityMode(), "fresh Performance uses explicit uncapped rings");
        Check(fresh.FoveationRates(true) == RingRates{Rate::X1x1, Rate::X2x2, Rate::X4x2}, "fresh Performance effective rates");
        Check(Near(fixed.inner, .70f) && Near(fixed.mid, .85f), "fixed default radii unchanged");
        Check(fresh.FoveationRates(false) == RingRates{Rate::X1x1, Rate::X2x1, Rate::X2x1}, "fixed default cap unchanged");
        for (const char* section : {"", "default", "vrs"}) {
            Config untuned;
            Set(untuned, section, "vrsEyeTracked", "true");
            Set(untuned, section, "foveationDebugRings", "false");
            Set(untuned, section, "foveatedBackend", "auto");
            Check(untuned.FoveationRates(true) == RingRates{Rate::X1x1, Rate::X2x2, Rate::X4x2}, "eye enable, backend and debug options do not opt out of fresh Performance");
            // Old hand-edited profiles can omit any of the rate keys. Those
            // omitted keys retain their old values even when custom is on.
            for (unsigned specified = 0; specified < 8; ++specified) {
                for (bool modeFirst : {false, true}) {
                    Config partial;
                    if (modeFirst) {
                        Set(partial, section, "vrsEyeCustomRates", "true");
                        Set(partial, section, "vrsEyeCompatibilityMode", "false");
                    }
                    if (specified & 1) Set(partial, section, "vrsEyeInnerRate", "1x2");
                    if (specified & 2) Set(partial, section, "vrsEyeMidRate", "2x2");
                    if (specified & 4) Set(partial, section, "vrsEyeOuterRate", "4x4");
                    if (!modeFirst) {
                        Set(partial, section, "vrsEyeCustomRates", "true");
                        Set(partial, section, "vrsEyeCompatibilityMode", "false");
                    }
                    Check(partial.FoveationRates(true) == RingRates{
                        specified & 1 ? Rate::X1x2 : Rate::X1x1,
                        specified & 2 ? Rate::X2x2 : Rate::X2x1,
                        specified & 4 ? Rate::X4x4 : Rate::X2x2}, "partial saved rate profile preserves every unspecified legacy rate in either key order");
                }
            }
            for (bool legacyCap : {false, true}) {
                Config legacy;
                Set(legacy, section, "vrsCompatibilityMode", legacyCap ? "true" : "false");
                Set(legacy, section, "vrsInnerRadius", "0.63");
                Set(legacy, section, "vrsMidRadius", "0.83");
                Check(legacy.VrsEyeCompatibilityMode() == legacyCap, "explicit legacy cap inherited by gaze");
                Check(!legacy.VrsEyeCustomRates(), "legacy tuned RDM sampling path retained");
                for (bool tracked : {false, true}) {
                    const auto radii = legacy.FoveationRadii(tracked);
                    Check(Near(radii.inner, .63f) && Near(radii.mid, .83f), "legacy sizes remain intact");
                }
                for (bool eyeCap : {false, true}) {
                    for (bool eyeFirst : {false, true}) {
                        Config both;
                        if (eyeFirst) Set(both, section, "vrsEyeCompatibilityMode", eyeCap ? "true" : "false");
                        Set(both, section, "vrsCompatibilityMode", legacyCap ? "true" : "false");
                        if (!eyeFirst) Set(both, section, "vrsEyeCompatibilityMode", eyeCap ? "true" : "false");
                        Check(both.VrsEyeCompatibilityMode() == eyeCap, "explicit eye cap wins regardless of order");
                        Check(both.FoveationRates(false).outer == (legacyCap ? Rate::X2x1 : Rate::X2x2), "eye cap cannot alter fixed rates");
                    }
                }
            }
            Config saved;
            Set(saved, section, "vrsEyeCustomRates", "false");
            Set(saved, section, "vrsEyeCompatibilityMode", "true");
            Set(saved, section, "vrsEyeInnerRadius", "0.51");
            Set(saved, section, "vrsEyeMidRadius", "0.73");
            Set(saved, section, "vrsEyeOuterRate", "4x4");
            Check(!saved.VrsEyeCustomRates() && saved.VrsEyeCompatibilityMode(), "saved mode and cap preserved");
            Check(Near(saved.FoveationRadii(true).inner, .51f) && Near(saved.FoveationRadii(true).mid, .73f), "saved eye sizes preserved");
            Check(saved.FoveationRates(true).outer == Rate::X2x1, "requested outer cannot evade saved cap");
            Set(saved, section, "vrsEyeCustomRates", "true");
            Set(saved, section, "vrsEyeCompatibilityMode", "false");
            Check(saved.FoveationRates(true).outer == Rate::X4x4, "saved custom rate survives cap changes");
            for (bool cap : {false, true}) {
                Config vertical;
                Set(vertical, section, "vrsFavorHorizontal", "false");
                Set(vertical, section, "vrsCompatibilityMode", cap ? "true" : "false");
                Check(!vertical.VrsEyeCustomRates(), "legacy axis retains legacy RDM pattern");
                Check(vertical.FoveationRates(true) == RingRates{Rate::X1x1, Rate::X1x2, cap ? Rate::X1x2 : Rate::X2x2}, "legacy vertical rate remains vertical");
                for (bool explicitFirst : {false, true}) {
                    Config aggressive;
                    if (explicitFirst) Set(aggressive, section, "vrsEyeCustomRates", "true");
                    Set(aggressive, section, "vrsFavorHorizontal", "false");
                    Set(aggressive, section, "vrsCompatibilityMode", cap ? "true" : "false");
                    Set(aggressive, section, "vrsEyeCompatibilityMode", "false");
                    Set(aggressive, section, "vrsEyeInnerRadius", "0.20");
                    Set(aggressive, section, "vrsEyeMidRadius", "0.40");
                    Set(aggressive, section, "vrsEyeInnerRate", "1x1");
                    Set(aggressive, section, "vrsEyeMidRate", "2x2");
                    Set(aggressive, section, "vrsEyeOuterRate", "4x4");
                    if (!explicitFirst) Set(aggressive, section, "vrsEyeCustomRates", "true");
                    Check(aggressive.VrsEyeCustomRates(), "explicit Aggressive sampling survives legacy keys in either order");
                    Check(aggressive.FoveationRates(true) == RingRates{Rate::X1x1, Rate::X2x2, Rate::X4x4}, "Aggressive explicit rate axis wins over legacy direction");
                }
            }
            for (const auto& option : {
                     std::pair{"vrsEyeInnerRadius", "0.6"}, {"vrsEyeMidRadius", "0.8"},
                     {"vrsInnerRadius", "0.6"}, {"vrsMidRadius", "0.8"},
                     {"vrsFavorHorizontal", "false"}, {"vrsEyeCustomRates", "false"},
                     {"vrsEyeCustomRates", "true"}, {"vrsEyeOuterRate", "4x4"}}) {
                Config tuned;
                Set(tuned, section, option.first, option.second);
                Check(tuned.VrsEyeCompatibilityMode(), "tuned legacy profile retains previously implicit cap");
                Check(tuned.FoveationRates(true).outer == (std::string(option.first) == "vrsFavorHorizontal" ? Rate::X1x2 : Rate::X2x1), "tuned legacy profile retains half density");
            }
            for (const char* radiusKey : {"vrsFixedInnerRadius", "vrsFixedMidRadius"}) {
                Config fixedOnly;
                Set(fixedOnly, section, radiusKey, "0.8");
                Check(fixedOnly.VrsEyeCustomRates(), "fixed-only tuning does not change fresh eye sampling");
                Check(fixedOnly.FoveationRates(true) == RingRates{Rate::X1x1, Rate::X2x2, Rate::X4x2}, "fixed-only tuning does not change Performance rates");
            }
        }
        std::printf("PASS %u production configuration checks: Performance default, Aggressive saved, legacy, saved profiles, cap precedence, fixed isolation\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL after %u checks: %s\n", checks, error.what());
        return 1;
    }
}
