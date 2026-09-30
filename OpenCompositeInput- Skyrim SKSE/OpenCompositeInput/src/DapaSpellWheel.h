#pragma once
namespace RE { class TESObjectREFR; }
namespace DapaSpellWheel {
void Initialize(); // DataLoaded, before any draw hook is installed
bool Owns(const RE::TESObjectREFR* reference);
}
