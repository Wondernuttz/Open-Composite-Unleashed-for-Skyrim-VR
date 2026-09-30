#include "DapaSpellWheel.h"
#include "DapaSpellWheelForms.h"
#include <RE/B/BGSProjectile.h>
#include <RE/T/TESDataHandler.h>
#include <RE/T/TESObjectREFR.h>
#include <RE/N/NiAVObject.h>
#include <SKSE/SKSE.h>

namespace DapaSpellWheel {
namespace {
    // Base forms remain game-owned for the process lifetime. Never cache a
    // projectile reference or a geometry root: page changes destroy those.
    ResolvedForms forms;
    bool initialized=false;
}
void Initialize() {
    if(initialized)return;
    initialized=true;
    auto* data=RE::TESDataHandler::GetSingleton();
    if(!data)return;
    for(auto id:localIds) {
        // Resolve through CommonLib instead of assuming an ESP load-order byte.
        auto* form=data->LookupForm<RE::BGSProjectile>(id,"SpellWheelVR.esp");
        if(form)forms.Add(form->GetFormID());
    }
    forms.Seal();
    SKSE::log::info("DAPA SPELL WHEEL: resolved {}/{} UI projectile base records; live reference ownership, world effects excluded",forms.Size(),localIds.size());
}
bool Owns(const RE::TESObjectREFR* reference) {
    if(!forms.Size() || !reference)return false;
    auto* base=reference->GetBaseObject();
    // Cheap rejection for NPCs, buildings and ordinary held inventory objects.
    if(!base || base->GetFormType()!=RE::FormType::Projectile)return false;
    return forms.Matches(base->GetFormID(),true);
}
static_assert(offsetof(RE::NiAVObject,userData)==0x110,"Spell Wheel owner traversal requires the CommonLib VR layout");
}
