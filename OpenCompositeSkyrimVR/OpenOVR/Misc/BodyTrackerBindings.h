#pragma once

#include <openxr/openxr.h>

// Suggested bindings become immutable with their action set's first attachment.
// Keep the result with the action handles, across session replacement; only an
// actual action-set rebuild permits another preparation pass.
class OcuBodyTrackerBindings {
public:
	bool NeedsSuggestion() const { return !prepared; }
	bool HasPoseBindings() const { return prepared && XR_SUCCEEDED(result); }
	bool HasHapticBindings() const { return HasPoseBindings() && haptics; }
	void Reset() { *this = {}; }

	template <class Suggest>
	XrResult Prepare(bool includeHaptics, Suggest&& suggest)
	{
		if (prepared)
			return result;
		prepared = true;
		auto attempt = [&](bool withHaptics) {
			XrResult value = suggest(withHaptics);
			// One bounded retry while the set is still mutable. Unsupported paths
			// are deterministic and are never retried unchanged.
			if (value == XR_ERROR_RUNTIME_FAILURE)
				value = suggest(withHaptics);
			return value;
		};
		haptics = includeHaptics;
		result = attempt(haptics);
		if (XR_FAILED(result) && haptics) {
			haptics = false;
			result = attempt(false);
		}
		return result;
	}

private:
	bool prepared = false;
	bool haptics = false;
	XrResult result = XR_ERROR_ACTIONSET_NOT_ATTACHED;
};
