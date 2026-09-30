#include "OpenOVR/Compositor/RDMDiagnosticSchedule.h"
#include <cstdio>
#include <stdexcept>

namespace {
unsigned checks = 0;
void Require(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
}

int main() {
    try {
        RDMDiagnosticSchedule schedule;
        unsigned captures = 0, reports = 0;
        for (uint64_t now = 0; now <= 10000; now += 10) {
            schedule.BeginFrame();
            const bool capture = schedule.Schedule(now, true);
            captures += capture;
            const bool report = schedule.ConsumeReport();
            reports += report;
            Require(report == capture, "scheduled capture did not produce its matching report");
            Require(!schedule.ConsumeReport(), "second eye repeated the report");
        }
        Require(captures == 3 && reports == 3, "detailed collection did not follow the five-second cadence");

        RDMDiagnosticSchedule basic;
        basic.BeginFrame();
        Require(!basic.Schedule(100, false), "debug-off frame requested descriptor capture");
        Require(basic.ConsumeReport(), "debug-off frame lost basic counters");
        basic.BeginFrame();
        Require(!basic.Schedule(5099, true), "debug toggle bypassed collection throttle");
        Require(!basic.ConsumeReport(), "unsampled frame requested a report");
        basic.BeginFrame();
        Require(basic.Schedule(5100, true), "detailed capture missed the exact interval");
        Require(basic.ConsumeReport(), "detailed capture lost its basic report");

        RDMDiagnosticSchedule skipped;
        skipped.BeginFrame();
        Require(skipped.Schedule(0, true), "first active arm was not sampled");
        skipped.BeginFrame();
        Require(!skipped.ConsumeReport(), "menu or skipped-arm frame reported stale captured state");
        skipped.BeginFrame();
        Require(!skipped.Schedule(1, true), "empty or failed arm caused repeated descriptor collection");
        Require(!skipped.ConsumeReport(), "failed-frame retry scheduled an early report");
        skipped.BeginFrame();
        Require(skipped.Schedule(5000, true), "resumed active arm missed next scheduled sample");
        Require(skipped.ConsumeReport(), "resumed active arm lost its report");
        skipped.BeginFrame();
        Require(!skipped.ConsumeReport(), "disabled foveation reported retained counters as a fresh frame");

        RDMDiagnosticSchedule otherCompositor;
        otherCompositor.BeginFrame();
        Require(otherCompositor.Schedule(5001, true), "another compositor inherited an unrelated throttle");
        Require(otherCompositor.ConsumeReport(), "another compositor lost its own report");
        std::printf("PASS: %u checks; 1001 frames collect and report exactly three times, once per stereo frame; debug-off counters and skipped-frame isolation retained.\n", checks);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
