#pragma once
#include <atomic>
#include <cstdint>

// Input thread -> render thread mailbox. Never touch capture/D3D state from input.
namespace DapaCaptureControl {
// Production builds must never record when users grab with both hands.
// Preserve the diagnostic tooling behind an explicit developer build option.
#if defined(OCU_DAPA_CAPTURE) && OCU_DAPA_CAPTURE
inline constexpr bool Enabled = true;
#else
inline constexpr bool Enabled = false;
#endif
inline std::atomic<bool> toggleRequested{false};
inline std::atomic<bool> recording{false};
inline std::atomic<bool> telemetryWanted{false};
inline std::atomic<int> feedback{0}; // 1 started, 2 stopped, 3 unavailable
inline constexpr unsigned MaxPhotoAttempts=8;
inline constexpr unsigned MaxMovementSamples=16384;
inline bool PhotoBudgetReached(unsigned attempts,unsigned failures) { return attempts>=MaxPhotoAttempts || failures>=3; }
inline bool SessionLimitReached(uint64_t elapsedMs,unsigned,bool) {
    return elapsedMs>=120000; // photo failures/budget never silently end movement recording
}

// Input-tick driven, no sleeps: one pulse=start, two=stop, three=unavailable.
struct FeedbackPulses {
    unsigned remaining=0;uint64_t next=0;
    bool Update(bool active,int event,uint64_t now) {
        if(!active){remaining=0;return false;}
        if(event>0){remaining=event<=3?unsigned(event):3;next=now;}
        if(!remaining || now<next)return false;
        --remaining;next=now+250;return true;
    }
};

struct GripLatch {
    bool released=false, pending=false;
    uint64_t since=0, last=0;
    bool Update(bool active,bool left,bool right,uint64_t now) {
        if(!active || (last && now-last>500)) { released=false;pending=false; }
        last=now;
        if(!active)return false;
        if(!left&&!right) { released=true;pending=false;return false; }
        if(!released || !left || !right) { pending=false;return false; }
        if(!pending) { pending=true;since=now;return false; }
        if(now-since<150)return false;
        released=false;pending=false;return true;
    }
};
}
