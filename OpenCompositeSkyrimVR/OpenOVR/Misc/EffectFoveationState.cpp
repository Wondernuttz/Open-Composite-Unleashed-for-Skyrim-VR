#include "EffectFoveationState.h"
#include "FoveationGeometrySettings.h"

#include <cmath>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#else
#include <chrono>
#endif

namespace ocu_effect_foveation {
namespace {
Snapshot Disabled(std::uint64_t frame, std::int64_t ticks, std::int64_t frequency)
{
    Snapshot value{};
    value.structSize = sizeof(value);
    value.version = Version;
    value.mode = Mode::Disabled;
    value.shape = Shape::UVRadialHalfExtent;
    value.frameId = frame;
    value.publicationQpc = ticks;
    value.qpcFrequency = frequency;
    return value;
}

bool ValidProfile(const Snapshot& value)
{
    if (value.shape != Shape::UVRadialHalfExtent ||
        (value.mode != Mode::Fixed && value.mode != Mode::EyeTracked) ||
        value.predictedDisplayTime <= 0 || value.publicationQpc <= 0 ||
        !std::isfinite(value.innerRadius) || !std::isfinite(value.midRadius) ||
        value.innerRadius < 0.1f || value.innerRadius > 1.0f ||
        value.midRadius < value.innerRadius || value.midRadius > 1.5f)
        return false;
    for (int eye = 0; eye < 2; ++eye) {
        for (float center : value.centerUV[eye])
            if (!std::isfinite(center) || center < 0.0f || center > 1.0f) return false;
        for (float tangent : value.fovTangents[eye])
            if (!std::isfinite(tangent)) return false;
        if (value.fovTangents[eye][1] - value.fovTangents[eye][0] <= 0.001f ||
            value.fovTangents[eye][2] - value.fovTangents[eye][3] <= 0.001f)
            return false;
    }
    return true;
}
} // namespace

void State::BeginFrame(std::int64_t ticks, std::int64_t frequency)
{
    std::lock_guard<std::mutex> guard(mutex);
    snapshot = Disabled(snapshot.frameId + 1, ticks, frequency);
    presentation = snapshot;
    frameOpen = true;
    blackout = {};
}

bool State::Publish(const Snapshot& value, float horizontalScale)
{
    const bool valid = ValidProfile(value);
    std::lock_guard<std::mutex> guard(mutex);
    if (!valid || !frameOpen || snapshot.frameId == 0 || snapshot.qpcFrequency <= 0) {
        snapshot = Disabled(snapshot.frameId, value.publicationQpc, snapshot.qpcFrequency);
        presentation = snapshot;
        return false;
    }
    const auto frame = snapshot.frameId;
    const auto frequency = snapshot.qpcFrequency;
    snapshot = value;
    snapshot.structSize = sizeof(snapshot);
    snapshot.version = Version;
    snapshot.frameId = frame;
    snapshot.qpcFrequency = frequency;
    snapshot.flags = value.gazeSampleTime != 0 ? SourceSampleTimeAvailable : 0;
    std::memset(snapshot.reserved, 0, sizeof(snapshot.reserved));
    presentation = snapshot;
    // V1 renderer consumers understand circular profiles only. Enclose the
    // scene ellipse so existing effect consumers never reduce quality inside
    // its full-quality region. Presentation retains the actual scene radii.
    if (value.mode == Mode::EyeTracked) {
        const float envelope = (std::max)(1.f, ocu_foveation::HorizontalScale(horizontalScale));
        snapshot.innerRadius *= envelope;
        snapshot.midRadius *= envelope;
        if (!ValidProfile(snapshot))
            snapshot = Disabled(frame, value.publicationQpc, frequency);
    }
    return true;
}

Snapshot State::ReadForPresentation(std::int64_t now) const
{
    std::lock_guard<std::mutex> guard(mutex);
    const double age = double(now) - double(presentation.publicationQpc);
    // Fixed rings describe the rendered frame's static profile, not a gaze
    // sample that can become stale while a long frame finishes. The next
    // BeginFrame or a rejected Publish still clears them immediately.
    if (presentation.qpcFrequency <= 0 || age < 0 ||
        (presentation.mode == Mode::EyeTracked &&
            age > double(presentation.qpcFrequency) * 0.5))
        return Disabled(snapshot.frameId, now, snapshot.qpcFrequency);
    return presentation;
}

bool State::LatchBlackout(const ocu_foveation::BlackoutFrame& value)
{
    std::lock_guard<std::mutex> guard(mutex);
    if (!frameOpen || blackout.Active() || !value.Active() || value.frameId != presentation.frameId ||
        presentation.mode != Mode::EyeTracked) return false;
    blackout = value;
    return true;
}

ocu_foveation::BlackoutFrame State::ReadBlackout() const
{
    std::lock_guard<std::mutex> guard(mutex);
    // Unlike a live gaze diagnostic, this describes pixels already omitted
    // from the rendered frame. It must survive Clear(), both submissions and
    // long frames. Only the next BeginFrame replaces this geometry.
    return blackout;
}

void State::Clear(std::int64_t ticks)
{
    std::lock_guard<std::mutex> guard(mutex);
    snapshot = Disabled(snapshot.frameId, ticks, snapshot.qpcFrequency);
    frameOpen = false;
}

Result State::Query(std::uint32_t requestedVersion, std::uint32_t outputBytes,
    Snapshot* output) const
{
    if (requestedVersion != Version) return Result::UnsupportedVersion;
    if (!output || outputBytes < sizeof(Snapshot)) return Result::InvalidOutput;
    Snapshot copy{};
    {
        std::lock_guard<std::mutex> guard(mutex);
        copy = snapshot;
    }
    // Supply a well-formed disabled result before the first game frame too.
    copy.structSize = sizeof(copy);
    copy.version = Version;
    copy.shape = Shape::UVRadialHalfExtent;
    std::memcpy(output, &copy, sizeof(copy));
    return Result::Success;
}

State& GetState()
{
    static State state;
    return state;
}

std::int64_t ClockTicks()
{
#ifdef _WIN32
    LARGE_INTEGER ticks{};
    QueryPerformanceCounter(&ticks);
    return ticks.QuadPart;
#else
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

std::int64_t ClockFrequency()
{
#ifdef _WIN32
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    return frequency.QuadPart;
#else
    return 1000000000;
#endif
}

void BeginFrame() { GetState().BeginFrame(ClockTicks(), ClockFrequency()); }
void Clear() { GetState().Clear(ClockTicks()); }

} // namespace ocu_effect_foveation
