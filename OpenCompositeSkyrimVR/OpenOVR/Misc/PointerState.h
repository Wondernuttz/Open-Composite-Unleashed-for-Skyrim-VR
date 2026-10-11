#pragma once

#include "../../SDK/OCUPointerAPI.h"
#include <mutex>

namespace ocu_pointer {
struct Interest {
    bool active = false;
    float pitch = 0.0f;
    std::uint64_t generation = 0;
};
struct Presentation {
    Snapshot snapshot{};
    LeaseRequest lease{};
    // Accepted ray publication; current arbitration still comes from snapshot.
    Snapshot renderSnapshot{};
};

// CPU-only publication and ownership policy, independent of OpenXR and graphics.
class State {
public:
    Result Query(std::uint32_t version, std::uint32_t bytes, Snapshot* output, std::int64_t now);
    Result Update(std::uint32_t version, std::uint32_t bytes, const LeaseRequest* request,
        std::uint32_t outputBytes, LeaseResponse* output, std::int64_t now);
    void Publish(std::uint64_t sessionKey, std::uint64_t spaceKey, const Snapshot& value);
    void Reset();
    void CaptureNativeHand(int side);
    void SetNativeCapturedHands(std::uint32_t hands);
    void SetBlockedHands(std::uint32_t hands, std::int64_t now);
    bool SuppressNative(int side, bool rawDown, std::int64_t now, bool inputAvailable);
    Interest ReadInterest(std::int64_t now);
    Presentation ReadPresentation(std::int64_t now);
private:
    void Expire(std::int64_t now);
    void Drop(std::uint32_t hands);
    void Decorate(Snapshot& value);
    std::mutex mutex;
    Snapshot snapshot{};
    Snapshot acceptedSnapshot{};
    LeaseRequest lease{};
    std::int64_t expires = 0;
    std::uint64_t sessionKey = 0, spaceKey = 0, epoch = 0, generation = 0;
    std::uint32_t owned = 0, blocked = 0, nativeCaptured = 0, guarded = 0;
};

State& GetPointerState();
std::int64_t PointerClockTicks();
std::int64_t PointerClockFrequency();
// Called once after frame tracking is available, before returning to the game.
void PublishPointerFrame();
void ResetPointerRuntime();
// Inhibit publication until the runtime reference-space change takes effect.
void SchedulePointerReferenceChange(std::int64_t time);
// Destroy session-owned graphics before the OpenXR session is destroyed.
void ReleasePointerRenderer();
}
