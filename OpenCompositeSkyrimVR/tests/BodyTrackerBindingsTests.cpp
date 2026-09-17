#include "OpenOVR/Misc/BodyTrackerBindings.h"
#include "OpenOVR/Misc/BodyTrackerDevices.h"
#include "OpenOVR/Misc/BodyTrackerRoles.h"

#include <iostream>
#include <memory>
#include <vector>

int main()
{
	int failures = 0;
	auto check = [&](bool value, const char* name) {
		if (!value) { std::cerr << "FAIL: " << name << '\n'; ++failures; }
	};
	auto selected = [](const char* list, uint32_t revision) {
		std::vector<int> roles;
		for (int role = 0; role < OCU_TRACKER_ROLE_COUNT; ++role)
			if (OcuTrackerRoleEnabled(list, role, revision)) roles.push_back(role);
		return roles;
	};
	for (uint32_t revision : { 1u, 2u, 3u })
		check(selected("waist,left_foot,right_foot", revision) == std::vector<int>({ 0, 1, 2 }),
		    "default pose roles survive every published HTCX revision");
	check(selected("all", 0).empty(), "absent or invalid extension revision enables no roles");
	for (uint32_t revision : { 1u, 2u }) {
		check(selected("all", revision) == std::vector<int>({ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 }),
		    "all on an older runtime excludes only the unsupported revision-three roles");
		check(selected("waist,left_wrist,right_ankle", revision) == std::vector<int>({ 0 }),
		    "unsupported explicit roles cannot poison a supported pose suggestion");
	}
	check(selected("all", 3).size() == OCU_TRACKER_ROLE_COUNT, "revision three retains every configured role");
	check(selected("left_wrist,right_wrist,left_ankle,right_ankle", 3) == std::vector<int>({ 10, 11, 12, 13 }),
	    "revision-three wrist and ankle paths become available together");
	check(selected("not_waist,left_foot_extra", 3).empty(), "role selection requires whole tokens");
	check(selected(" \tWaIsT \r,\n LEFT_FOOT\t,RIGHT_FOOT ", 3) == std::vector<int>({ 0, 1, 2 }),
	    "saved mixed-case roles and ASCII whitespace match the Configurator selection");
	check(selected("\t AlL \r\n", 3).size() == OCU_TRACKER_ROLE_COUNT,
	    "standalone all accepts case differences and surrounding whitespace");
	check(selected(" AlL ", 2) == std::vector<int>({ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 }),
	    "normalizing all preserves the runtime revision guard");
	check(selected(" LEFT_WRIST , WaIsT , RIGHT_ANKLE ", 2) == std::vector<int>({ 0 }),
	    "normalized explicit revision-three roles cannot poison older-runtime bindings");
	check(selected(" ALL, WAIST ", 3) == std::vector<int>({ 0 })
	        && selected("all,", 3).empty() && selected("small", 3).empty(),
	    "all is recognized only as a standalone value, never a token or substring");
	check(selected(" , \t, \r\n", 3).empty() && selected("wa ist,left_ foot", 3).empty(),
	    "empty tokens and whitespace inside role names do not enable roles");
	check(!OcuTrackerRoleEnabled("all", -1, 3) && !OcuTrackerRoleEnabled("all", OCU_TRACKER_ROLE_COUNT, 3),
	    "role bounds are checked before indexing");

	OcuBodyTrackerBindings bindings;
	check(bindings.NeedsSuggestion() && !bindings.HasPoseBindings(), "no spaces before accepted bindings");
	std::vector<bool> calls;
	auto rejectHaptics = [&](bool haptics) {
		calls.push_back(haptics);
		return haptics ? XR_ERROR_PATH_UNSUPPORTED : XR_SUCCESS;
	};
	check(bindings.Prepare(true, rejectHaptics) == XR_SUCCESS
	        && calls == std::vector<bool>({ true, false })
	        && bindings.HasPoseBindings() && !bindings.HasHapticBindings(),
	    "runtime rejecting haptics receives a complete pose-only replacement suggestion");
	bindings.Prepare(true, rejectHaptics);
	check(calls.size() == 2 && !bindings.NeedsSuggestion(),
	    "session recreation preserves pose-only acceptance without mutating an attached action set");

	bindings.Reset(); // A new legacy action set owns new action handles.
	calls.clear();
	auto rejectAll = [&](bool haptics) { calls.push_back(haptics); return XR_ERROR_PATH_UNSUPPORTED; };
	check(bindings.Prepare(true, rejectAll) == XR_ERROR_PATH_UNSUPPORTED
	        && calls == std::vector<bool>({ true, false }) && !bindings.HasPoseBindings(),
	    "rejected pose suggestions leave spaces unavailable and do not repeat unsupported paths");
	bindings.Prepare(true, rejectHaptics);
	check(calls.size() == 2 && !bindings.HasPoseBindings(),
	    "session replacement cannot retry a rejected immutable action set");
	bindings.Reset();
	calls.clear();
	check(bindings.Prepare(true, [&](bool haptics) { calls.push_back(haptics); return XR_SUCCESS; }) == XR_SUCCESS
	        && calls == std::vector<bool>({ true }) && bindings.HasPoseBindings() && bindings.HasHapticBindings(),
	    "actual action-set rebuild retries and recovers both poses and haptics");

	bindings.Reset();
	calls.clear();
	check(bindings.Prepare(false, [&](bool haptics) {
		calls.push_back(haptics);
		return calls.size() == 1 ? XR_ERROR_RUNTIME_FAILURE : XR_SUCCESS;
	}) == XR_SUCCESS && calls == std::vector<bool>({ false, false }) && bindings.HasPoseBindings(),
	    "transient failure is retried before attachment without fabricating haptic availability");
	bindings.Reset();
	calls.clear();
	check(bindings.Prepare(true, [&](bool haptics) {
		calls.push_back(haptics); return XR_ERROR_RUNTIME_FAILURE;
	}) == XR_ERROR_RUNTIME_FAILURE && calls == std::vector<bool>({ true, true, false, false })
	        && !bindings.HasPoseBindings(), "persistent runtime failure has a bounded retry budget");

	OcuBodyTrackerDiscovery discovery;
	check(discovery.Claim(1), "initial rejected binding pass can be observed");
	check(!discovery.Claim(1), "unchanged rejected pass does not trigger per-frame discovery");
	check(discovery.Claim(2), "later space recreation recovers discovery after initial rejection");
	check(!discovery.Claim(2) && discovery.Claim(3), "each subsequent session is inspected only once");
	struct FakeDevice {
		uint32_t index;
		uint32_t DeviceIndex() const { return index; }
	};
	std::vector<std::unique_ptr<FakeDevice>> native, network;
	for (uint32_t index = 3; index <= 10; ++index)
		network.push_back(std::make_unique<FakeDevice>(FakeDevice{ index }));
	auto* originalWaist = network.front().get();
	// Initial OSC-only publication is followed by recovered non-OSC HTCX roles.
	native.push_back(std::make_unique<FakeDevice>(FakeDevice{ 11 }));
	native.push_back(std::make_unique<FakeDevice>(FakeDevice{ 12 }));
	check(OcuFindPublishedTracker<FakeDevice>(3, native, network) == originalWaist,
	    "late native discovery preserves the original canonical waist index");
	for (uint32_t index = 3; index <= 12; ++index) {
		auto* device = OcuFindPublishedTracker<FakeDevice>(index, native, network);
		check(device && device->DeviceIndex() == index, "mixed publication order resolves every assigned identity");
	}
	check(!OcuFindPublishedTracker<FakeDevice>(13, native, network), "unassigned tracker indices remain absent");

	if (!failures) std::cout << "Body tracker role/binding regression tests passed\n";
	return failures ? 1 : 0;
}
