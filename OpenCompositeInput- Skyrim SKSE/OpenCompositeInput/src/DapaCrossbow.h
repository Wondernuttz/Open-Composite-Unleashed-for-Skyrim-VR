#pragma once
namespace RE { class TESObjectREFR; }
namespace DapaCrossbow {
void Initialize(); // DataLoaded; optional Immersive Crossbow Reload VR dependency.
bool Owns(const RE::TESObjectREFR* reference);
}
