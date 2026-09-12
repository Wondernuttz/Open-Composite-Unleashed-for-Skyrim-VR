#include "../src/RaceMenuNativeInput.h"
#include "../src/RaceMenuKeyboardRecovery.h"
#include <array>
#include <cstdlib>
#include <iostream>

#ifndef EXCLUSIVE_SKYRIM_VR
#error This test must exercise the production VR ABI selection.
#endif

namespace
{
    void Require(bool value, const char* message)
    {
        if (!value) { std::cerr << message << '\n'; std::exit(1); }
    }
    struct Event { bool accept; float value; float duration; };
    struct Handler;
    using Callback = bool (*)(Handler*, Event*);
    struct Handler
    {
        Callback* vtable;
        bool pending = false;
        int wrongCalls = 0;
        int buttonCalls = 0;
        int keyboardRequests = 0;
    };
    bool WrongSlot(Handler* handler, Event*)
    {
        ++handler->wrongCalls;
        return false;
    }
    bool NativeButton(Handler* handler, Event* event)
    {
        ++handler->buttonCalls;
        // Match the observed native deferred naming branch. Clearing pending
        // prevents a duplicate even before the asynchronous keyboard appears.
        if (handler->pending && event->accept && event->value == 0 && event->duration >= 0) {
            ++handler->keyboardRequests;
            handler->pending = false;
        }
        return true;
    }
}

int main()
{
    // Model the observed nine-entry native prefix, not CommonLib's six-entry
    // flat declaration. Every wrong slot is a trap, including flat slot 5.
    std::array<Callback, 9> vtable;
    vtable.fill(WrongSlot);
    vtable[8] = NativeButton;
    Handler native{vtable.data()};
    Event release{true, 0.0f, 0.01f};
    ocu::RaceMenuKeyboardRecovery recovery;
    auto pump = [&](bool held, bool keyboardVisible, std::uint64_t now) {
        if (recovery.Observe(1, false, keyboardVisible, held, now))
            Require(ocu::DispatchRaceMenuButton(&native, &release), "VR handler must accept release");
    };

    recovery.ConfirmAccepted(1, 1000);
    native.pending = true;
    recovery.NativeRequest(1);
    pump(true, false, 1300);
    Require(native.buttonCalls == 0, "holding OK cannot open keyboard early");
    pump(false, false, 1310);
    Require(native.wrongCalls == 0 && native.buttonCalls == 1,
        "release must reach VR slot 8, not flat slot 5");
    Require(native.keyboardRequests == 1 && !native.pending,
        "OK release must complete the pending native handoff");
    pump(false, false, 1400);
    pump(false, true, 1500);
    pump(false, false, 1600);
    Require(native.keyboardRequests == 1, "queued keyboard, Done or Cancel must not reopen");

    recovery.ConfirmAccepted(1, 2000);
    native.pending = true;
    recovery.NativeRequest(1);
    NativeButton(&native, &release); // Real native release won before the bridge.
    pump(false, false, 2200);
    Require(native.keyboardRequests == 2, "pending native overlay must not be queued twice");

    recovery = {};
    native.pending = false;
    const int beforeCancel = native.buttonCalls;
    pump(false, false, 3000);
    Require(native.buttonCalls == beforeCancel, "confirmation Cancel must not enter the release path");
    Require(!ocu::DispatchRaceMenuButton(static_cast<Handler*>(nullptr), &release), "missing handler");
    Require(!ocu::DispatchRaceMenuButton(&native, static_cast<Event*>(nullptr)), "missing event");
    std::cout << "PASS: VR native slot 8, held OK, missing release, pending overlay, native-release race and Cancel isolation\n";
}
