#include "../src/RaceMenuKeyboardRecovery.h"
#include <cstdlib>
#include <iostream>

void require(bool value, const char* message)
{
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}

int main()
{
    ocu::RaceMenuKeyboardRecovery r;
    require(!r.Observe(1, false, false, false, 0), "editor must not open keyboard");
    require(!r.Observe(1, true, false, true, 100), "confirmation trigger is held");
    require(!r.Observe(1, true, false, true, 1000), "held trigger remains blocked");
    require(r.Observe(1, true, false, false, 1010), "release recovers desktop naming");
    require(!r.Observe(1, true, true, false, 1020), "no duplicate while keyboard open");
    require(!r.Observe(1, true, false, false, 5000), "cancel must not reopen keyboard");
    r.Observe(1, false, false, false, 5010);
    require(!r.Observe(1, true, false, false, 5020), "new naming gives native flow time");
    require(r.Observe(1, true, false, false, 5170), "new naming may recover again");

    r = {};
    r.Observe(2, true, false, false, 100);
    require(!r.Observe(2, true, true, false, 200), "normal native request wins");
    require(!r.Observe(2, true, false, false, 400), "native completion must not reopen");
    require(!r.Observe(3, true, false, false, 500), "replacement movie starts fresh");
    require(r.Observe(3, true, false, false, 650), "replacement movie can recover");
    r = {};
    require(!r.Observe(0, true, false, false, 1000), "missing movie cannot open");

    // The reported failure: OK returns to the editor; no desktop field exists.
    r.ConfirmAccepted(4, 1000);
    require(!r.Observe(4, false, false, true, 1200), "VR confirmation waits for release");
    require(r.Observe(4, false, false, false, 1210), "confirmed VR naming needs no desktop field");
    require(!r.Observe(4, false, false, false, 1400), "VR recovery only runs once");
    require(!r.Observe(4, true, false, false, 1500), "late desktop field cannot duplicate recovery");
    require(!r.Observe(4, false, false, false, 1800), "cancel returns to editor without reopen");
    r.ConfirmAccepted(4, 2000);
    require(!r.Observe(4, false, false, false, 2149), "new confirmation allows normal native handoff");
    require(r.Observe(4, false, false, false, 2150), "fresh confirmation can recover again");

    r.ConfirmAccepted(4, 3000);
    r.NativeRequest(4);
    require(r.NativeRequestObserved(), "native request is remembered without reissuing it");
    require(!r.Observe(4, false, false, true, 3300), "deferred native request still waits for physical release");
    require(r.Observe(4, false, false, false, 3310), "observed native request needs its missing Accept release");
    require(!r.Observe(4, true, false, false, 3400), "release cannot duplicate desktop recovery");
    require(!r.Observe(4, false, false, false, 3500), "queued overlay gets only one release");
    r.ConfirmAccepted(4, 3600);
    r.NativeRequest(99);
    require(!r.NativeRequestObserved(), "another movie cannot mark this request");
    r.NativeRequest(4);
    require(!r.Observe(4, false, true, false, 3800), "native keyboard already open consumes pending release");
    require(!r.Observe(4, false, false, false, 3900), "native keyboard completion must not reopen it");
    r.ConfirmAccepted(4, 4000);
    require(!r.Observe(5, false, false, false, 4200), "replacement movie must discard old confirmation");
    r.ConfirmAccepted(5, 5000);
    require(!r.Observe(5, false, true, false, 5100), "existing overlay consumes the request");
    require(!r.Observe(5, false, false, false, 5300), "closing an existing overlay must not reopen");
    r.ConfirmAccepted(5, 6000);
    require(!r.Observe(5, false, false, false, 12000), "stale confirmation expires behind another modal");
    r = {};
    require(!r.Observe(5, false, false, false, 13000), "Cancel or unrelated OK without naming callback stays idle");
    std::cout << "RaceMenu keyboard lifecycle: PASS\n";
}
