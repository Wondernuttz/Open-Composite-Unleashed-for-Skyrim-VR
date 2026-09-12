#pragma once

#include <array>
#include <cstdint>
#include <openxr/openxr.h>

namespace OcuInputTrace {
using Snapshot = std::array<uint64_t, 5>;

// Use per-owner or thread_local gates: no mutex, allocation, or formatting on
// unchanged samples. Rapid changes are coalesced to the latest observed state.
class ChangeGate {
public:
	bool Allow(bool enabled, const Snapshot& value, uint64_t nowMs)
	{
		if (!enabled) { reported = false; return false; }
		if (reported && (value == last || nowMs < nextMs)) return false;
		last = value;
		reported = true;
		nextMs = nowMs + 1000;
		return true;
	}
private:
	Snapshot last{};
	bool reported = false;
	uint64_t nextMs = 0;
};

template<class T> uint64_t Handle(T handle) { return reinterpret_cast<uintptr_t>(handle); }
inline uint64_t Code(XrResult result) { return static_cast<uint64_t>(static_cast<int64_t>(result)); }
inline const char* Result(XrResult result)
{
	switch (result) {
	case XR_SUCCESS: return "SUCCESS";
	case XR_SESSION_NOT_FOCUSED: return "SESSION_NOT_FOCUSED";
	case XR_SESSION_LOSS_PENDING: return "SESSION_LOSS_PENDING";
	case XR_ERROR_ACTIONSET_NOT_ATTACHED: return "ACTIONSET_NOT_ATTACHED";
	case XR_ERROR_SESSION_LOST: return "SESSION_LOST";
	case XR_ERROR_HANDLE_INVALID: return "HANDLE_INVALID";
	default: return "OTHER_SEE_NUMERIC_RESULT";
	}
}

inline const char* State(XrSessionState state)
{
	switch (state) {
	case XR_SESSION_STATE_UNKNOWN: return "UNKNOWN";
	case XR_SESSION_STATE_IDLE: return "IDLE";
	case XR_SESSION_STATE_READY: return "READY";
	case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
	case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
	case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
	case XR_SESSION_STATE_STOPPING: return "STOPPING";
	case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
	case XR_SESSION_STATE_EXITING: return "EXITING";
	default: return "UNRECOGNIZED";
	}
}
}
