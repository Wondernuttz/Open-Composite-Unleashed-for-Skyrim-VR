#include "../src/DapaEngineDraw.h"
#include <cstdio>
#include <vector>
#include <stdexcept>

struct Context {void** table;};
using Draw=void(*)(Context*,UINT,UINT,INT);
using Instanced=void(*)(Context*,UINT,UINT,UINT,INT,UINT);
struct Record {UINT n=0,instances=0,start=0;INT base=0;UINT first=0;};
Record record;
unsigned nativeA=0,nativeB=0,callbacks=0,ownerCalls=0;
void* chained=nullptr;
void DrawA(Context*,UINT n,UINT start,INT base) {++nativeA;record={n,1,start,base,0};}
void DrawB(Context*,UINT n,UINT start,INT base) {++nativeB;record={n,1,start,base,0};}
void InstancedA(Context*,UINT n,UINT count,UINT start,INT base,UINT first) {++nativeA;record={n,count,start,base,first};}
void InstancedB(Context*,UINT n,UINT count,UINT start,INT base,UINT first) {++nativeB;record={n,count,start,base,first};}
void Owner(Context* ctx,UINT n,UINT count,UINT start,INT base,UINT first) {
    ++ownerCalls;reinterpret_cast<Instanced>(ctx->table[20])(ctx,n,count,start,base,first);
}
void CallbackDraw(Context* ctx,UINT n,UINT start,INT base) {
    ++callbacks;reinterpret_cast<Draw>(ctx->table[12])(ctx,n,start,base);
}
void CallbackInstanced(Context* ctx,UINT n,UINT count,UINT start,INT base,UINT first) {
    ++callbacks;
    const auto next=chained?reinterpret_cast<Instanced>(chained):reinterpret_cast<Instanced>(ctx->table[20]);
    next(ctx,n,count,start,base,first);
}
void Require(bool condition,const char* why) {if(!condition)throw std::runtime_error(why);}

void Test(const DapaEngineDraw::Site& site,bool existingOwner) {
    auto* memory=static_cast<uint8_t*>(VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_EXECUTE_READWRITE));
    Require(memory!=nullptr,"allocation");
    // Windows x64 ABI: preserve three nonvolatile registers, align stack, retain
    // shadow space plus both spilled arguments. Match each real argument prefix.
    std::vector<uint8_t> code{
        0x53,0x56,0x57,0x48,0x83,0xec,0x30,
        0xba,123,0,0,0, 0x41,0xb8,2,0,0,0, 0x41,0xb9,7,0,0,0,
        0xbf,11,0,0,0, 0xbb,12,0,0,0, 0xbe,13,0,0,0,
        0xc7,0x44,0x24,0x20,0xf7,0xff,0xff,0xff,
        0xc7,0x44,0x24,0x28,17,0,0,0,
        0x48,0x8b,0x01, 0x49,0x89,0xc2
    };
    const auto offset=code.size();
    code.insert(code.end(),site.expected.begin(),site.expected.begin()+site.size);
    code.insert(code.end(),{0x48,0x83,0xc4,0x30,0x5f,0x5e,0x5b,0xc3});
    std::memcpy(memory,code.data(),code.size());
    if(existingOwner) {
        Require(site.instanced && site.size==6,"owner fixture site");
        const auto target=reinterpret_cast<uintptr_t>(&Owner);
        std::memcpy(memory+1024,&target,8);
        memory[offset]=0xff;memory[offset+1]=0x15;
        const int32_t displacement=static_cast<int32_t>(1024-offset-6);
        std::memcpy(memory+offset+2,&displacement,4);
    }
    FlushInstructionCache(GetCurrentProcess(),memory,code.size());
    void* table[21]{};table[12]=reinterpret_cast<void*>(&DrawA);table[20]=reinterpret_cast<void*>(&InstancedA);
    Context ctx{table};
    auto function=reinterpret_cast<void(*)(Context*)>(memory);
    auto validate=[&] {
        Require(record.n==123,"index count corrupted");
        if(site.instanced)Require(record.instances==2 && record.start==7 && record.base==-9 && record.first==17,"instanced/spilled arguments corrupted");
        else {
            const UINT start=site.prefix==3?0:site.expected[5]==0xc7?11:site.expected[5]==0xc3?12:13;
            Require(record.instances==1 && record.start==start && record.base==(site.prefix==3?7:0),"indexed argument prefix corrupted");
        }
    };
    nativeA=nativeB=callbacks=ownerCalls=0;
    function(&ctx);validate();Require(nativeA==1 && callbacks==0,"native fixture");
    DapaEngineDraw::Hook hook;
    Require(hook.Prepare(site,memory+offset),"prepare rejected valid call");
    chained=hook.chained;
    const auto callback=site.instanced?reinterpret_cast<uintptr_t>(&CallbackInstanced):reinterpret_cast<uintptr_t>(&CallbackDraw);
    Require(hook.Build(memory+512,callback),"build");
    DWORD old=0;VirtualProtect(memory,4096,PAGE_EXECUTE_READ,&old);
    Require(hook.Install(),"install");
    function(&ctx);validate();Require(nativeA==2 && callbacks==1,"first hooked call");
    // Reproduce the live failure: D3D replaces its dispatch entries. The engine
    // hook must survive and use the NEW dispatch, not the stale A function.
    table[12]=reinterpret_cast<void*>(&DrawB);table[20]=reinterpret_cast<void*>(&InstancedB);
    function(&ctx);validate();Require(nativeB==1 && callbacks==2,"mutable dispatch lost interception");
    if(existingOwner)Require(ownerCalls==3,"existing owner bypassed or called twice");
    MEMORY_BASIC_INFORMATION region{};VirtualQuery(memory,&region,sizeof(region));
    Require(region.Protect==PAGE_EXECUTE_READ,"code protection not restored");
    Require(hook.Remove(),"rollback");
    function(&ctx);validate();Require(nativeB==2 && callbacks==2,"rollback did not restore original");
    if(existingOwner)Require(ownerCalls==4,"rollback lost prior owner");
    // Unknown modifications must be rejected rather than stomped.
    VirtualProtect(memory,4096,PAGE_EXECUTE_READWRITE,&old);memory[offset]=0xcc;
    DapaEngineDraw::Hook unknown;
    Require(!unknown.Prepare(site,memory+offset) && memory[offset]==0xcc,"unknown hook overwritten");
    VirtualFree(memory,0,MEM_RELEASE);
    std::printf("PASS RVA %06X %s%s: ABI, mutable dispatch, chaining, rollback\n",site.rva,site.instanced?"instanced":"indexed",existingOwner?" with existing owner":"");
}
int main() {
    try {
        for(const auto& site:DapaEngineDraw::sites)Test(site,false);
        Test(DapaEngineDraw::sites[0],true);
        std::puts("PASS: all 17 exact mesh-call instruction fixtures plus existing-owner chain");
        return 0;
    } catch(const std::exception& error) {
        std::printf("FAIL: %s\n",error.what());return 1;
    }
}
