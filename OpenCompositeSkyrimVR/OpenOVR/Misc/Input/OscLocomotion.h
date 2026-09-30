#pragma once

#include "LocomotionFrame.h"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>

// Local IPC only. This receiver has no dependency on a treadmill vendor SDK.
class OscLocomotion {
public:
    static OscLocomotion& Instance();
    ~OscLocomotion();
    bool Start(int port, double fullSpeedMetersPerSecond);
    void Stop();
    void Invalidate();
    void SuspendHeading();
    bool CalibrateFromControllers();
    bool ControllerCalibrationReady();
    locomotion::Output Read(locomotion::Stick physical, std::optional<double> hmdYaw,
        bool focused, bool paused, std::uint64_t referenceEpoch = 0,
        std::uint64_t headingToken = 0, bool calibrationAllowed = true);
    static std::uint64_t NowMs();

private:
    void Receive();
    std::mutex lifecycle;
    std::mutex stateMutex;
    locomotion::Config config{250, 3.0, 0.05};
    locomotion::Receiver receiver{config};
    std::optional<double> latestHmdYaw;
    std::uint64_t hmdStamp = 0;
    std::uint64_t headingReferenceEpoch = 0, reportedCalibration = 0;
    std::uint64_t latestHeadingToken = 0;
    bool latestCalibrationAllowed = false;
    bool CalibrationHeadingCurrent() const;
    bool eligible = false;
    locomotion::Status lastStatus = locomotion::Status::Empty;
    std::atomic<bool> running{false};
    std::atomic<std::uintptr_t> socketHandle{~std::uintptr_t{0}};
    std::thread worker;
    bool winsockStarted = false;
};
