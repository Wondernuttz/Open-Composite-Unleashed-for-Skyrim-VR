#include "OpenOVR/Misc/PointerState.h"
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace ocu_pointer;
void Check(bool passed, const char* message) { if (!passed) throw std::runtime_error(message); }
void CheckPresentation()
{
    State state;
    const auto frequency = PointerClockFrequency();
    auto now = frequency * 4;
    Snapshot frame{};
    frame.qpcFrequency = frequency;
    frame.publicationQpc = now;
    frame.predictedDisplayTime = 100;
    frame.trackingOrigin = 1;
    frame.appliedPitchTrimDegrees = -25;
    for (auto& hand : frame.hands) { hand.flags = RayValid | InputAvailable; hand.direction[2] = -1; }
    LeaseRequest request{ sizeof(LeaseRequest), Version, 20, 0, 0, 0, MenuVisible, 100, 1, -25, {}, 0 };
    LeaseResponse response{};
    auto update = [&] { return state.Update(Version, sizeof(request), &request, sizeof(response), &response, now); };
    auto read = [&] { Snapshot value{}; Check(state.Query(Version, sizeof(value), &value, now) == Result::Success, "presentation query"); return value; };
    auto acquire = [&] {
        const auto latest = read();
        request.sessionId = latest.sessionId; request.frameId = latest.frameId;
        request.trackingOrigin = latest.trackingOrigin; request.handMask = 3;
        request.flags = MenuVisible | HasLeftHit | HasRightHit;
        request.hitDistance[0] = 1; request.hitDistance[1] = 2;
        Check(update() == Result::Success, "presentation acquire");
    };
    Check(update() == Result::Success, "presentation bootstrap");
    state.Publish(1, 1, frame);
    const auto accepted = read();
    acquire();
    now += frequency / 100;
    frame.publicationQpc = now; frame.predictedDisplayTime = 200;
    frame.hands[1].origin[0] = 10; frame.hands[1].direction[0] = 1; frame.hands[1].direction[2] = 0;
    frame.hands[1].flags |= TriggerDown;
    state.Publish(1, 1, frame);
    const auto latest = read();
    Check(latest.frameId > accepted.frameId && latest.hands[1].triggerSequence == 1, "input publication stays current");
    auto presentation = state.ReadPresentation(now);
    Check(presentation.snapshot.frameId == latest.frameId && presentation.snapshot.ownedHands == 3,
        "presentation arbitration uses current publication");
    Check(presentation.renderSnapshot.ownedHands == 3 && presentation.renderSnapshot.frameId == presentation.lease.frameId &&
        presentation.renderSnapshot.sessionId == presentation.lease.sessionId, "beam retains accepted publication after frame advance");
    Check(presentation.renderSnapshot.predictedDisplayTime == accepted.predictedDisplayTime &&
        std::memcmp(presentation.renderSnapshot.hands, accepted.hands, sizeof(accepted.hands)) == 0,
        "presentation ray and trigger match consumer cursor frame");
    Check(presentation.renderSnapshot.hands[1].origin[0] + presentation.renderSnapshot.hands[1].direction[0] * presentation.lease.hitDistance[1] == 0 &&
        presentation.renderSnapshot.hands[1].origin[2] + presentation.renderSnapshot.hands[1].direction[2] * presentation.lease.hitDistance[1] == -2,
        "new ray cannot reuse old hit distance");
    acquire();
    presentation = state.ReadPresentation(now);
    Check(presentation.renderSnapshot.frameId == latest.frameId && presentation.renderSnapshot.hands[1].origin[0] == 10,
        "renewal advances coherent presentation");
    request.flags = MenuVisible;
    Check(update() == Result::Success, "hitless dragging renewal");
    presentation = state.ReadPresentation(now);
    Check(presentation.snapshot.ownedHands == 3 && !(presentation.lease.flags & (HasLeftHit | HasRightHit)),
        "hitless drag retains input ownership and hides old beams");
    acquire();
    state.SetBlockedHands(2, now);
    presentation = state.ReadPresentation(now);
    Check(presentation.snapshot.ownedHands == 1 && presentation.renderSnapshot.ownedHands == 1 &&
        (presentation.renderSnapshot.hands[1].flags & SuppressedUntilRelease),
        "keyboard preemption masks retained hand immediately");
    state.SetBlockedHands(0, now);
    now += frequency / 100; frame.publicationQpc = now; frame.hands[0].flags &= ~RayValid;
    state.Publish(1, 1, frame);
    Check(state.ReadPresentation(now).renderSnapshot.ownedHands == 0, "tracking loss clears retained beam ownership");
    state.CaptureNativeHand(0);
    now += frequency / 100; frame.publicationQpc = now; frame.hands[0].flags |= RayValid | TriggerDown;
    state.Publish(1, 1, frame); acquire();
    Check(state.ReadPresentation(now).renderSnapshot.ownedHands == 2, "native held hand cannot reappear in retained presentation");
    now += frequency / 100; frame.publicationQpc = now;
    state.Publish(2, 1, frame);
    presentation = state.ReadPresentation(now);
    Check(presentation.snapshot.ownedHands == 0 && presentation.renderSnapshot.ownedHands == 0 &&
        presentation.snapshot.clientToken == 0 && presentation.snapshot.sessionId > accepted.sessionId,
        "new session invalidates retained presentation");
    acquire();
    now += frequency / 100; frame.publicationQpc = now;
    state.Publish(2, 2, frame);
    Check(state.ReadPresentation(now).renderSnapshot.ownedHands == 0, "new reference space invalidates retained presentation");
    acquire();
    now += frequency / 100; frame.publicationQpc = now; frame.trackingOrigin = 0;
    state.Publish(2, 2, frame);
    Check(state.ReadPresentation(now).renderSnapshot.ownedHands == 0, "tracking origin change invalidates retained presentation");
    acquire();
    now += frequency / 10 + 1; frame.publicationQpc = now;
    state.Publish(2, 2, frame);
    presentation = state.ReadPresentation(now);
    Check(presentation.snapshot.ownedHands == 0 && presentation.renderSnapshot.ownedHands == 0 && presentation.snapshot.clientToken == 0,
        "lease expiry clears presentation despite a fresh input frame");
    acquire();
    state.Reset();
    Check(state.ReadPresentation(now).renderSnapshot.ownedHands == 0, "runtime reset clears retained presentation");
    frame.publicationQpc = now;
    state.Publish(3, 3, frame);
    now += frequency * 99 / 1000;
    acquire();
    frame.publicationQpc = now;
    state.Publish(3, 3, frame);
    now += frequency / 500;
    presentation = state.ReadPresentation(now);
    Check(presentation.snapshot.ownedHands == 3 && presentation.renderSnapshot.ownedHands == 0,
        "accepted ray freshness is independent of lease and current input frame");
    acquire();
    Check(state.ReadPresentation(now).renderSnapshot.ownedHands == 3, "fresh renewal restores presentation");
}
int main()
{
    try {
        CheckPresentation();
        State state;
        const auto frequency = PointerClockFrequency();
        auto now = frequency * 2;
        auto read = [&] { Snapshot v{}; Check(state.Query(Version, sizeof(v), &v, now) == Result::Success, "query"); return v; };
        LeaseRequest request{ sizeof(LeaseRequest), Version, 10, 0, 0, 0, MenuVisible, 100, 1, -25.0f, {}, 0 };
        LeaseResponse response{};
        auto update = [&] { return state.Update(Version, sizeof(request), &request, sizeof(response), &response, now); };
        Snapshot sentinel; std::memset(&sentinel, 0x71, sizeof(sentinel)); const Snapshot before = sentinel;
        Check(state.Query(2, sizeof(sentinel), &sentinel, now) == Result::UnsupportedVersion, "version rejected");
        Check(std::memcmp(&sentinel, &before, sizeof(before)) == 0, "error leaves output untouched");
        Check(update() == Result::Success && response.ownedHands == 0, "observer bootstrap");
        Check(state.ReadInterest(now).active && state.ReadInterest(now).pitch == -25.0f, "interest before first frame");
        Snapshot frame{}; frame.qpcFrequency = frequency; frame.publicationQpc = now;
        frame.trackingOrigin = 1; frame.predictedDisplayTime = 100; frame.appliedPitchTrimDegrees = -25;
        for (auto& hand : frame.hands) { hand.flags = RayValid | InputAvailable; hand.direction[2] = -1; }
        state.Publish(1, 1, frame);
        auto snap = read(); request.sessionId = snap.sessionId; request.frameId = snap.frameId;
        request.handMask = 3; request.flags |= HasRightHit; request.hitDistance[1] = 1.5f;
        state.CaptureNativeHand(1);
        Check(update() == Result::Success && response.ownedHands == 1, "native held click retains ownership");
        state.Publish(1, 1, frame);
        snap = read(); request.frameId = snap.frameId;
        Check(update() == Result::Success && response.ownedHands == 3, "physical release clears native capture");
        Check(state.SuppressNative(1, false, now, true), "native trigger masked while owned");
        request.clientToken = 11; Check(update() == Result::Busy, "other client excluded"); request.clientToken = 10;
        now += frequency / 100; frame.publicationQpc = now; frame.hands[1].flags |= TriggerDown;
        state.Publish(1, 1, frame); snap = read(); Check(snap.hands[1].triggerSequence == 1, "press counted");
        request.frameId = snap.frameId; Check(update() == Result::Success, "drag renew");
        Check(state.SuppressNative(1, true, now, true), "owned press remains hidden from native input");
        Check(!(read().hands[1].flags & SuppressedUntilRelease), "native poll does not suppress legitimate client press");
        state.SetBlockedHands(2, now); snap = read();
        Check(snap.ownedHands == 1 && (snap.hands[1].flags & SuppressedUntilRelease), "keyboard revokes and guards held hand");
        Check(state.SuppressNative(1, true, now, true), "keyboard cannot inherit held click");
        state.SetBlockedHands(0, now); Check(update() == Result::Success && response.ownedHands == 3, "reacquire guarded hand");
        now += frequency / 100; frame.publicationQpc = now; frame.hands[1].flags &= ~TriggerDown;
        state.Publish(1, 1, frame); snap = read();
        Check(snap.hands[1].triggerSequence == 2 && !(snap.hands[1].flags & SuppressedUntilRelease), "physical release rearms");
        now += frequency / 100; frame.publicationQpc = now; frame.hands[1].flags |= TriggerDown;
        state.Publish(1, 1, frame); snap = read(); request.frameId = snap.frameId;
        Check(update() == Result::Success, "held renew");
        now += frequency / 5; snap = read();
        Check(snap.ownedHands == 0 && snap.clientToken == 0 && !(snap.hands[1].flags & RayValid), "expiry clears ray and ownership");
        Check(state.SuppressNative(1, true, now, true), "expiry holds native suppression");
        Check(!state.SuppressNative(1, false, now, true), "expiry release restores native input");
        request.flags = MenuVisible; request.handMask = 0; request.sessionId = request.frameId = 0;
        Check(update() == Result::Success, "rebootstrap");
        frame.publicationQpc = now; frame.hands[1].flags &= ~TriggerDown; state.Publish(1, 1, frame);
        snap = read(); request.sessionId = snap.sessionId; request.frameId = snap.frameId;
        request.handMask = 1; Check(update() == Result::Success, "acquire after expiry");
        request.pitchTrimDegrees = -10; Check(update() == Result::Success && response.ownedHands == 0, "pitch change cancels old ray");
        Check(!(read().hands[0].flags & RayValid), "old pitch unavailable");
        frame.appliedPitchTrimDegrees = -10; now += frequency / 100; frame.publicationQpc = now; state.Publish(1, 1, frame);
        snap = read(); request.frameId = snap.frameId; Check(update() == Result::Success && response.ownedHands == 1, "new pitch acquires");
        request.frameId--; Check(update() == Result::Stale, "old endpoint frame rejected");
        request.flags = 0; Check(update() == Result::Success && read().ownedHands == 0, "stale matching release accepted");
        Check(state.SuppressNative(0, true, now, true), "newer physical down guarded on ownership loss");
        Check(state.SuppressNative(0, false, now, false), "unavailable input cannot confirm a physical release");
        Check(state.SuppressNative(0, true, now, true), "focus return preserves held handoff guard");
        Check(!state.SuppressNative(0, false, now, true), "physical release ends handoff guard");
        request.flags = MenuVisible; request.handMask = 0; request.sessionId = request.frameId = 0;
        request.pitchTrimDegrees = std::numeric_limits<float>::quiet_NaN(); Check(update() == Result::InvalidArgument, "nonfinite pitch rejected");
        request.pitchTrimDegrees = -25; request.leaseMilliseconds = 101; Check(update() == Result::InvalidArgument, "lease bound");
        const auto oldSession = snap.sessionId; state.Reset(); request.leaseMilliseconds = 100;
        Check(update() == Result::Success, "session bootstrap"); frame.publicationQpc = now; state.Publish(2, 2, frame);
        Check(read().sessionId > oldSession, "session epoch changes");
        std::cout << "Pointer lease, freshness, handoff, session, pitch and trigger tests passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
