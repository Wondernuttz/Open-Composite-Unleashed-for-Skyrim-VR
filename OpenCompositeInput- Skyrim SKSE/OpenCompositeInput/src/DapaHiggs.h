#pragma once
#include "DapaHeldRoots.h"
#include <RE/N/NiAVObject.h>
#include <RE/N/NiSmartPointer.h>

namespace DapaHiggs {
using Roots=DapaHeldRoots<RE::NiAVObject,RE::NiPointer<RE::NiAVObject>>;
extern Roots roots;
void Connect(); // SKSE PostPostLoad only; optional dependency
void SetEnabled(bool enabled); // clears all held ownership on session changes
}
