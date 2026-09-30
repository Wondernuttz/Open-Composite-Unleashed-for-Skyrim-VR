#include "DapaHiggs.h"
#include "DapaHiggsApi.h"
#include <RE/T/TESObjectREFR.h>
#include <SKSE/SKSE.h>

namespace DapaHiggs {
Roots roots;
namespace {
    Interface* api=nullptr;
    std::mutex updateMutex;
    bool enabled=false;
    bool attempted=false;

    void ClearHand(bool left) {
        std::scoped_lock lock(updateMutex);
        roots.Set(left,{});
    }
    void Dropped(bool left,RE::TESObjectREFR*) { ClearHand(left); }
    void Removed(bool left,RE::TESForm*) { ClearHand(left); }
    void AfterUpdate() {
        std::scoped_lock lock(updateMutex);
        if(!enabled || !api)return;
        // HIGGS's main update has completed. Grab callbacks fire BEFORE its
        // state becomes Held; drop callbacks can be suppressed on handoff.
        // Refresh both hands here, not from a renderer/physics worker thread.
        for(bool left:{true,false}) {
            RE::NiPointer<RE::TESObjectREFR> ref(api->GetGrabbedObject(left));
            RE::NiPointer<RE::NiAVObject> root;
            // Do not freeze a whole NPC/ragdoll into hand space when one of
            // its limbs is grabbed. This path is for held inventory objects.
            if(ref && ref->GetFormType()!=RE::FormType::ActorCharacter)
                root.reset(ref->Get3D());
            if(roots.Set(left,root))
                SKSE::log::info("DAPA HIGGS: {} held root={} ref={:08X}",
                    left?"left":"right",static_cast<void*>(root.get()),ref?ref->GetFormID():0);
        }
    }
}
void SetEnabled(bool value) {
    std::scoped_lock lock(updateMutex);
    enabled=value;
    roots.Clear();
}
void Connect() {
    if(attempted)return;
    attempted=true;
    auto* messaging=SKSE::GetMessagingInterface();
    Message message;
    if(!messaging || !messaging->Dispatch(Message::type,&message,sizeof(message),"HIGGS") || !message.getApiFunction) {
        SKSE::log::info("DAPA HIGGS: API unavailable; body/equipped mask unchanged");
        return;
    }
    auto* candidate=static_cast<Interface*>(message.getApiFunction(1));
    if(!candidate)return;
    const auto build=candidate->GetBuildNumber();
    if(!Supported(build)) {
        SKSE::log::warn("DAPA HIGGS: build {} outside verified API range (requires 1.10.10+ 1.x); held mask inactive",build);
        return;
    }
    api=candidate;
    api->AddDroppedCallback(Dropped);
    api->AddStashedCallback(Removed);
    api->AddConsumedCallback(Removed);
    api->AddPreVrikPostHiggsCallback(AfterUpdate);
    SKSE::log::info("DAPA HIGGS: interface 1 connected, build {}; post-update held-object tracking registered",build);
}
}
