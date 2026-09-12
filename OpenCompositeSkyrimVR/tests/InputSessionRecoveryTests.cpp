#include "DrvOpenXR/InputSessionRecovery.h"
#include "OpenOVR/InputTrace.h"
#include "OpenOVR/Misc/Input/IndexTrackpadRouting.h"
#include <iostream>
#include <string_view>

using namespace OcuInputSession;
using namespace std::chrono_literals;

int main()
{
	int failures = 0;
	auto check = [&](bool ok, const char* name) {
		if (!ok) { std::cerr << "FAIL: " << name << '\n'; ++failures; }
	};
	const auto now = ProfileRetry::Clock::time_point{} + 10s;
	check(NeedsBridgeStartupRecovery(true,"WiVRn") && NeedsBridgeStartupRecovery(true,"Monado"), "Wine/native Linux bridge recovery selection");
	check(!NeedsBridgeStartupRecovery(false,"Monado") && !NeedsBridgeStartupRecovery(true,"SteamVR"), "ordinary Windows/other runtimes excluded");
	for (auto state : {XR_SESSION_STATE_UNKNOWN, XR_SESSION_STATE_IDLE}) {
		StartupWait startup(now);
		check(startup.Waiting(now+9999ms,state,false), "missing initial/READY event waits within bound");
		check(!startup.ClaimRecovery(now+9999ms,state,false,true), "no premature recovery");
		check(!startup.Waiting(now+10s,state,false), "startup cannot wait forever");
		check(startup.ClaimRecovery(now+10s,state,false,true), "one recovery after timeout");
		check(!startup.ClaimRecovery(now+20s,state,false,true), "no repeated forced begin after failure");
	}
	for (auto state : {XR_SESSION_STATE_READY,XR_SESSION_STATE_STOPPING,XR_SESSION_STATE_EXITING,XR_SESSION_STATE_LOSS_PENDING}) {
		StartupWait startup(now);
		check(!startup.Waiting(now,state,false) && !startup.ClaimRecovery(now+10s,state,false,true), "no recovery across normal/terminal transition");
	}
	StartupWait activeStartup(now);
	check(!activeStartup.Waiting(now,XR_SESSION_STATE_IDLE,true) &&
	    !activeStartup.ClaimRecovery(now+10s,XR_SESSION_STATE_IDLE,true,true), "no duplicate begin for an active session");
	StartupWait ordinaryStartup(now);
	check(!ordinaryStartup.ClaimRecovery(now+10s,XR_SESSION_STATE_UNKNOWN,false,false), "ordinary runtime only resumes via events");
	for (const auto state : { XR_SESSION_STATE_READY, XR_SESSION_STATE_SYNCHRONIZED,
	         XR_SESSION_STATE_VISIBLE, XR_SESSION_STATE_FOCUSED }) {
		ProfileRetry retry;
		check(retry.Due(now, state, true, false, false), "discovery without focus or a profile event");
		check(!retry.Due(now, state, false, false, false), "loaded actions are insufficient before attachment");
	}
	for (const auto state : { XR_SESSION_STATE_UNKNOWN, XR_SESSION_STATE_IDLE,
	         XR_SESSION_STATE_STOPPING, XR_SESSION_STATE_EXITING, XR_SESSION_STATE_LOSS_PENDING }) {
		ProfileRetry retry;
		check(!retry.Due(now, state, true, false, false), "no queries while session unavailable/stopping");
	}

	ProfileRetry retry;
	retry.Complete(now, false);
	check(!retry.Due(now + 999ms, XR_SESSION_STATE_SYNCHRONIZED, true, false, false), "unresolved retry is throttled");
	check(retry.Due(now + 1s, XR_SESSION_STATE_SYNCHRONIZED, true, false, false), "retry progresses without focus");
	retry.Complete(now + 1s, false); // Left appeared; right is still unavailable.
	check(retry.Due(now + 2s, XR_SESSION_STATE_VISIBLE, true, true, false), "left hand does not hide missing right");
	check(retry.Due(now + 2s, XR_SESSION_STATE_VISIBLE, true, false, true), "right hand does not hide missing left");
	retry.Complete(now + 2s, true);
	check(!retry.Due(now + 20s, XR_SESSION_STATE_FOCUSED, true, true, true), "no continuous queries once resolved");

	retry.Request(); // Current-session profile change or regained focus.
	check(retry.Due(now + 2001ms, XR_SESSION_STATE_FOCUSED, true, true, true), "wake/profile change bypasses old retry deadline");
	retry.Complete(now + 2001ms, true);
	retry.Request(); // Replacement session can retain device identities, never old attachment.
	check(!retry.Due(now + 2002ms, XR_SESSION_STATE_READY, false, true, true), "replacement waits for its own attachment");
	check(retry.Due(now + 2002ms, XR_SESSION_STATE_READY, true, true, true), "replacement rechecks retained identities immediately");

	const auto oldSession = reinterpret_cast<XrSession>(static_cast<uintptr_t>(1));
	const auto newSession = reinterpret_cast<XrSession>(static_cast<uintptr_t>(2));
	check(!Matches(newSession, oldSession), "ignore events belonging to previous session");
	check(Matches(newSession, newSession), "accept current session events");
	check(!Matches(XR_NULL_HANDLE, XR_NULL_HANDLE), "no events accepted without a session");

	// Replay CrashTD's replacement-session sequence: old events are discarded,
	// begin succeeds but state stays UNKNOWN, and real action sync is successful.
	ProfileRetry bridgeRetry;
	for (auto state : { XR_SESSION_STATE_UNKNOWN, XR_SESSION_STATE_IDLE }) {
		check(!bridgeRetry.Due(now, state, true, false, false, false), "unbegun bridge cannot query");
		check(bridgeRetry.Due(now, state, true, false, false, true), "successful bridge begin unblocks discovery without events");
		check(!bridgeRetry.Due(now, state, false, false, false, true), "bridge must attach current-session actions");
	}
	for (auto state : { XR_SESSION_STATE_STOPPING, XR_SESSION_STATE_EXITING, XR_SESSION_STATE_LOSS_PENDING })
		check(!bridgeRetry.Due(now, state, true, false, false, true), "begin proof never overrides terminal state");
	bridgeRetry.Complete(now, false);
	check(!bridgeRetry.Due(now + 999ms, XR_SESSION_STATE_UNKNOWN, true, false, false, true), "bridge retries remain bounded");
	check(bridgeRetry.Due(now + 1s, XR_SESSION_STATE_UNKNOWN, true, false, false, true), "bridge retries continue without profile events");
	SyncFocus focus;
	check(!focus.Confirmed(newSession, 1000), "missing focus event alone grants no input");
	focus.Observe(oldSession, XR_SUCCESS, 1000);
	check(!focus.Confirmed(newSession, 1000), "old-session action sync grants no replacement input");
	focus.Observe(newSession, XR_SUCCESS, 1000);
	check(focus.Confirmed(newSession, 1001), "real current-session action sync proves runtime focus");
	check(!focus.Confirmed(newSession, 2000), "stopped sync cannot leave indefinite focus");
	focus.Observe(newSession, XR_SESSION_NOT_FOCUSED, 1100);
	check(!focus.Confirmed(newSession, 1101), "runtime focus withdrawal immediately clears proof");
	for (auto result : { XR_ERROR_ACTIONSET_NOT_ATTACHED, XR_ERROR_SESSION_LOST, XR_SESSION_LOSS_PENDING }) {
		focus.Observe(newSession, XR_SUCCESS, 1200);
		focus.Observe(newSession, result, 1201);
		check(!focus.Confirmed(newSession, 1202), "failed or qualified action sync grants no input");
	}
	focus.Observe(newSession, XR_SUCCESS, 1300);
	focus.Reset();
	check(!focus.Confirmed(newSession, 1301), "detach clears evidence even if the runtime reuses a handle");

	OcuInputTrace::ChangeGate trace;
	const OcuInputTrace::Snapshot valid{ 1, 0, 3, 0, 0 }, invalid{ 1, 0, 0, 0, 0 };
	check(!trace.Allow(false, valid, 0), "checkbox off suppresses diagnostic samples");
	check(trace.Allow(true, valid, 1), "first enabled sample is immediate");
	for (uint64_t time = 2; time < 5000; ++time)
		check(!trace.Allow(true, valid, time), "unchanged samples never produce periodic spam");
	check(trace.Allow(true, invalid, 5000), "changed state is logged");
	check(!trace.Allow(true, valid, 5001), "rapid state changes are rate limited");
	check(trace.Allow(true, valid, 6000), "latest changed state is reported after cooldown");
	check(!trace.Allow(false, invalid, 6001), "disabled logging resets diagnostic baseline");
	check(trace.Allow(true, invalid, 6002), "reenabling logging records a fresh snapshot");
	check(std::string_view(OcuInputTrace::State(XR_SESSION_STATE_SYNCHRONIZED)) == "SYNCHRONIZED", "state names are readable");
	check(std::string_view(OcuInputTrace::Result(XR_SESSION_NOT_FOCUSED)) == "SESSION_NOT_FOCUSED", "focus loss is distinct from errors");
	check(std::string_view(OcuInputTrace::Result(XR_ERROR_ACTIONSET_NOT_ATTACHED)) == "ACTIONSET_NOT_ATTACHED", "attachment failure is identified");
	if (failures) return 1;
	using OcuIndexTrackpad::PressMask;
	for (int hand = 0; hand < 2; ++hand) {
		check(PressMask(hand, 0.8f, true, true, false, false, 0) == (uint64_t{1} << 1), "upper pad defaults to Menu");
		check(PressMask(hand, -0.8f, true, true, false, false, 0) == (uint64_t{1} << 7), "lower pad defaults to A");
		check(PressMask(hand, 0, true, true, false, false, 0) == (uint64_t{1} << 7), "center retains legacy lower behavior");
		check(PressMask(hand, 0.8f, true, true, false, false, 15) == (uint64_t{1} << 5), "custom upper emits only independent id");
		check(PressMask(hand, -0.8f, true, true, false, false, 15) == (uint64_t{1} << 6), "custom lower emits only independent id");
		check(PressMask(hand, 0.8f, false, true, false, false, 15) == 0, "inactive axes cannot synthesize presses");
		check(PressMask(hand, 0.8f, true, false, false, false, 15) == 0, "touch alone cannot synthesize press");
		check(PressMask(hand, 0.8f, true, true, true, false, 15) == 0, "disabled routing suppresses buttons");
		check(PressMask(hand, 0.8f, true, true, false, true, 15) == 0, "VRIK mode does not double-fire normal assignments");
	}
	check(PressMask(0, 0.8f, true, true, false, false, 1) == (uint64_t{1} << 5), "left upper custom mask");
	check(PressMask(1, 0.8f, true, true, false, false, 1) == (uint64_t{1} << 1), "left custom leaves right unchanged");
	if (failures) return 1;
	std::cout << "PASS: input recovery plus diagnostic checkbox, change-only logging and rate limits\n";
	return 0;
}
