#pragma once
#include <windows.h>

// Startup-only slot chaining. Keep the current target, including other mods'
// detours, instead of asking an instruction decoder to patch its prologue.
// No destructor: a later hook may retain ours in its chain until process exit.
class DapaVtableSlot {
    void** slot=nullptr;
    void* replacement=nullptr;
    void* previous=nullptr;
public:
    DWORD error=ERROR_SUCCESS;
    DWORD protectionError=ERROR_SUCCESS;

    bool Install(void** address,void* target,void** original) {
        error=protectionError=ERROR_SUCCESS;
        if(slot || !address || !target || !original) {
            error=ERROR_INVALID_PARAMETER;return false;
        }
        DWORD protection=0;
        if(!VirtualProtect(address,sizeof(void*),PAGE_READWRITE,&protection)) {
            error=GetLastError();return false;
        }
        previous=*address;
        if(!previous || previous==target) {
            error=ERROR_INVALID_DATA;
        } else {
            // Publish the next function before exposing the callback to callers.
            *original=previous;
            auto* observed=InterlockedCompareExchangePointer(address,target,previous);
            if(observed!=previous)error=ERROR_RETRY;
            else {slot=address;replacement=target;}
        }
        DWORD ignored=0;
        if(!VirtualProtect(address,sizeof(void*),protection,&ignored))
            protectionError=GetLastError();
        return slot!=nullptr;
    }
    bool Remove() {
        error=protectionError=ERROR_SUCCESS;
        if(!slot)return true;
        DWORD protection=0;
        if(!VirtualProtect(slot,sizeof(void*),PAGE_READWRITE,&protection)) {
            error=GetLastError();return false;
        }
        // Never overwrite a newer hook which may be chaining through ours.
        auto* observed=InterlockedCompareExchangePointer(slot,previous,replacement);
        if(observed!=replacement)error=ERROR_BUSY;
        DWORD ignored=0;
        if(!VirtualProtect(slot,sizeof(void*),protection,&ignored))
            protectionError=GetLastError();
        if(error==ERROR_SUCCESS)slot=nullptr;
        return error==ERROR_SUCCESS;
    }
};
