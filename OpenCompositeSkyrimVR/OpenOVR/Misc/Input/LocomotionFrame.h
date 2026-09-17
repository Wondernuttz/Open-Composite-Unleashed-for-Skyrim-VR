#pragma once

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace locomotion {

inline constexpr std::string_view Address = "/locomotion/v1/frame";
inline constexpr std::string_view TypeTags = ",iiiiifffffff";
constexpr std::size_t Padded(std::size_t size) { return (size + 3u) & ~std::size_t{3}; }
inline constexpr std::size_t AddressBytes = Padded(Address.size() + 1);
inline constexpr std::size_t TypeBytes = Padded(TypeTags.size() + 1);
inline constexpr std::size_t PayloadOffset = AddressBytes + TypeBytes;
inline constexpr std::size_t PacketBytes = PayloadOffset + 12u * 4u;
inline constexpr std::uint32_t Connected = 1;

struct Frame {
    std::uint32_t version = 1;
    std::uint32_t epoch = 0;
    std::uint32_t sequence = 0;
    std::uint32_t flags = 0;
    std::uint32_t calibrationSerial = 0;
    float vx = 0, vy = 0, vz = 0;
    float qx = 0, qy = 0, qz = 0, qw = 1;
};

enum class Error {
    None, Length, Address, TypeTags, Padding, Version, Flags,
    NonFinite, Quaternion, VerticalOrientation, Sequence, CalibrationSerial, Clock
};

inline Error Validate(const Frame& frame) {
    if (frame.version != 1) return Error::Version;
    if ((frame.flags & ~Connected) != 0) return Error::Flags;
    for (const float value : {frame.vx, frame.vy, frame.vz, frame.qx, frame.qy, frame.qz, frame.qw})
        if (!std::isfinite(value)) return Error::NonFinite;
    const double lengthSquared = double(frame.qx) * frame.qx + double(frame.qy) * frame.qy
        + double(frame.qz) * frame.qz + double(frame.qw) * frame.qw;
    if (lengthSquared < 0.81 || lengthSquared > 1.21) return Error::Quaternion;
    const double forwardX = 2.0 * (double(frame.qx) * frame.qz + double(frame.qw) * frame.qy);
    const double forwardZ = lengthSquared - 2.0 * (double(frame.qy) * frame.qy + double(frame.qx) * frame.qx);
    if (std::hypot(forwardX, forwardZ) < 0.01 * lengthSquared) return Error::VerticalOrientation;
    return Error::None;
}

inline void Put32(std::span<std::uint8_t> bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0; i != 4; ++i) bytes[offset + i] = std::uint8_t(value >> (24u - i * 8u));
}

inline std::uint32_t Get32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (unsigned i = 0; i != 4; ++i) value = (value << 8u) | bytes[offset + i];
    return value;
}

// OSC strings are null-terminated and padded to four bytes; payload words are big endian.
inline std::array<std::uint8_t, PacketBytes> Encode(const Frame& frame) {
    std::array<std::uint8_t, PacketBytes> result{};
    for (std::size_t i = 0; i != Address.size(); ++i) result[i] = std::uint8_t(Address[i]);
    for (std::size_t i = 0; i != TypeTags.size(); ++i) result[AddressBytes + i] = std::uint8_t(TypeTags[i]);
    const std::array<std::uint32_t, 12> words{
        frame.version, frame.epoch, frame.sequence, frame.flags, frame.calibrationSerial,
        std::bit_cast<std::uint32_t>(frame.vx), std::bit_cast<std::uint32_t>(frame.vy),
        std::bit_cast<std::uint32_t>(frame.vz), std::bit_cast<std::uint32_t>(frame.qx),
        std::bit_cast<std::uint32_t>(frame.qy), std::bit_cast<std::uint32_t>(frame.qz),
        std::bit_cast<std::uint32_t>(frame.qw)};
    for (std::size_t i = 0; i != words.size(); ++i) Put32(result, PayloadOffset + i * 4, words[i]);
    return result;
}

inline Error Decode(std::span<const std::uint8_t> bytes, Frame& frame) {
    if (bytes.size() != PacketBytes) return Error::Length;
    for (std::size_t i = 0; i != Address.size(); ++i)
        if (bytes[i] != std::uint8_t(Address[i])) return Error::Address;
    for (std::size_t i = Address.size(); i != AddressBytes; ++i)
        if (bytes[i] != 0) return Error::Padding;
    for (std::size_t i = 0; i != TypeTags.size(); ++i)
        if (bytes[AddressBytes + i] != std::uint8_t(TypeTags[i])) return Error::TypeTags;
    for (std::size_t i = TypeTags.size(); i != TypeBytes; ++i)
        if (bytes[AddressBytes + i] != 0) return Error::Padding;
    const auto word = [&](std::size_t index) { return Get32(bytes, PayloadOffset + index * 4); };
    Frame value{};
    value.version = word(0); value.epoch = word(1); value.sequence = word(2);
    value.flags = word(3); value.calibrationSerial = word(4);
    value.vx = std::bit_cast<float>(word(5)); value.vy = std::bit_cast<float>(word(6));
    value.vz = std::bit_cast<float>(word(7)); value.qx = std::bit_cast<float>(word(8));
    value.qy = std::bit_cast<float>(word(9)); value.qz = std::bit_cast<float>(word(10));
    value.qw = std::bit_cast<float>(word(11));
    const auto error = Validate(value);
    if (error == Error::None) frame = value;
    return error;
}

inline bool Newer(std::uint32_t candidate, std::uint32_t previous) {
    const auto distance = candidate - previous;
    return distance != 0 && distance < 0x80000000u;
}

inline double BodyYaw(const Frame& frame) {
    const double norm = double(frame.qx) * frame.qx + double(frame.qy) * frame.qy
        + double(frame.qz) * frame.qz + double(frame.qw) * frame.qw;
    return std::atan2(2.0 * (double(frame.qw) * frame.qy + double(frame.qx) * frame.qz),
        norm - 2.0 * (double(frame.qy) * frame.qy + double(frame.qx) * frame.qx));
}

inline double WrappedYaw(double yaw) { return std::remainder(yaw, 6.283185307179586476925286766559); }

struct Stick { double x = 0, y = 0; };
enum class Status { Empty, Disconnected, Warming, Stale, Uncalibrated, NoHmdPose, Unfocused, Paused, AwaitingFresh, Ready };
enum class Source { None, Physical, Locomotion };
struct Output { Stick stick{}; Source source = Source::None; Status status = Status::Empty; };
struct Config {
    std::uint64_t timeoutMs = 250;
    double fullSpeedMetersPerSecond = 2;
    double physicalDeadzone = 0.05;
};

class Receiver {
public:
    static constexpr std::uint64_t CalibrationRequestTimeoutMs = 5000;
    explicit Receiver(Config config = {}) : config_(config) {
        if (config_.timeoutMs == 0 || config_.timeoutMs > 250) config_.timeoutMs = 250;
        if (!std::isfinite(config_.fullSpeedMetersPerSecond) || config_.fullSpeedMetersPerSecond <= 0)
            config_.fullSpeedMetersPerSecond = 2;
        if (!std::isfinite(config_.physicalDeadzone) || config_.physicalDeadzone < 0 || config_.physicalDeadzone >= 1)
            config_.physicalDeadzone = 0.05;
    }

    Error Ingest(std::span<const std::uint8_t> bytes, std::uint64_t nowMs,
        std::optional<double> hmdYawRadians = {}) {
        Frame frame{};
        const auto error = Decode(bytes, frame);
        return error == Error::None ? Ingest(frame, nowMs, hmdYawRadians) : error;
    }

    Error Ingest(const Frame& frame, std::uint64_t nowMs, std::optional<double> hmdYawRadians = {}) {
        if (const auto error = Validate(frame); error != Error::None) return error;
        if (hasFrame_ && nowMs < arrivalMs_) { ClearCalibration(); return Error::Clock; }
        const bool sameEpoch = hasFrame_ && frame.epoch == last_.epoch;
        // A stop is useful even when sensor time stopped advancing. It cannot refresh arrival time.
        if (sameEpoch && !(frame.flags & Connected)) {
            ClearCalibration(); last_.flags = 0;
            if (!Newer(frame.sequence, last_.sequence)) return Error::None;
        }
        if (sameEpoch && !Newer(frame.sequence, last_.sequence)) return Error::Sequence;
        if (sameEpoch && frame.calibrationSerial != last_.calibrationSerial
            && !Newer(frame.calibrationSerial, last_.calibrationSerial)) return Error::CalibrationSerial;
        const bool freshContinuity = sameEpoch && (last_.flags & Connected)
            && nowMs - arrivalMs_ < config_.timeoutMs;
        const bool calibrationEvent = freshContinuity && frame.calibrationSerial != last_.calibrationSerial;
        if (!freshContinuity || !(frame.flags & Connected)) ClearCalibration();
        warmed_ = freshContinuity && (frame.flags & Connected);
        last_ = frame; arrivalMs_ = nowMs; hasFrame_ = true;
        if (calibrationEvent && warmed_) RequestCalibration(nowMs);
        CompletePendingCalibration(nowMs, hmdYawRadians);
        return Error::None;
    }

    // A physical sensor/SDK request may arrive while the runtime is returning
    // from a menu or recenter. Keep the explicit request briefly; never infer
    // a new calibration merely from a pose or a connection becoming available.
    bool RequestCalibration(std::uint64_t nowMs) {
        if (!Fresh(nowMs) || !warmed_ || !(last_.flags & Connected)) return false;
        calibrated_ = false; calibrationPending_ = true; calibrationRequestMs_ = nowMs;
        return true;
    }
    std::uint64_t CalibrationGeneration() const { return calibrationGeneration_; }
    bool CalibrationPending() const { return calibrationPending_; }
    bool CanCalibrate(std::uint64_t nowMs) const { return Fresh(nowMs) && warmed_ && (last_.flags & Connected); }

    bool Calibrate(std::uint64_t nowMs, double hmdYawRadians) {
        if (!std::isfinite(hmdYawRadians) || !Fresh(nowMs) || !warmed_ || !(last_.flags & Connected)) return false;
        SetCalibration(hmdYawRadians);
        return true;
    }

    void Reset() { *this = Receiver(config_); }
    void InvalidateCalibration() { ClearCalibration(); }
    void InvalidateReference() { calibrated_ = false; yawCorrection_ = 0; }

    Output Mix(Stick physical, std::uint64_t nowMs, std::optional<double> hmdYawRadians,
        bool focused = true, bool paused = false, bool calibrationAllowed = true) {
        Output output{};
        output.status = State(nowMs, hmdYawRadians, focused, paused, calibrationAllowed);
        // Physical controls remain usable for normal controller and menu handling.
        if (!std::isfinite(physical.x) || !std::isfinite(physical.y)) return output;
        if (std::hypot(physical.x, physical.y) > config_.physicalDeadzone) {
            output.stick = physical; output.source = Source::Physical; return output;
        }
        if (output.status != Status::Ready) return output;
        const double angle = BodyYaw(last_) - yawCorrection_ - WrappedYaw(*hmdYawRadians);
        const double sine = std::sin(angle), cosine = std::cos(angle);
        const double x = cosine * last_.vx + sine * last_.vz;
        const double y = sine * last_.vx - cosine * last_.vz;
        const double magnitude = std::hypot(x, y);
        const double divisor = magnitude > config_.fullSpeedMetersPerSecond ? magnitude : config_.fullSpeedMetersPerSecond;
        output.stick = {x / divisor, y / divisor};
        output.source = Source::Locomotion;
        return output;
    }

    Status State(std::uint64_t nowMs, std::optional<double> hmdYawRadians,
        bool focused = true, bool paused = false, bool calibrationAllowed = true) {
        if (!hasFrame_) return Status::Empty;
        if (!Fresh(nowMs)) { ClearCalibration(); return Status::Stale; }
        if (!(last_.flags & Connected)) return Status::Disconnected;
        if (!warmed_) return Status::Warming;
        const auto gate = [&]() { gateHeld_ = true; awaitingFresh_ = true; gatedEpoch_ = last_.epoch; gatedSequence_ = last_.sequence; };
        if (!focused) { gate(); return Status::Unfocused; }
        if (paused) { gate(); return Status::Paused; }
        if (!hmdYawRadians || !std::isfinite(*hmdYawRadians)) { gate(); return Status::NoHmdPose; }
        if (gateHeld_) {
            gateHeld_ = false; gatedEpoch_ = last_.epoch; gatedSequence_ = last_.sequence;
            return Status::AwaitingFresh;
        }
        if (awaitingFresh_) {
            if (last_.epoch == gatedEpoch_ && !Newer(last_.sequence, gatedSequence_)) return Status::AwaitingFresh;
            awaitingFresh_ = false;
        }
        CompletePendingCalibration(nowMs, calibrationAllowed ? hmdYawRadians : std::nullopt);
        if (!calibrated_) return Status::Uncalibrated;
        return Status::Ready;
    }

private:
    bool Fresh(std::uint64_t nowMs) const {
        return hasFrame_ && nowMs >= arrivalMs_ && nowMs - arrivalMs_ < config_.timeoutMs;
    }
    void ClearCalibration() { calibrated_ = false; warmed_ = false; yawCorrection_ = 0; calibrationPending_ = false; }
    void CompletePendingCalibration(std::uint64_t nowMs, std::optional<double> hmdYaw) {
        if (!calibrationPending_) return;
        if (nowMs < calibrationRequestMs_ || nowMs - calibrationRequestMs_ > CalibrationRequestTimeoutMs) {
            calibrationPending_ = false; return;
        }
        if (Fresh(nowMs) && warmed_ && (last_.flags & Connected) && hmdYaw && std::isfinite(*hmdYaw))
            SetCalibration(*hmdYaw);
    }
    void SetCalibration(double hmdYawRadians) {
        yawCorrection_ = BodyYaw(last_) - WrappedYaw(hmdYawRadians); calibrated_ = true;
        calibrationPending_ = false; ++calibrationGeneration_;
    }
    Config config_{};
    Frame last_{};
    std::uint64_t arrivalMs_ = 0;
    std::uint64_t calibrationRequestMs_ = 0, calibrationGeneration_ = 0;
    bool calibrationPending_ = false;
    bool hasFrame_ = false, warmed_ = false, calibrated_ = false;
    bool awaitingFresh_ = false, gateHeld_ = false;
    std::uint32_t gatedEpoch_ = 0, gatedSequence_ = 0;
    double yawCorrection_ = 0;
};

} // namespace locomotion
