#pragma once
#include "DapaEngineDraw.h"
#include <bcrypt.h>
#include <vector>

namespace DapaCsxDraw {
// Exact, disassembly-verified VRMenuBridgeDirectDrawHook contracts. Version
// strings alone are insufficient: regular and Paintball DLLs both say 3.19.
// Observe only the fast-path draw and the draw AFTER CSX's suppression test.
// Unknown/rebuilt owners still fail closed; never bypass menu decisions.
struct Build {
    const char* name;
    uint32_t ownerRva;
    size_t ownerSize;
    std::array<uint8_t,32> ownerSha256;
    std::array<DapaEngineDraw::Site,2> sites;
};
inline constexpr std::array<Build,8> builds{{
    {"MGO CSX 3.18 (A86BFFCC)",0xe0170,0x356,
     {0xab,0x32,0x4b,0xb4,0x51,0xf5,0x9b,0x11,0x03,0x3b,0x8c,0xc3,0xde,0x73,0x94,0xb4,
      0x8b,0xf9,0xc4,0xb7,0xa6,0x85,0x9f,0x30,0xa1,0xd3,0x5d,0x42,0x8e,0x38,0xb8,0x0c},
     {{{0xe01dc,true,7,0,{0x41,0xff,0x92,0xa0,0,0,0}},
       {0xe0482,true,6,0,{0xff,0x90,0xa0,0,0,0}}}}},
    {"CSX Paintball 3.19 (C192935E)",0x105bd0,0x384,
     {0x2b,0x5b,0xc1,0x45,0xd2,0xa4,0xd7,0xf9,0xc9,0x79,0xdf,0x11,0x7f,0xa7,0x24,0xec,
      0xb4,0x9b,0x14,0xe8,0x15,0x83,0x02,0x87,0xb4,0x39,0x84,0xf2,0xbd,0x92,0xef,0x24},
     {{{0x105c3c,true,7,0,{0x41,0xff,0x92,0xa0,0,0,0}},
       {0x105f10,true,6,0,{0xff,0x90,0xa0,0,0,0}}}}},
    {"CSX 3.18 (BCDECB99)",0xe01a0,0x356,
     {0x61,0x7b,0x54,0xc5,0x30,0xce,0xdd,0x95,0x38,0xa4,0x66,0xe5,0x43,0xa6,0x11,0xd5,
      0xe2,0xb0,0xbe,0xfb,0x49,0xc4,0xf6,0x04,0xe2,0x27,0x3f,0x4f,0xee,0xb5,0xb4,0x83},
     {{{0xe020c,true,7,0,{0x41,0xff,0x92,0xa0,0,0,0}},
       {0xe04b2,true,6,0,{0xff,0x90,0xa0,0,0,0}}}}},
    {"CSX 3.19 (04CBC257)",0xf5460,0x384,
     {0x1e,0xc9,0xe2,0x17,0xd6,0x49,0x08,0x58,0x9a,0x70,0x4c,0xb5,0x25,0xfe,0xa1,0xfb,
      0xff,0xe5,0x35,0x8a,0xbe,0xcc,0x3b,0xcb,0x12,0xe6,0x31,0x30,0xc6,0xa2,0xf4,0x1f},
     {{{0xf54cc,true,7,0,{0x41,0xff,0x92,0xa0,0,0,0}},
       {0xf57a0,true,6,0,{0xff,0x90,0xa0,0,0,0}}}}},
    {"Treatid CSX 3.19 PR20 (D198742E)",0x10edb0,0x386,
     {0x62,0xd6,0xf5,0x32,0x8f,0x21,0xe4,0xf0,0xcc,0x55,0xaa,0xa4,0x9f,0x3b,0xb6,0xe4,
      0xd2,0xe8,0x07,0x11,0xce,0xc0,0xc4,0x5d,0xf3,0xfc,0xe8,0x8a,0x68,0x29,0x04,0x90},
     {{{0x10ee1c,true,7,0,{0x41,0xff,0x92,0xa0,0,0,0}},
       {0x10f0f2,true,6,0,{0xff,0x90,0xa0,0,0,0}}}}},
    {"Treatid CSX 3.19 PR21 (5C3F56BA)",0x10e5c0,0x386,
     {0xab,0xe2,0x7d,0xd5,0x1e,0xec,0x9a,0x96,0x65,0xa8,0xd7,0xb8,0x97,0xa3,0xa0,0x5a,
      0xce,0xa8,0xe6,0xd4,0xc1,0x57,0x93,0x61,0x5c,0x9e,0xe0,0xa5,0x4a,0x11,0x4c,0x77},
     {{{0x10e62c,true,7,0,{0x41,0xff,0x92,0xa0,0,0,0}},
       {0x10e902,true,6,0,{0xff,0x90,0xa0,0,0,0}}}}},
    {"CSX AIO VR 1595cffd (B5D67479)",0x10e240,0x386,
     {0x81,0xf4,0x08,0x49,0xd9,0x8f,0xfc,0x1b,0x84,0xb6,0x8a,0x6c,0x0c,0x86,0xf5,0xa8,
      0x71,0x06,0x15,0x50,0x06,0xa6,0x78,0xe9,0xe2,0x10,0xc1,0x97,0xd1,0x4b,0x13,0xb6},
     {{{0x10e2ac,true,7,0,{0x41,0xff,0x92,0xa0,0,0,0}},
       {0x10e582,true,6,0,{0xff,0x90,0xa0,0,0,0}}}}},
    {"MGO CSX (E8E7686C)",0xf78f0,0x386,
     {0xd2,0x10,0xbd,0x48,0xea,0x46,0xe9,0x90,0x36,0x8b,0x95,0x21,0x95,0x4f,0xb7,0x3b,
      0xca,0xce,0x1b,0x63,0x52,0xa3,0xfc,0x69,0xb3,0x3b,0xea,0x1a,0x25,0xe0,0xec,0xa2},
     {{{0xf795c,true,7,0,{0x41,0xff,0x92,0xa0,0,0,0}},
       {0xf7c32,true,6,0,{0xff,0x90,0xa0,0,0,0}}}}}
}};

inline bool MatchesOwner(const Build& build,const void* function) {
    std::vector<uint8_t> bytes(build.ownerSize);
    SIZE_T read=0;
    if(!ReadProcessMemory(GetCurrentProcess(),function,bytes.data(),bytes.size(),&read) || read!=bytes.size())return false;
    std::array<uint8_t,32> hash{};
    BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE handle=nullptr;
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)return false;
    DWORD objectSize=0,resultSize=0;
    bool ok=BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&objectSize),sizeof(objectSize),&resultSize,0)>=0;
    std::vector<uint8_t> object(ok?objectSize:0);
    if(ok)ok=BCryptCreateHash(algorithm,&handle,object.data(),objectSize,nullptr,0,0)>=0;
    if(ok)ok=BCryptHashData(handle,bytes.data(),ULONG(bytes.size()),0)>=0;
    if(ok)ok=BCryptFinishHash(handle,hash.data(),ULONG(hash.size()),0)>=0;
    if(handle)BCryptDestroyHash(handle);
    BCryptCloseAlgorithmProvider(algorithm,0);
    return ok && hash==build.ownerSha256;
}

inline const Build* FindBuild(uintptr_t module,const void* owner) {
    for(const auto& build:builds)
        if(reinterpret_cast<uintptr_t>(owner)==module+build.ownerRva && MatchesOwner(build,owner))
            return &build;
    return nullptr;
}

// A known function may move without changing its machine code. Inspect the
// actual chained owner, not a guessed address or a module-wide short signature.
// Changed RIP-relative operands still require a separately verified contract.
inline bool RebaseKnownOwner(const Build& known,uintptr_t module,const void* owner,Build& result) {
    const auto address=reinterpret_cast<uintptr_t>(owner);
    if(address<module || address-module>UINT32_MAX)return false;
    DWORD64 imageBase=0;
    const auto* function=RtlLookupFunctionEntry(address,&imageBase,nullptr);
    if(!function || imageBase!=module || module+function->BeginAddress!=address ||
       function->EndAddress-function->BeginAddress!=known.ownerSize)return false;
    MEMORY_BASIC_INFORMATION region{};
    if(!VirtualQuery(owner,&region,sizeof(region)) || region.Type!=MEM_IMAGE ||
       region.AllocationBase!=reinterpret_cast<void*>(module) || region.State!=MEM_COMMIT ||
       (region.Protect&PAGE_GUARD) ||
       !(region.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)))return false;
    if(!MatchesOwner(known,owner))return false;
    result=known;
    result.ownerRva=static_cast<uint32_t>(address-module);
    for(auto& site:result.sites) {
        if(site.rva<known.ownerRva || site.rva-known.ownerRva+site.size>known.ownerSize)return false;
        site.rva=result.ownerRva+(site.rva-known.ownerRva);
    }
    return true;
}

// Non-fatal near allocation. The page stays resident for any in-flight callback.
inline uint8_t* AllocateNear(uintptr_t address) {
    SYSTEM_INFO info{};GetSystemInfo(&info);
    const uintptr_t step=info.dwAllocationGranularity;
    const uintptr_t lower=address>0x70000000?address-0x70000000:step;
    const uintptr_t upper=address+0x70000000;
    for(uintptr_t probe=(lower+step-1)&~(step-1);probe<upper;) {
        MEMORY_BASIC_INFORMATION region{};
        if(!VirtualQuery(reinterpret_cast<void*>(probe),&region,sizeof(region)))return nullptr;
        const uintptr_t end=reinterpret_cast<uintptr_t>(region.BaseAddress)+region.RegionSize;
        if(end<=probe)return nullptr;
        if(region.State==MEM_FREE && end-probe>=4096) {
            if(auto* memory=VirtualAlloc(reinterpret_cast<void*>(probe),4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE))
                return static_cast<uint8_t*>(memory);
        }
        probe=(end+step-1)&~(step-1);
    }
    return nullptr;
}

struct Adapter {
    const Build* build=nullptr;
    Build relocatedBuild{};
    bool relocated=false;
    std::array<DapaEngineDraw::Hook,2> hooks{};
    uint8_t* code=nullptr;
    DWORD error=0;
    bool Prepare(uintptr_t module,void* owner,uintptr_t callback) {
        build=FindBuild(module,owner);
        if(!build)for(const auto& known:builds) {
            if(RebaseKnownOwner(known,module,owner,relocatedBuild)) {
                build=&relocatedBuild;relocated=true;break;
            }
        }
        if(!build) {
            error=ERROR_REVISION_MISMATCH;return false;
        }
        for(size_t i=0;i<hooks.size();++i) {
            if(!hooks[i].Prepare(build->sites[i],reinterpret_cast<uint8_t*>(module+build->sites[i].rva)) || hooks[i].chained) {
                error=ERROR_INVALID_DATA;return false;
            }
        }
        code=AllocateNear(module+build->ownerRva);
        if(!code) {error=ERROR_NOT_ENOUGH_MEMORY;return false;}
        for(size_t i=0;i<hooks.size();++i)if(!hooks[i].Build(code+i*64,callback)) {
            error=hooks[i].error;return false;
        }
        DWORD old=0;
        if(!VirtualProtect(code,4096,PAGE_EXECUTE_READ,&old)) {error=GetLastError();return false;}
        FlushInstructionCache(GetCurrentProcess(),code,128);
        return true;
    }
    bool Install() {
        for(auto& hook:hooks)if(!hook.Install()) {error=hook.error;Remove();return false;}
        return true;
    }
    bool Remove() {
        bool ok=true;
        for(auto& hook:hooks)if(!hook.Remove()) {error=hook.error;ok=false;}
        return ok;
    }
};
}
