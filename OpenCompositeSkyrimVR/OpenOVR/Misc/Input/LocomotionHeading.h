#pragma once

#include <atomic>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <optional>

// Input must not locate the HMD or touch session-owned objects. Observe the
// existing head-space sample instead, and expire it when rendering stops.
class OcuLocomotionHeading {
public:
    static constexpr std::uint64_t MaxAgeMs = 100;
    struct Snapshot {
        bool focused = false;
        std::optional<double> yaw;
        std::uint64_t referenceEpoch = 0, token = 0;
        bool calibrationAllowed = false;
    };
    // Backend teardown can run after ordinary function-static destructors.
    static OcuLocomotionHeading& Instance() { static auto* cache = new OcuLocomotionHeading; return *cache; }
    static std::uint64_t NowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void Reset() {
        focused.store(false, std::memory_order_release);
        paused.store(true, std::memory_order_release);
        Invalidate();
        std::lock_guard lock(mutex);
        pendingCount = 0; referenceYaw = 0; latestPoseTime = 0;
        referenceEpoch.fetch_add(1, std::memory_order_acq_rel);
        referenceChangesPending.store(false, std::memory_order_release);
    }
    void Invalidate() { generation.fetch_add(1, std::memory_order_acq_rel); }
    void SetFocused(bool value) {
        if (focused.exchange(value, std::memory_order_acq_rel) != value) Invalidate();
    }
    std::uint64_t CaptureToken() const {
        const auto token = generation.load(std::memory_order_acquire);
        return focused.load(std::memory_order_acquire) && !paused.load(std::memory_order_acquire) ? token : 0;
    }
    bool SampleCurrent(std::uint64_t epoch, std::uint64_t token) const {
        return token && token == CaptureToken() && epoch == referenceEpoch.load(std::memory_order_acquire);
    }
    bool CalibrationSampleCurrent(std::uint64_t epoch, std::uint64_t token) const {
        return !referenceChangesPending.load(std::memory_order_acquire) && SampleCurrent(epoch, token);
    }

    // Called only for STAGE: the treadmill observes VIEW in the standing
    // floor space. OpenXR's new-origin pose is expressed in the OLD space.
    // Keep a stable heading coordinate by adding the new origin's yaw, but
    // only to poses whose locate time has reached the event's changeTime.
    bool QueueStageChange(std::int64_t changeTime, bool poseValid,
        double x, double y, double z, double w) {
        const double norm = x*x + y*y + z*z + w*w;
        const bool knownYaw = poseValid && std::isfinite(norm) && norm >= .81 && norm <= 1.21 &&
            std::abs(x) <= .001 && std::abs(z) <= .001 && changeTime > 0;
        std::lock_guard lock(mutex);
        referenceChangesPending.store(true, std::memory_order_release);
        Invalidate(); // A sample started before notification cannot publish after it.
        StageChange change{changeTime, knownYaw && changeTime > latestPoseTime,
            knownYaw ? std::atan2(2*w*y, norm-2*y*y) : 0};
        if (pendingCount == pending.size()) {
            // Bounded storage, fail closed if a runtime floods space changes.
            change = {pending[0].time < changeTime ? pending[0].time : changeTime, false, 0};
            pendingCount = 0;
        }
        auto index = pendingCount++;
        while (index && pending[index-1].time > change.time) {
            pending[index] = pending[index-1]; --index;
        }
        pending[index] = change;
        return change.known;
    }

    // Quaternion is the actual VIEW-space orientation in the locate base,
    // never an eye orientation (which may include optical cant).
    void Publish(std::uint64_t token, std::uint64_t nowMs, bool orientationValid,
        double x, double y, double z, double w, std::int64_t poseTime = 0) {
        if (!token) return;
        const auto reject = [&] { auto expected = token; generation.compare_exchange_strong(
            expected, token + 1, std::memory_order_acq_rel); };
        if (!orientationValid || !std::isfinite(x) || !std::isfinite(y) ||
            !std::isfinite(z) || !std::isfinite(w)) { reject(); return; }
        const double norm = x*x + y*y + z*z + w*w;
        if (norm < 0.81 || norm > 1.21) { reject(); return; }
        const double forwardX = 2.0 * (x*z + w*y);
        const double forwardZ = norm - 2.0 * (y*y + x*x);
        if (std::hypot(forwardX, forwardZ) <= 0.001 * norm) { reject(); return; }
        std::unique_lock lock(mutex, std::try_to_lock);
        if (!lock.owns_lock() || CaptureToken() != token) return;
        if (poseTime < latestPoseTime) return; // Do not replay an old-space pose.
        latestPoseTime = poseTime;
        while (pendingCount && poseTime >= pending[0].time) {
            if (pending[0].known) referenceYaw = std::remainder(referenceYaw + pending[0].yaw, 6.283185307179586);
            else { referenceYaw = 0; referenceEpoch.fetch_add(1, std::memory_order_acq_rel); }
            --pendingCount;
            for (std::size_t i = 0; i < pendingCount; ++i) pending[i] = pending[i+1];
        }
        sample = {token, nowMs, std::remainder(std::atan2(forwardX, forwardZ) + referenceYaw, 6.283185307179586)};
        referenceChangesPending.store(pendingCount != 0, std::memory_order_release);
    }

    Snapshot Read(std::uint64_t nowMs, bool blocked) {
        // Both edges need a fresh sample; a menu-era heading cannot be replayed
        // when Skyrim resumes querying its movement action.
        if (paused.exchange(blocked, std::memory_order_acq_rel) != blocked) Invalidate();
        Snapshot result{focused.load(std::memory_order_acquire), {}, referenceEpoch.load(std::memory_order_acquire)};
        if (blocked || !result.focused) return result;
        const auto token = CaptureToken();
        std::unique_lock lock(mutex, std::try_to_lock);
        if (lock.owns_lock() && token && sample.generation == token &&
            nowMs >= sample.time && nowMs - sample.time <= MaxAgeMs && CaptureToken() == token)
        {
            result.yaw = sample.yaw;
            result.referenceEpoch = referenceEpoch.load(std::memory_order_acquire);
            result.token = token;
            result.calibrationAllowed = pendingCount == 0;
        }
        return result;
    }

private:
    std::atomic<std::uint64_t> generation{1};
    std::atomic<bool> focused{false}, paused{true};
    std::mutex mutex;
    struct StageChange { std::int64_t time; bool known; double yaw; };
    std::array<StageChange, 8> pending{};
    std::size_t pendingCount = 0;
    std::int64_t latestPoseTime = 0;
    double referenceYaw = 0;
    std::atomic<std::uint64_t> referenceEpoch{1};
    std::atomic<bool> referenceChangesPending{false};
    struct { std::uint64_t generation = 0, time = 0; double yaw = 0; } sample;
};
