#pragma once

#include <REL/Relocation.h>

namespace ocu
{
    // The installed CommonLib MenuEventHandler declaration uses the flat-game
    // layout even in its VR build. Skyrim VR adds three virtuals before the
    // button handler. Verified on the live RaceSexMenu secondary vtable:
    // slot 5 -> 0x52F000 (returns false); slot 8 -> 0x8E0F80 (ProcessButton).
    // Keep this adaptation local instead of changing every menu's ABI.
    template <class Handler, class Event>
    bool DispatchRaceMenuButton(Handler* handler, Event* event)
    {
        if (!handler || !event)
            return false;
        return REL::RelocateVirtual<bool(Handler*, Event*)>(5, 8, handler, event);
    }
}
