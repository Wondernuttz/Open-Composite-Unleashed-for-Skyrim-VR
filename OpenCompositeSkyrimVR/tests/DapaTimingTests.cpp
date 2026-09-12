#include "DrvOpenXR/DapaTiming.h"
#include <cstdio>
#include <stdexcept>
#include <limits>

static void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static int acquireCalls, waitCalls, releaseCalls;
static XrResult waitResult = XR_SUCCESS, releaseResult = XR_SUCCESS;
static XrDuration observedBudget;
static XrResult XRAPI_CALL Acquire(XrSwapchain, const XrSwapchainImageAcquireInfo*, uint32_t* index)
{ ++acquireCalls; *index = 2; return XR_SUCCESS; }
static XrResult XRAPI_CALL Wait(XrSwapchain, const XrSwapchainImageWaitInfo* info)
{ ++waitCalls; observedBudget = info->timeout; return waitResult; }
static XrResult XRAPI_CALL Release(XrSwapchain, const XrSwapchainImageReleaseInfo*)
{ ++releaseCalls; return releaseResult; }

int main()
{
	try {

		using Clock = DapaTiming::Clock;
		const auto at = [](double ms) { return Clock::time_point{} +
			std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(ms)); };
		for (double hz : {60., 72., 80., 90., 96., 100., 120., 144.}) {
			const double period = 1000.0 / hz;
			Check(std::abs(DapaTiming::StallLimitMs(period) / period - 2.7) < 1e-9, "refresh-relative stall limit");
			Check(DapaTiming::ImageWaitBudget(period) > 0 && DapaTiming::ImageWaitBudget(period) <= 2000000, "bounded image wait");
			const double engageInterval = std::max(1000.0 / DapaTiming::AutoEngageFps(50, period), 1.15 * period);
			Check(engageInterval < 2.05 * period, "auto engagement band survives every tested refresh");
			DapaTiming::PeriodBaseline baseline;
			baseline.Observe(period);
			DapaTiming::PacingGuard pacing;
			for (int i = 0; i < 100; ++i) {
				Check(!pacing.Observe(period, period, 12, period, at(i*100)), "routine slot pacing is not failure");
				Check(!pacing.Observe(period, 1.5 * period, 12, period, at(i*100)), "isolated moderate spike must not park DAPA");
			}
			pacing = {};
			Check(!pacing.Observe(period, 1.5 * period, 12, period, at(10)), "first slow end tolerated");
			Check(!pacing.Observe(period, 1.5 * period, 12, period, at(20)), "second slow end tolerated");
			Check(pacing.Observe(period, 1.5 * period, 12, period, at(30)), "sustained pressure yields briefly");
			Check(std::abs(pacing.backoffMs - std::min(25.0, 2 * period)) < 1e-8, "yield bounded to two baseline slots and 25ms");
			Check(pacing.HoldRealFrame(at(31)), "first real frame may get a yield");
			Check(!pacing.HoldRealFrame(at(32)), "second real frame retries even before deadline");
			for (int i = 0; i < 100; ++i) {
				const double now = 1000.0 + i*200;
				baseline.Observe(period * (1 + i % 6));
				Check(std::abs(baseline.Get() - period) < 1e-8, "missed-slot cadence cannot inflate thresholds");
				Check(pacing.Observe(0, 8 * period, 12, baseline.Get(), at(now)), "multi-slot accepted end yields");
				Check(pacing.backoffMs <= 25.0, "chronic pressure never escalates hold");
				Check(pacing.HoldRealFrame(at(now + 1)), "pressure survives cadence change");
				Check(!pacing.HoldRealFrame(at(now + 2)), "chronic pressure still retries next real frame");
			}
			pacing = {};
			Check(pacing.Observe(3 * period, 0, 0, period, at(0)), "wait stall protection survives disabled end threshold");
			Check(!pacing.HoldRealFrame(at(30)), "no extra skipped frame after deadline has already elapsed");
			pacing = {};
			Check(!pacing.Observe(period, 100, 0, period, at(0)), "explicit zero disables end pressure policy");
			DapaTiming::Recovery recovery;
			recovery.Trouble(at(137.7));
			recovery.Advance(155.7, at(155.7));
			Check(std::abs(recovery.backoffMs - 162.0) < 1e-6, "error cooldown starts after triggering stall");
			recovery.Advance(162.0, at(317.7));
			Check(recovery.backoffMs == 0.0, "error cooldown expires at its deadline");
			for (int i = 0; i < 20; ++i) recovery.Trouble(at(500));
			Check(recovery.level == 3 && recovery.backoffMs == 11380.0, "actual API failures retain bounded recovery");
			recovery.Advance(12000.0, at(12500));
			Check(recovery.backoffMs == 0, "slow frame cannot prolong error cooldown");
			recovery.CleanInjection(10000.0);
			Check(recovery.level == 2, "clean API submissions de-escalate error recovery");
			recovery.engageHoldMs = 120000;
			recovery.Advance(121000, at(133500));
			Check(recovery.dwellMs > recovery.engageHoldMs, "maximum dwell remains recoverable");
			recovery.Trouble(at(134000));
			recovery.Clear();
			recovery.Advance(1, at(134001));
			Check(recovery.backoffMs == 0, "cleared cooldown cannot resurrect from old deadline");
			std::printf("DAPA %g Hz fluctuation/quick-recovery PASS\n", hz);
		}
		DapaTiming::PacingGuard observed;
		DapaTiming::Recovery errors;
		// PSVR trace: a long end followed by another real frame 18ms later.
		observed.Observe(0, 137.7, 12, 11.122, at(137.7));
		Check(observed.HoldRealFrame(at(155.7)), "137.7ms trigger cannot consume the newly-created pacing hold");
		Check(observed.backoffMs > 4 && observed.backoffMs < 5, "only post-trigger elapsed time is deducted");
		Check(!observed.HoldRealFrame(at(156.7)), "one yielded real frame is enough for retry");
		for (double endMs : {19.2, 64.7, 50.3, 16.0, 12.6, 13.1, 15.7, 1.0}) {
			observed.Observe(9.6, endMs, 12, 1000.0 / 90.0, at(1000));
			Check(observed.backoffMs <= 25 && errors.backoffMs == 0, "accepted log replay never trips API-error recovery");
		}
		DapaTiming::PeriodBaseline change;
		change.Observe(1000./90.);
		change.Observe(std::numeric_limits<double>::quiet_NaN());
		change.Observe(-10);
		Check(std::abs(change.Get() - 1000./90.) < 1e-8, "invalid period cannot poison baseline");
		change.Observe(1000./120.);
		Check(std::abs(change.Get() - 1000./120.) < 1e-8, "faster runtime cadence updates baseline");
		change = {};
		change.Observe(1000./72.);
		Check(std::abs(change.Get() - 1000./72.) < 1e-8, "new session can learn a slower baseline");
		Check(DapaTiming::Accepted(XR_SUCCESS), "successful submission counted");
		Check(!DapaTiming::Accepted(XR_ERROR_RUNTIME_FAILURE), "failed submission not counted");
		Check(!DapaTiming::Accepted(XR_SESSION_LOSS_PENDING), "loss pending not counted as healthy");
		Check(std::isfinite(DapaTiming::PeriodMs(std::numeric_limits<double>::quiet_NaN())), "invalid period fallback");
		DapaTiming::ImageLease lease;
		waitResult = XR_TIMEOUT_EXPIRED;
		Check(lease.Wait(XR_NULL_HANDLE, 2000000, Acquire, Wait) == XR_TIMEOUT_EXPIRED, "timeout detection");
		Check(lease.acquired && !lease.writable && acquireCalls == 1, "timeout retains ownership");
		Check(lease.Release(XR_NULL_HANDLE, Release) == XR_ERROR_CALL_ORDER_INVALID && releaseCalls == 0, "no early release");
		for (int i = 0; i < 1000; ++i)
			Check(lease.Wait(XR_NULL_HANDLE, 0, Acquire, Wait) == XR_TIMEOUT_EXPIRED, "repeated timeout");
		Check(acquireCalls == 1 && observedBudget == 0, "retry same image without queue growth");
		waitResult = XR_SUCCESS;
		Check(lease.Wait(XR_NULL_HANDLE, 1000, Acquire, Wait) == XR_SUCCESS && lease.index == 2, "recovery preserves image index");
		Check(lease.Release(XR_NULL_HANDLE, Release) == XR_SUCCESS && !lease.acquired, "release after successful wait");
		Check(lease.Wait(XR_NULL_HANDLE, 1000, Acquire, Wait) == XR_SUCCESS && acquireCalls == 2, "next acquire after release");
		releaseResult = XR_ERROR_RUNTIME_FAILURE;
		Check(lease.Release(XR_NULL_HANDLE, Release) == XR_ERROR_RUNTIME_FAILURE, "release error");
		const int waitsBefore = waitCalls;
		Check(lease.Wait(XR_NULL_HANDLE, 0, Acquire, Wait) == XR_ERROR_RUNTIME_FAILURE && waitCalls == waitsBefore, "no reuse after failed release");
		lease = {};
		Check(!lease.acquired && !lease.writable && lease.failure == XR_SUCCESS, "resize/shutdown reset");
		waitResult = XR_SESSION_LOSS_PENDING;
		Check(lease.Wait(XR_NULL_HANDLE, 1000, Acquire, Wait) == XR_SESSION_LOSS_PENDING && !lease.writable, "positive session loss status is not a ready image");
		lease = {};
		waitResult = XR_ERROR_SESSION_LOST;
		Check(lease.Wait(XR_NULL_HANDLE, 1000, Acquire, Wait) == XR_ERROR_SESSION_LOST && !lease.writable, "session loss cannot publish an image");
		std::puts("DAPA ownership/recovery PASS");
	} catch (const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
}
