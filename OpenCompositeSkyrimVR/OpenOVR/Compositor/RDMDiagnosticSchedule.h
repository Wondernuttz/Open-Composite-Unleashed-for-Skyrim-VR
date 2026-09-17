#pragma once
#include "../LogRateLimit.h"
#include <utility>

// The immediate-context owner schedules collection before drawing and consumes
// its matching report once after disarming, regardless of eye submission order.
class RDMDiagnosticSchedule {
    OcuLogging::RateLimit limiter;
    bool reportPending = false;
public:
    // Menus, missing gaze and skipped submissions must not report an old arm.
    void BeginFrame() { reportPending = false; }

    bool Schedule(uint64_t nowMs, bool detailed) {
        reportPending = limiter.Allow(nowMs, 5000);
        return reportPending && detailed;
    }

    bool ConsumeReport() { return std::exchange(reportPending, false); }
};
