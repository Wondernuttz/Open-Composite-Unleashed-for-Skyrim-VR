#pragma once

#include <openxr/openxr.h>
#include <chrono>
#include <string>
#include <algorithm>
#include <cctype>
#include <atomic>
#include <cstdint>

// Discovery is independent of input focus. Never fabricate focus or pose validity.
namespace OcuInputSession {
inline bool NeedsBridgeStartupRecovery(bool compatibilityPlatform, std::string runtime)
{
	std::transform(runtime.begin(), runtime.end(), runtime.begin(),
	    [](unsigned char c) { return char(std::tolower(c)); });
	return compatibilityPlatform && (runtime.find("monado") != std::string::npos ||
	    runtime.find("wivrn") != std::string::npos);
}

class StartupWait {
public:
	using Clock = std::chrono::steady_clock;
	explicit StartupWait(Clock::time_point now) : deadline(now + std::chrono::seconds(10)) {}
	bool Waiting(Clock::time_point now, XrSessionState state, bool active) const
	{
		return now < deadline && Pending(state, active);
	}
	bool ClaimRecovery(Clock::time_point now, XrSessionState state, bool active, bool compatible)
	{
		if (!compatible || attempted || now < deadline || !Pending(state, active)) return false;
		attempted = true;
		return true;
	}
private:
	static bool Pending(XrSessionState state, bool active)
	{
		return !active && (state == XR_SESSION_STATE_UNKNOWN || state == XR_SESSION_STATE_IDLE);
	}
	Clock::time_point deadline;
	bool attempted = false;
};

inline bool Matches(XrSession current, XrSession eventSession)
{
	return current != XR_NULL_HANDLE && current == eventSession;
}

// A successful xrBeginSession is authoritative even if a bridge drops READY.
// It never overrides an explicitly stopping/lost session or fabricates focus.
inline bool CanQueryProfiles(XrSessionState state, bool begun)
{
	return state == XR_SESSION_STATE_READY || state == XR_SESSION_STATE_SYNCHRONIZED
	    || state == XR_SESSION_STATE_VISIBLE || state == XR_SESSION_STATE_FOCUSED
	    || (begun && (state == XR_SESSION_STATE_UNKNOWN || state == XR_SESSION_STATE_IDLE));
}

// xrSyncActions reports XR_SESSION_NOT_FOCUSED when the runtime withholds input.
// Remember positive evidence briefly, scoped to the attached session, so a lost
// FOCUSED event does not suppress real input that the runtime is already sending.
class SyncFocus {
public:
	void Reset() { session.store(XR_NULL_HANDLE); }
	void Observe(XrSession current, XrResult result, uint64_t nowMs)
	{
		if (current == XR_NULL_HANDLE || result != XR_SUCCESS) { Reset(); return; }
		stamp.store(nowMs);
		session.store(current);
	}
	bool Confirmed(XrSession current, uint64_t nowMs) const
	{
		if (current == XR_NULL_HANDLE || session.load() != current) return false;
		const auto sampleMs = stamp.load();
		return nowMs >= sampleMs && nowMs - sampleMs < 1000 && session.load() == current;
	}
private:
	std::atomic<XrSession> session{ XR_NULL_HANDLE };
	std::atomic<uint64_t> stamp{ 0 };
};

class ProfileRetry {
public:
	using Clock = std::chrono::steady_clock;
	void Request() { pending = true; next = {}; }

	bool Due(Clock::time_point now, XrSessionState state, bool attached,
	    bool leftResolved, bool rightResolved, bool begun = false) const
	{
		// Do not query a destroyed, stopping, or not-yet-bound session.
		const bool runningState = CanQueryProfiles(state, begun);
		return attached && runningState && (pending || !leftResolved || !rightResolved) && now >= next;
	}

	void Complete(Clock::time_point now, bool resolved)
	{
		pending = !resolved;
		next = now + std::chrono::seconds(1);
	}

private:
	bool pending = true;
	Clock::time_point next{};
};
}
