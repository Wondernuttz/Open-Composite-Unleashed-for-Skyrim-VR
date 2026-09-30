#ifndef OCU_RUNTIME_SELF_TEST
#include "stdafx.h"
#else
#define OOVR_LOGF(...) ((void)0)
#endif
#include "OscLocomotion.h"
#include "LocomotionHeading.h"
#include <array>
#include <chrono>
#include <cmath>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

inline const char* LocomotionStatusName(locomotion::Status status) {
    switch (status) {
    case locomotion::Status::Empty: return "waiting-for-sender";
    case locomotion::Status::Disconnected: return "device-disconnected";
    case locomotion::Status::Warming: return "waiting-for-continuous-data";
    case locomotion::Status::Stale: return "stale-data";
    case locomotion::Status::Uncalibrated: return "heading-calibration-required";
    case locomotion::Status::NoHmdPose: return "head-pose-unavailable";
    case locomotion::Status::Unfocused: return "runtime-input-unfocused";
    case locomotion::Status::Paused: return "gameplay-blocked";
    case locomotion::Status::AwaitingFresh: return "waiting-for-fresh-data-after-resume";
    case locomotion::Status::Ready: return "ready";
    }
    return "unknown";
}

OscLocomotion& OscLocomotion::Instance() { static OscLocomotion instance; return instance; }
OscLocomotion::~OscLocomotion() { Stop(); }
std::uint64_t OscLocomotion::NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool OscLocomotion::Start(int port, double fullSpeedMetersPerSecond) {
    std::lock_guard lock(lifecycle);
    if (running.load()) return true;
#ifdef _WIN32
    if (port < 1024 || port > 65535 || !std::isfinite(fullSpeedMetersPerSecond)
        || fullSpeedMetersPerSecond <= 0.0) {
        OOVR_LOGF("Locomotion: invalid port or speed configuration; receiver disabled");
        return false;
    }
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) { WSACleanup(); return false; }
    BOOL exclusive = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<u_short>(port));
    if (bind(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        OOVR_LOGF("Locomotion: cannot bind 127.0.0.1:%d (error %d)", port, WSAGetLastError());
        closesocket(s); WSACleanup(); return false;
    }
    DWORD timeout = 100;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    {
        std::lock_guard stateLock(stateMutex);
        config = {250, fullSpeedMetersPerSecond, 0.05};
        receiver = locomotion::Receiver(config);
        latestHmdYaw.reset(); hmdStamp = 0; eligible = false;
        headingReferenceEpoch = 0; reportedCalibration = 0;
        latestHeadingToken = 0; latestCalibrationAllowed = false;
    }
    winsockStarted = true;
    socketHandle.store(static_cast<std::uintptr_t>(s));
    running.store(true);
    try { worker = std::thread(&OscLocomotion::Receive, this); }
    catch (...) {
        running.store(false); socketHandle.store(~std::uintptr_t{0});
        closesocket(s); WSACleanup(); winsockStarted = false; return false;
    }
    OOVR_LOGF("Locomotion: listening on 127.0.0.1:%d; HMD-relative game movement and heading calibration required", port);
    return true;
#else
    (void)port; (void)fullSpeedMetersPerSecond;
    return false;
#endif
}

void OscLocomotion::Stop() {
    std::lock_guard lock(lifecycle);
    running.store(false);
#ifdef _WIN32
    const auto s = static_cast<SOCKET>(socketHandle.exchange(~std::uintptr_t{0}));
    if (s != INVALID_SOCKET) closesocket(s);
#endif
    if (worker.joinable()) worker.join();
#ifdef _WIN32
    if (winsockStarted) { WSACleanup(); winsockStarted = false; }
#endif
    Invalidate();
}

void OscLocomotion::Invalidate() {
    std::lock_guard lock(stateMutex);
    receiver.InvalidateCalibration();
    latestHmdYaw.reset(); hmdStamp = 0; eligible = false;
    headingReferenceEpoch = 0;
    latestHeadingToken = 0; latestCalibrationAllowed = false;
    lastStatus = locomotion::Status::Empty;
}

void OscLocomotion::SuspendHeading() {
    std::lock_guard lock(stateMutex);
    latestHmdYaw.reset(); hmdStamp = 0; eligible = false;
    latestHeadingToken = 0; latestCalibrationAllowed = false;
}

locomotion::Output OscLocomotion::Read(locomotion::Stick physical,
    std::optional<double> hmdYaw, bool focused, bool paused, std::uint64_t referenceEpoch,
    std::uint64_t headingToken, bool calibrationAllowed) {
    const auto now = NowMs();
    std::unique_lock lock(stateMutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        locomotion::Output result{};
        result.status = locomotion::Status::Paused;
        if (std::isfinite(physical.x) && std::isfinite(physical.y)) {
            result.stick = physical;
            result.source = physical.x != 0 || physical.y != 0 ?
                locomotion::Source::Physical : locomotion::Source::Locomotion;
        }
        return result; // No waiting on the receiver thread; no synthetic fallback on contention.
    }
    if ((referenceEpoch && headingReferenceEpoch && referenceEpoch < headingReferenceEpoch) ||
        (headingToken && !OcuLocomotionHeading::Instance().SampleCurrent(referenceEpoch, headingToken))) {
        // Concurrent input readers can deliver old snapshots after a newer
        // reference was installed. Never roll back or erase its calibration.
        locomotion::Output result{};
        result.status = locomotion::Status::NoHmdPose;
        if (std::isfinite(physical.x) && std::isfinite(physical.y) &&
            std::hypot(physical.x, physical.y) > config.physicalDeadzone) {
            result.stick = physical; result.source = locomotion::Source::Physical;
        }
        return result;
    }
    const bool referenceReset = referenceEpoch && headingReferenceEpoch && referenceEpoch != headingReferenceEpoch;
    if (referenceReset) receiver.InvalidateReference();
    if (referenceEpoch) headingReferenceEpoch = referenceEpoch;
    latestHeadingToken = headingToken;
    latestCalibrationAllowed = calibrationAllowed;
    eligible = focused && !paused;
    latestHmdYaw = eligible ? hmdYaw : std::nullopt;
    hmdStamp = now;
    auto result = receiver.Mix(physical, now, hmdYaw, focused, paused, CalibrationHeadingCurrent());
    const bool changed = result.status != lastStatus;
    lastStatus = result.status;
    const auto calibration = receiver.CalibrationGeneration();
    const bool calibrated = calibration != reportedCalibration;
    reportedCalibration = calibration;
    lock.unlock();
    if (referenceReset) OOVR_LOGF("Locomotion: unknown STAGE transform/session changed; explicit heading alignment required");
    if (calibrated) OOVR_LOGF("Locomotion: heading calibration completed (generation=%llu)", (unsigned long long)calibration);
    if (changed) {
        OOVR_LOGF("Locomotion: %s state=%u selected=%u axis=(%.3f,%.3f)",
            LocomotionStatusName(result.status), static_cast<unsigned>(result.status),
            static_cast<unsigned>(result.source), result.stick.x, result.stick.y);
    }
    return result;
}

bool OscLocomotion::CalibrateFromControllers() {
    const auto now = NowMs();
    std::unique_lock lock(stateMutex, std::try_to_lock);
    if (!lock.owns_lock() || !eligible || !latestHmdYaw || !CalibrationHeadingCurrent() || now < hmdStamp || now - hmdStamp > 100) return false;
    return receiver.Calibrate(now, *latestHmdYaw);
}

bool OscLocomotion::ControllerCalibrationReady() {
    const auto now = NowMs();
    std::unique_lock lock(stateMutex, std::try_to_lock);
    return lock.owns_lock() && eligible && latestHmdYaw && CalibrationHeadingCurrent() && now >= hmdStamp && now - hmdStamp <= 100 &&
        receiver.CanCalibrate(now);
}

bool OscLocomotion::CalibrationHeadingCurrent() const {
    return latestCalibrationAllowed && (!headingReferenceEpoch ||
        OcuLocomotionHeading::Instance().CalibrationSampleCurrent(headingReferenceEpoch, latestHeadingToken));
}

void OscLocomotion::Receive() {
#ifdef _WIN32
    std::array<std::uint8_t, 2048> bytes{};
    std::uint64_t lastRejectedLog = 0;
    std::uint64_t lastCalibrationWaitLog = 0;
    while (running.load()) {
        const auto s = static_cast<SOCKET>(socketHandle.load());
        if (s == INVALID_SOCKET) break;
        sockaddr_in source{}; int sourceLength = sizeof(source);
        const int length = recvfrom(s, reinterpret_cast<char*>(bytes.data()), static_cast<int>(bytes.size()),
            0, reinterpret_cast<sockaddr*>(&source), &sourceLength);
        if (length <= 0 || !running.load()) continue;
        if (source.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) continue;
        const auto now = NowMs();
        locomotion::Error error;
        bool calibrationQueued = false, calibrationCanceled = false;
        {
            std::lock_guard lock(stateMutex);
            const auto yaw = eligible && CalibrationHeadingCurrent() && now >= hmdStamp && now - hmdStamp <= 100 ? latestHmdYaw : std::nullopt;
            const bool wasPending = receiver.CalibrationPending();
            const auto calibration = receiver.CalibrationGeneration();
            error = receiver.Ingest(std::span<const std::uint8_t>(bytes.data(), length), now, yaw);
            calibrationQueued = !wasPending && receiver.CalibrationPending();
            calibrationCanceled = wasPending && !receiver.CalibrationPending() && calibration == receiver.CalibrationGeneration();
        }
        if ((calibrationQueued || calibrationCanceled) && now - lastCalibrationWaitLog >= 1000) {
            OOVR_LOGF("Locomotion: sensor calibration %s", calibrationQueued ?
                "requested; waiting up to 5 seconds for an eligible headset pose" :
                "expired or canceled by a sensor discontinuity; request alignment again");
            lastCalibrationWaitLog = now;
        }
        if (error != locomotion::Error::None && now - lastRejectedLog >= 5000) {
            OOVR_LOGF("Locomotion: rejected frame (reason=%u); rejected data does not refresh movement", static_cast<unsigned>(error));
            lastRejectedLog = now;
        }
    }
#endif
}
