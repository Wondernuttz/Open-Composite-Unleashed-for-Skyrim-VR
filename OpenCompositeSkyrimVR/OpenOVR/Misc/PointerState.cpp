#include "PointerState.h"
#include <cmath>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#else
#include <chrono>
#endif

namespace ocu_pointer {
namespace {
bool IsFresh(const Snapshot& value, std::int64_t now)
{
    return value.publicationQpc > 0 && value.qpcFrequency > 0 && now >= value.publicationQpc &&
        double(now - value.publicationQpc) <= double(value.qpcFrequency) * double(MaximumLeaseMilliseconds) / 1000.0;
}
}
void State::Drop(std::uint32_t hands)
{
    guarded |= hands;
    owned &= ~hands;
}
void State::Expire(std::int64_t now)
{
    if (lease.clientToken && (now <= 0 || now >= expires || now < snapshot.publicationQpc)) {
        Drop(owned);
        lease = {};
        acceptedSnapshot = {};
        expires = 0;
    }
    if (!IsFresh(snapshot, now)) {
        Drop(owned);
        for (auto& hand : snapshot.hands) hand.flags &= ~(RayValid | InputAvailable);
    }
}
void State::Decorate(Snapshot& value)
{
    value.structSize = sizeof(value);
    value.version = Version;
    value.clientToken = lease.clientToken;
    value.ownedHands = owned;
    for (int side = 0; side < 2; ++side) {
        value.hands[side].flags &= ~SuppressedUntilRelease;
        if (guarded & (1u << side)) value.hands[side].flags |= SuppressedUntilRelease;
    }
}
Result State::Query(std::uint32_t version, std::uint32_t bytes, Snapshot* output, std::int64_t now)
{
    if (version != Version) return Result::UnsupportedVersion;
    if (!output || bytes < sizeof(Snapshot)) return Result::InvalidArgument;
    std::lock_guard<std::mutex> lock(mutex);
    Expire(now);
    Decorate(snapshot);
    std::memcpy(output, &snapshot, sizeof(snapshot));
    return Result::Success;
}
Result State::Update(std::uint32_t version, std::uint32_t bytes, const LeaseRequest* request,
    std::uint32_t outputBytes, LeaseResponse* output, std::int64_t now)
{
    if (version != Version) return Result::UnsupportedVersion;
    if (!request || bytes < sizeof(LeaseRequest) || !output || outputBytes < sizeof(LeaseResponse))
        return Result::InvalidArgument;
    const LeaseRequest value = *request;
    if (value.structSize != sizeof(value) || value.version != Version || !value.clientToken ||
        value.reserved || (value.flags & ~(MenuVisible | HasLeftHit | HasRightHit)) ||
        (value.handMask & ~3u)) return Result::InvalidArgument;
    if (value.flags && (!(value.flags & MenuVisible) || value.leaseMilliseconds == 0 ||
        value.leaseMilliseconds > MaximumLeaseMilliseconds || value.trackingOrigin > 2 ||
        !std::isfinite(value.pitchTrimDegrees) || std::abs(value.pitchTrimDegrees) > 90.0f))
        return Result::InvalidArgument;
    for (int side = 0; side < 2; ++side)
        if ((value.flags & (HasLeftHit << side)) &&
            (!(value.handMask & (1u << side)) || !std::isfinite(value.hitDistance[side]) ||
                value.hitDistance[side] <= 0.0f || value.hitDistance[side] > MaximumHitDistanceMeters))
            return Result::InvalidArgument;
    std::lock_guard<std::mutex> lock(mutex);
    Expire(now);
    if (lease.clientToken && lease.clientToken != value.clientToken) return Result::Busy;
    if (!value.flags) {
        Drop(owned);
        lease = {};
        acceptedSnapshot = {};
        expires = 0;
    } else {
        const bool bootstrap = value.handMask == 0 && value.sessionId == 0 && value.frameId == 0;
        if (!bootstrap && (value.sessionId != snapshot.sessionId || value.frameId != snapshot.frameId ||
            value.trackingOrigin != snapshot.trackingOrigin || !IsFresh(snapshot, now)))
            return Result::Stale;
        std::uint32_t granted = value.handMask & ~(blocked | nativeCaptured);
        for (int side = 0; side < 2; ++side)
            if ((snapshot.hands[side].flags & (RayValid | InputAvailable)) != (RayValid | InputAvailable))
                granted &= ~(1u << side);
        const bool pitchChanged = snapshot.appliedPitchTrimDegrees != value.pitchTrimDegrees;
        if (pitchChanged) {
            Drop(owned);
            granted = 0;
            for (auto& hand : snapshot.hands) hand.flags &= ~RayValid;
        }
        Drop(owned & ~granted);
        for (int side = 0; side < 2; ++side)
            if ((granted & ~owned & (1u << side)) && (snapshot.hands[side].flags & TriggerDown))
                guarded |= 1u << side;
        owned = granted;
        lease = value;
        acceptedSnapshot = snapshot;
        const auto frequency = PointerClockFrequency();
        expires = now + frequency * value.leaseMilliseconds / 1000;
    }
    Decorate(snapshot);
    *output = { sizeof(LeaseResponse), Version, snapshot.sessionId, snapshot.frameId, owned, 0 };
    return Result::Success;
}
void State::Publish(std::uint64_t newSession, std::uint64_t newSpace, const Snapshot& value)
{
    std::lock_guard<std::mutex> lock(mutex);
    Expire(value.publicationQpc);
    const bool changed = sessionKey != newSession || spaceKey != newSpace ||
        snapshot.trackingOrigin != value.trackingOrigin;
    if (changed) {
        Drop(owned);
        if (sessionKey) { lease = {}; expires = 0; }
        acceptedSnapshot = {};
        sessionKey = newSession;
        spaceKey = newSpace;
        ++epoch;
        ++generation;
        snapshot = {};
    }
    Snapshot next = value;
    next.sessionId = epoch;
    next.frameId = snapshot.frameId + 1;
    for (int side = 0; side < 2; ++side) {
        const bool down = (value.hands[side].flags & TriggerDown) != 0;
        const bool wasDown = (snapshot.hands[side].flags & TriggerDown) != 0;
        next.hands[side].triggerSequence = snapshot.hands[side].triggerSequence + (down != wasDown);
        if (!down && (value.hands[side].flags & InputAvailable)) {
            guarded &= ~(1u << side);
            nativeCaptured &= ~(1u << side);
        }
        if ((value.hands[side].flags & (RayValid | InputAvailable)) != (RayValid | InputAvailable))
            Drop(owned & (1u << side));
    }
    snapshot = next;
    Decorate(snapshot);
}
void State::Reset()
{
    std::lock_guard<std::mutex> lock(mutex);
    Drop(owned);
    snapshot = {};
    acceptedSnapshot = {};
    lease = {};
    expires = 0;
    sessionKey = spaceKey = 0;
    ++generation;
    blocked = nativeCaptured = 0;
}
void State::CaptureNativeHand(int side)
{
    if (side < 0 || side > 1) return;
    std::lock_guard<std::mutex> lock(mutex);
    nativeCaptured |= (1u << side) & ~owned;
}
void State::SetNativeCapturedHands(std::uint32_t hands)
{
    std::lock_guard<std::mutex> lock(mutex);
    nativeCaptured = hands & ~owned & 3u;
}
void State::SetBlockedHands(std::uint32_t hands, std::int64_t now)
{
    std::lock_guard<std::mutex> lock(mutex);
    Expire(now);
    blocked = hands & 3u;
    Drop(owned & blocked);
    Decorate(snapshot);
}
bool State::SuppressNative(int side, bool rawDown, std::int64_t now, bool inputAvailable)
{
    if (side < 0 || side > 1) return false;
    std::lock_guard<std::mutex> lock(mutex);
    Expire(now);
    if (inputAvailable && !rawDown) guarded &= ~(1u << side);
    return ((owned | guarded) & (1u << side)) != 0;
}
Interest State::ReadInterest(std::int64_t now)
{
    std::lock_guard<std::mutex> lock(mutex);
    Expire(now);
    return { lease.clientToken != 0, lease.pitchTrimDegrees, generation };
}
Presentation State::ReadPresentation(std::int64_t now)
{
    std::lock_guard<std::mutex> lock(mutex);
    Expire(now);
    Decorate(snapshot);
    Snapshot render = acceptedSnapshot;
    Decorate(render);
    if (!IsFresh(render, now) || render.sessionId != snapshot.sessionId ||
        render.trackingOrigin != snapshot.trackingOrigin) render.ownedHands = 0;
    return { snapshot, lease, render };
}
State& GetPointerState() { static State value; return value; }
std::int64_t PointerClockTicks()
{
#ifdef _WIN32
    LARGE_INTEGER value{}; QueryPerformanceCounter(&value); return value.QuadPart;
#else
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}
std::int64_t PointerClockFrequency()
{
#ifdef _WIN32
    LARGE_INTEGER value{}; QueryPerformanceFrequency(&value); return value.QuadPart;
#else
    return 1000000000;
#endif
}
}
