#pragma once

#include <cstdint>
#include <atomic>

// Minimal SKSE structures for MenuOpenCloseEvent tracking

class BSFixedString
{
public:
	const char* c_str() const { return data ? data : ""; }
	const char* data;
};

struct MenuOpenCloseEvent
{
	BSFixedString menuName;
	bool opening;
	char pad[3];
};

template <typename EventT>
class BSTEventSource;

enum class BSEventNotifyControl
{
	kContinue,
	kStop
};

template <typename EventT>
class BSTEventSink
{
public:
	virtual ~BSTEventSink() {}
	virtual BSEventNotifyControl ProcessEvent(const EventT* evn, BSTEventSource<EventT>* src) = 0;
};

// Menu tracker event sink
class SkyrimMenuTracker : public BSTEventSink<MenuOpenCloseEvent>
{
public:
	static SkyrimMenuTracker& GetInstance();

	BSEventNotifyControl ProcessEvent(const MenuOpenCloseEvent* evn, BSTEventSource<MenuOpenCloseEvent>* src) override;

	// Returns true if ANY menu is currently open (checks UI singleton directly)
	bool IsAnyMenuOpen() const;

	// Initialize and register with SKSE event system
	void Initialize();

private:
	SkyrimMenuTracker() : menuCount(0) {}
	std::atomic<int> menuCount;
};
