#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

// A bridge view is published once and retained for the runtime's lifetime.
// Only the initializer owns provisional views; readers never see a rejected
// view or race its cleanup. Connected readers take no initialization lock.
template <typename Bridge>
class PublishedBridge {
public:
	static_assert(std::atomic<Bridge*>::is_always_lock_free,
	    "Connected bridge readers require lock-free pointer loads");
	Bridge* Get() const { return published.load(std::memory_order_acquire); }
	explicit operator bool() const { return Get() != nullptr; }
	Bridge* operator->() const { return Get(); }

	template <typename OpenValidatedView>
	void Connect(OpenValidatedView&& openValidatedView)
	{
		if (Get()) return;
		std::unique_lock lock(initializationMutex, std::try_to_lock);
		if (!lock.owns_lock() || Get()) return;
		// Preserve the bridge's existing 180-call retry cadence, including an
		// immediate first attempt. Other threads can keep rendering meanwhile.
		if (retryCountdown != 0 && --retryCountdown != 0) return;
		retryCountdown = 180;
		if (auto* view = openValidatedView())
			published.store(view, std::memory_order_release);
	}

private:
	std::atomic<Bridge*> published{nullptr};
	std::mutex initializationMutex;
	std::uint32_t retryCountdown = 0;
};
