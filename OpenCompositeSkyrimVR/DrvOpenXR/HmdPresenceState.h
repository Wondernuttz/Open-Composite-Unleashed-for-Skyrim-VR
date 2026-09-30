#pragma once

#include <atomic>
#include <mutex>
#include <utility>

// Presence queries can arrive from plugins, including callbacks made by the
// runtime itself. Never start another probe during a probe or lifecycle call.
class HmdPresenceState {
public:
	class RuntimeCall {
	public:
		explicit RuntimeCall(HmdPresenceState& state)
		    : state(state), lock(state.mutex, std::defer_lock), previousOwner(owner)
		{
			// Nested teardown is inside the caller's SEH boundary. Do not take
			// another recursive ownership count that SEH could leave locked.
			if (owner != &state)
				lock.lock();
			previous = state.inRuntimeCall;
			owner = &state;
			state.inRuntimeCall = true;
		}
		~RuntimeCall() { state.inRuntimeCall = previous; owner = previousOwner; }
		bool Nested() const { return previous; }
		RuntimeCall(const RuntimeCall&) = delete;
		RuntimeCall& operator=(const RuntimeCall&) = delete;

	private:
		HmdPresenceState& state;
		std::unique_lock<std::recursive_mutex> lock;
		bool previous;
		HmdPresenceState* previousOwner;
		inline static thread_local HmdPresenceState* owner = nullptr;
	};

	bool IsQuarantined() const { return quarantined.load(std::memory_order_acquire); }
	bool LastKnown() const { return !IsQuarantined() && present.load(std::memory_order_acquire); }
	void Publish(bool value) { present.store(value, std::memory_order_release); }
	void Quarantine() { quarantined.store(true, std::memory_order_release); Publish(false); }

	template <typename Probe>
	bool Query(Probe&& probe)
	{
		if (IsQuarantined())
			return false;
		// A runtime callback may wait for its caller, so waiting here could
		// deadlock startup or shutdown. The cached answer is a presence hint,
		// not a promise that a session is currently ready to render.
		std::unique_lock<std::recursive_mutex> lock(mutex, std::try_to_lock);
		if (!lock.owns_lock() || inRuntimeCall || IsQuarantined())
			return LastKnown();
		RuntimeCall call(*this);
		const bool result = std::forward<Probe>(probe)();
		Publish(result);
		return result;
	}

private:
	std::recursive_mutex mutex;
	bool inRuntimeCall = false; // Protected by mutex, including same-thread reentry.
	std::atomic<bool> present{false};
	std::atomic<bool> quarantined{false};
};
