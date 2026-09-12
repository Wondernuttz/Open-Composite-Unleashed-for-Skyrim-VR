#include "OpenOVR/Misc/EffectFoveationState.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>

using namespace ocu_effect_foveation;
static std::atomic<unsigned> failures{0};

static void Check(bool condition, const char* message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

static Snapshot Profile(std::int64_t marker = 1)
{
    Snapshot value{};
    value.mode = Mode::EyeTracked;
    value.shape = Shape::UVRadialHalfExtent;
    value.publicationQpc = marker * 10;
    value.predictedDisplayTime = marker;
    // Precisely zero means valid source pose with unavailable sample time.
    value.gazeSampleTime = 0;
    value.innerRadius = 0.6f;
    value.midRadius = 0.8f;
    for (int eye = 0; eye < 2; ++eye) {
        value.centerUV[eye][0] = static_cast<float>(marker % 99) / 100.0f;
        value.centerUV[eye][1] = 1.0f - value.centerUV[eye][0];
        value.fovTangents[eye][0] = -1.0f - static_cast<float>(eye);
        value.fovTangents[eye][1] = 1.0f;
        value.fovTangents[eye][2] = 1.1f;
        value.fovTangents[eye][3] = -0.9f;
    }
    return value;
}

static Snapshot Read(State& state)
{
    Snapshot value{};
    Check(state.Query(Version, sizeof(value), &value) == Result::Success, "query succeeds");
    Check(value.structSize == 128 && value.version == Version, "ABI size/version");
    return value;
}

static void CheckContract()
{
    State state;
    Check(Read(state).mode == Mode::Disabled, "initial query disabled");
    Snapshot sentinel;
    std::memset(&sentinel, 0x5a, sizeof(sentinel));
    const Snapshot before = sentinel;
    Check(state.Query(Version + 1, sizeof(sentinel), &sentinel) == Result::UnsupportedVersion,
        "unsupported ABI rejected");
    Check(std::memcmp(&sentinel, &before, sizeof(sentinel)) == 0, "bad version leaves output untouched");
    Check(state.Query(Version, sizeof(sentinel) - 1, &sentinel) == Result::InvalidOutput,
        "short output rejected");
    Check(std::memcmp(&sentinel, &before, sizeof(sentinel)) == 0, "short output leaves bytes untouched");
    Check(state.Query(Version, sizeof(sentinel), nullptr) == Result::InvalidOutput, "null output rejected");
    struct Extended { Snapshot snapshot; std::uint64_t extra; } extended{{}, 0x12345678};
    Check(state.Query(Version, sizeof(extended), &extended.snapshot) == Result::Success &&
        extended.extra == 0x12345678, "larger caller retains extension bytes");

    Check(!state.Publish(Profile()), "cannot publish before a game frame");
    state.BeginFrame(10, 10000000);
    Check(state.Publish(Profile()), "active valid profile publishes");
    auto value = Read(state);
    Check(value.mode == Mode::EyeTracked && value.gazeSampleTime == 0 && value.flags == 0,
        "valid gaze accepted with unavailable source sample time");
    Check(value.frameId == 1 && value.qpcFrequency == 10000000, "provider owns frame/clock identity");
    auto predicted = Profile();
    predicted.gazeSampleTime = predicted.predictedDisplayTime + 1000000000;
    Check(state.Publish(predicted) && Read(state).flags == SourceSampleTimeAvailable,
        "predicted source timestamp is metadata, not age gate");
    auto old = Profile();
    old.gazeSampleTime = -100000;
    Check(state.Publish(old), "different runtime timeline value is not QPC age");

    state.BeginFrame(20, 10000000);
    value = Read(state);
    Check(value.frameId == 2 && value.mode == Mode::Disabled && value.innerRadius == 0,
        "next frame immediately clears old gaze before early returns");
    Check(state.Publish(Profile(2)), "recovery publishes in the next frame immediately");
    state.Clear(21);
    Check(Read(state).mode == Mode::Disabled, "submit/shutdown clears immediately");
    Check(state.ReadForPresentation(21).mode == Mode::EyeTracked,
        "diagnostic presentation retains the submitted frame without reopening the renderer API");
    Check(state.ReadForPresentation(6000021).mode == Mode::Disabled,
        "diagnostic presentation expires old gaze");
    Check(!state.Publish(Profile(2)), "late publication cannot reopen a submitted frame");
    state.BeginFrame(30, 10000000);
    Check(state.ReadForPresentation(30).mode == Mode::Disabled,
        "a new game frame never inherits previous diagnostic gaze");
    auto fixed = Profile(3);
    fixed.mode = Mode::Fixed;
    Check(state.Publish(fixed) && Read(state).mode == Mode::Fixed, "explicit fixed mode published distinctly");

    const auto invalid = [&](Snapshot candidate, const char* message) {
        Check(!state.Publish(candidate), message);
        Check(Read(state).mode == Mode::Disabled, "invalid profile always clears active data");
    };
    auto bad = Profile(); bad.centerUV[1][0] = std::numeric_limits<float>::quiet_NaN();
    invalid(bad, "NaN gaze rejected");
    bad = Profile(); bad.centerUV[0][1] = -0.01f; invalid(bad, "out-of-eye coordinate rejected");
    bad = Profile(); bad.fovTangents[1][1] = -3.0f; invalid(bad, "reversed FOV rejected");
    bad = Profile(); bad.midRadius = 0.5f; invalid(bad, "reversed rings rejected");
    bad = Profile(); bad.predictedDisplayTime = 0; invalid(bad, "missing display time rejected");
    bad = Profile(); bad.shape = static_cast<Shape>(2); invalid(bad, "unknown shape rejected");
    Check(ClockTicks() > 0 && ClockFrequency() > 0, "monotonic publication clock available");
}

static void CheckPresentationLifetime()
{
    constexpr std::int64_t frequency = 10000000;
    const auto tracked = Profile(100);
    auto fixed = tracked;
    fixed.mode = Mode::Fixed;
    State state;
    state.BeginFrame(fixed.publicationQpc, frequency);
    Check(state.Publish(fixed), "fixed presentation fixture publishes");
    for (const auto delay : {frequency / 2, frequency / 2 + 1, frequency, frequency * 30}) {
        const auto displayed = state.ReadForPresentation(fixed.publicationQpc + delay);
        Check(displayed.mode == Mode::Fixed && displayed.frameId == Read(state).frameId &&
            displayed.publicationQpc == fixed.publicationQpc,
            "fixed rings survive a long rendering frame without extending renderer state");
    }
    state.Clear(fixed.publicationQpc + 1);
    Check(Read(state).mode == Mode::Disabled &&
        state.ReadForPresentation(fixed.publicationQpc + frequency).mode == Mode::Fixed,
        "submitted fixed rings remain visible while renderer API stays closed");
    Check(state.ReadForPresentation(fixed.publicationQpc - 1).mode == Mode::Disabled,
        "fixed presentation still rejects a backwards clock");

    state.BeginFrame(fixed.publicationQpc + frequency * 31, frequency);
    Check(state.ReadForPresentation(fixed.publicationQpc + frequency * 31).mode == Mode::Disabled,
        "menu or missing-geometry frame immediately clears old fixed rings");
    Check(state.Publish(tracked), "tracked presentation fixture publishes");
    Check(state.ReadForPresentation(tracked.publicationQpc + frequency / 2).mode == Mode::EyeTracked,
        "tracked rings remain valid at the expiry boundary");
    Check(state.ReadForPresentation(tracked.publicationQpc + frequency / 2 + 1).mode == Mode::Disabled,
        "stale tracked rings still expire after half a second");

    state.BeginFrame(fixed.publicationQpc, frequency);
    Check(state.Publish(fixed) &&
        state.ReadForPresentation(fixed.publicationQpc + frequency).mode == Mode::Fixed,
        "gaze-loss fixed fallback follows fixed presentation lifetime");
    auto bad = fixed;
    bad.fovTangents[0][1] = bad.fovTangents[0][0];
    Check(!state.Publish(bad) &&
        state.ReadForPresentation(fixed.publicationQpc + frequency).mode == Mode::Disabled,
        "invalid replacement immediately clears fixed presentation");
    state.BeginFrame(fixed.publicationQpc, 0);
    Check(!state.Publish(fixed) &&
        state.ReadForPresentation(fixed.publicationQpc).mode == Mode::Disabled,
        "invalid clock frequency cannot create fixed rings");
}

static void CheckConcurrentReaders()
{
    State state;
    std::atomic<bool> begin{false}, done{false};
    std::atomic<std::uint64_t> reads{0};
    std::vector<std::thread> readers;
    for (int reader = 0; reader < 3; ++reader) {
        readers.emplace_back([&] {
            while (!begin.load(std::memory_order_acquire)) std::this_thread::yield();
            std::uint64_t previousFrame = 0;
            do {
                Snapshot value{};
                const auto result = state.Query(Version, sizeof(value), &value);
                bool coherent = result == Result::Success && value.frameId >= previousFrame &&
                    value.structSize == sizeof(value) && value.version == Version;
                previousFrame = value.frameId;
                if (value.mode != Mode::Disabled) {
                    const auto expected = Profile(value.predictedDisplayTime);
                    coherent = coherent && value.publicationQpc == expected.publicationQpc &&
                        value.qpcFrequency == 10000000 && value.innerRadius == 0.6f && value.midRadius == 0.8f &&
                        std::memcmp(value.centerUV, expected.centerUV, sizeof(value.centerUV)) == 0 &&
                        std::memcmp(value.fovTangents, expected.fovTangents, sizeof(value.fovTangents)) == 0;
                } else {
                    coherent = coherent && value.innerRadius == 0 && value.midRadius == 0 && value.gazeSampleTime == 0;
                }
                Check(coherent, "concurrent reader never observes mixed frame/eye fields");
                ++reads;
            } while (!done.load(std::memory_order_acquire));
        });
    }
    begin.store(true, std::memory_order_release);
    for (std::int64_t frame = 1; frame <= 50000; ++frame) {
        state.BeginFrame(frame * 10 - 1, 10000000);
        Check(state.Publish(Profile(frame)), "single producer publishes concurrently");
        if ((frame % 3) == 0) state.Clear(frame * 10 + 1);
    }
    done.store(true, std::memory_order_release);
    for (auto& reader : readers) reader.join();
    Check(reads > 0, "concurrency exercise observed publications");
    std::printf("Concurrent snapshot reads: %llu across 50000 frame publications\n",
        static_cast<unsigned long long>(reads.load()));
}

int main()
{
    CheckContract();
    CheckPresentationLifetime();
    CheckConcurrentReaders();
    std::printf("Effect foveation ABI/state tests: %u failures\n", failures.load());
    return failures == 0 ? 0 : 1;
}
