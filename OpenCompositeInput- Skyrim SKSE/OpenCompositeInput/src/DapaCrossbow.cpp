#include "DapaCrossbow.h"
#include <RE/B/BGSProjectile.h>
#include <RE/T/TESDataHandler.h>
#include <RE/T/TESObjectREFR.h>
#include <SKSE/SKSE.h>
#include <Windows.h>

namespace DapaCrossbow {
namespace {
    // Installed CrossbowReloadVR.esp: PROJ 0x803 / ProjectileBolt.
    // ImmersiveCrossbowReloadVR 1.0.4 (DLL SHA-256 0b3c48b7052e18fe35a44ed55390b357b626d8ee12faaabcfbbcd039e95e3c30):
    // CreateFakeBoltFunc spawns this dedicated prop. UpdateProjectile positions
    // it from the off-hand transform while grabbedBoltInHand, removes it when
    // released, and does not use its trajectory for a real crossbow shot.
    // Exact base-form identity only: no retained reference/mesh pointers,
    // whole-mod matching, projectile-type exclusion or private DLL hook.
    const RE::BGSProjectile* reloadBolt=nullptr;
    bool initialized=false;
}
void Initialize()
{
    if(initialized)return;
    initialized=true;
    auto* data=RE::TESDataHandler::GetSingleton();
    if(data && GetModuleHandleW(L"ImmersiveCrossbowReloadVR.dll"))
        reloadBolt=data->LookupForm<RE::BGSProjectile>(0x803,"CrossbowReloadVR.esp");
    SKSE::log::info("DAPA CROSSBOW MASK v1: hand-positioned reload prop {} (resolved form={:08X}); fired bolts retain world correction",
        reloadBolt?"resolved":"not present",reloadBolt?reloadBolt->GetFormID():0);
}
bool Owns(const RE::TESObjectREFR* reference)
{
    return reloadBolt && reference && reference->GetBaseObject()==reloadBolt;
}
}
