#pragma once

#include <cstdint>

// Inspect each completed space-creation pass once, including a pass that failed
// to expose any roles. A replacement set/session can then recover those roles.
class OcuBodyTrackerDiscovery {
public:
	bool Claim(uint64_t generation)
	{
		if (attempted && lastGeneration == generation)
			return false;
		attempted = true;
		lastGeneration = generation;
		return true;
	}
private:
	bool attempted = false;
	uint64_t lastGeneration = 0;
};

// Native roles may be appended after NET devices during recovery. Their vector
// position is not their public index; calibrated identities never move.
template <class Device, class NativeDevices, class NetworkDevices>
Device* OcuFindPublishedTracker(uint32_t index, const NativeDevices& native, const NetworkDevices& network)
{
	for (const auto& tracker : native)
		if (tracker->DeviceIndex() == index) return tracker.get();
	for (const auto& tracker : network)
		if (tracker->DeviceIndex() == index) return tracker.get();
	return nullptr;
}
