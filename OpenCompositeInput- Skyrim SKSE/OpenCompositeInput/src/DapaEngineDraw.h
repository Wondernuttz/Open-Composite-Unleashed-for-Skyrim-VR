#pragma once
#include <windows.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>

namespace DapaEngineDraw {
// Skyrim VR 1.4.15 mesh renderer. Verified against live, unpacked instructions.
// Prefix bytes are argument setup, not arbitrary instruction fragments.
struct Site {
    uint32_t rva; bool instanced; uint8_t size,prefix;
    std::array<uint8_t,10> expected;
};
inline constexpr std::array<Site,17> sites{{
    {0xDBDDF3,true,6,0,{0xff,0x90,0xa0,0,0,0}}, // CSX menu bridge may already own this call
    {0xDBDE06,false,9,6,{0x45,0x33,0xc9,0x44,0x8b,0xc7,0xff,0x50,0x60}},
    {0xDBDF27,true,6,0,{0xff,0x90,0xa0,0,0,0}},
    {0xDBDF3A,false,9,6,{0x45,0x33,0xc9,0x44,0x8b,0xc7,0xff,0x50,0x60}},
    {0xDBE09B,true,6,0,{0xff,0x90,0xa0,0,0,0}},
    {0xDBE98E,true,7,0,{0x41,0xff,0x92,0xa0,0,0,0}},
    {0xDBE9A5,false,10,6,{0x45,0x33,0xc9,0x44,0x8b,0xc3,0x41,0xff,0x52,0x60}},
    {0xDBEBD7,true,6,0,{0xff,0x90,0xa0,0,0,0}},
    {0xDBEBDF,false,6,3,{0x45,0x33,0xc0,0xff,0x50,0x60}},
    {0xDBEE58,true,6,0,{0xff,0x90,0xa0,0,0,0}},
    {0xDBEE60,false,6,3,{0x45,0x33,0xc0,0xff,0x50,0x60}},
    {0xDBF15B,true,6,0,{0xff,0x90,0xa0,0,0,0}},
    {0xDBF171,false,6,3,{0x45,0x33,0xc0,0xff,0x50,0x60}},
    {0xDBF4D9,true,6,0,{0xff,0x90,0xa0,0,0,0}},
    {0xDBF4E1,false,9,6,{0x45,0x33,0xc9,0x44,0x8b,0xc6,0xff,0x50,0x60}},
    {0xDBF63C,true,6,0,{0xff,0x90,0xa0,0,0,0}},
    {0xDBF64F,false,9,6,{0x45,0x33,0xc9,0x44,0x8b,0xc3,0xff,0x50,0x60}}
}};

struct Hook {
    const Site* spec=nullptr;
    uint8_t* address=nullptr;
    void* chained=nullptr;
    std::array<uint8_t,10> before{},patch{};
    DWORD error=0;
    bool installed=false;

    bool Prepare(const Site& site,uint8_t* call) {
        spec=&site;address=call;error=0;
        SIZE_T count=0;
        if(!ReadProcessMemory(GetCurrentProcess(),call,before.data(),site.size,&count) || count!=site.size) {
            error=ERROR_READ_FAULT;return false;
        }
        if(std::memcmp(before.data(),site.expected.data(),site.size)==0)return true;
        // Preserve an existing SKSE six-byte indirect-call chain, notably CSX's
        // higher-level HUD capture/suppression callback. Do not accept unknown patches.
        if(site.instanced && site.size==6 && before[0]==0xff && before[1]==0x15) {
            int32_t offset=0;std::memcpy(&offset,before.data()+2,4);
            if(ReadProcessMemory(GetCurrentProcess(),call+6+offset,&chained,sizeof(chained),&count) &&
                count==sizeof(chained) && chained) {
                MEMORY_BASIC_INFORMATION region{};
                if(VirtualQuery(chained,&region,sizeof(region)) && region.State==MEM_COMMIT &&
                   !(region.Protect & PAGE_GUARD) && (region.Protect & (PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)))return true;
            }
        }
        error=ERROR_INVALID_DATA;return false;
    }
    bool Build(uint8_t* stub,uintptr_t callback) {
        // Jump in, reproduce argument setup, call with the ORIGINAL stack/shadow
        // space (including arguments 5/6), then jump to the original continuation.
        size_t n=spec->prefix;
        std::memcpy(stub,before.data(),n);
        stub[n++]=0x48;stub[n++]=0xb8; // mov rax, callback
        std::memcpy(stub+n,&callback,8);n+=8;
        stub[n++]=0xff;stub[n++]=0xd0; // call rax
        stub[n++]=0xff;stub[n++]=0x25;
        std::memset(stub+n,0,4);n+=4;
        const auto resume=reinterpret_cast<uintptr_t>(address+spec->size);
        std::memcpy(stub+n,&resume,8);n+=8;
        FlushInstructionCache(GetCurrentProcess(),stub,n);
        const auto distance=reinterpret_cast<intptr_t>(stub)-reinterpret_cast<intptr_t>(address+5);
        if(distance<(std::numeric_limits<int32_t>::min)() || distance>(std::numeric_limits<int32_t>::max)()) {
            error=ERROR_ARITHMETIC_OVERFLOW;return false;
        }
        patch.fill(0x90);patch[0]=0xe9;
        const int32_t relative=static_cast<int32_t>(distance);
        std::memcpy(patch.data()+1,&relative,4);return true;
    }
    bool Write(const uint8_t* expected,const uint8_t* replacement) {
        DWORD old=0;
        if(!VirtualProtect(address,spec->size,PAGE_EXECUTE_READWRITE,&old)) {error=GetLastError();return false;}
        const bool match=std::memcmp(address,expected,spec->size)==0;
        if(match)std::memcpy(address,replacement,spec->size);
        DWORD ignored=0;
        const bool restored=VirtualProtect(address,spec->size,old,&ignored)!=FALSE;
        if(!restored)error=GetLastError();
        else if(!match)error=ERROR_BUSY;
        FlushInstructionCache(GetCurrentProcess(),address,spec->size);
        return match && restored;
    }
    bool Install() {
        const bool ok=Write(before.data(),patch.data());
        // Include a successfully written patch in rollback even if restoring
        // its page protection failed.
        installed=std::memcmp(address,patch.data(),spec->size)==0;
        return ok;
    }
    bool Remove() {
        if(!installed)return true;
        if(!Write(patch.data(),before.data()))return false;
        installed=false;return true;
    }
};
}
