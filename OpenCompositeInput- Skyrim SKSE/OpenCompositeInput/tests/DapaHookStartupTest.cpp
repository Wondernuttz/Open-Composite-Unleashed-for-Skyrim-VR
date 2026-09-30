#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <MinHook.h>
#include <cstdio>
#include "../src/DapaVtableSlot.h"

using DrawFn=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT,INT);
using InstancedFn=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT,UINT,INT,UINT);
DrawFn originalDraw=nullptr;
InstancedFn originalInstanced=nullptr;
unsigned drawCalls=0,instancedCalls=0;
DrawFn chainedDraw=nullptr;
InstancedFn chainedInstanced=nullptr;
unsigned slotDrawCalls=0,slotInstancedCalls=0;
void STDMETHODCALLTYPE SlotDraw(ID3D11DeviceContext* ctx,UINT n,UINT start,INT base) {
    ++slotDrawCalls;chainedDraw(ctx,n,start,base);
}
void STDMETHODCALLTYPE SlotInstanced(ID3D11DeviceContext* ctx,UINT n,UINT count,UINT start,INT base,UINT first) {
    ++slotInstancedCalls;chainedInstanced(ctx,n,count,start,base,first);
}
void STDMETHODCALLTYPE Draw(ID3D11DeviceContext* ctx,UINT n,UINT start,INT base) {
    ++drawCalls;originalDraw(ctx,n,start,base);
}
void STDMETHODCALLTYPE Instanced(ID3D11DeviceContext* ctx,UINT n,UINT count,UINT start,INT base,UINT first) {
    ++instancedCalls;originalInstanced(ctx,n,count,start,base,first);
}
int main() {
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> ctx;
    D3D_FEATURE_LEVEL level{};
    auto hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,
        D3D11_SDK_VERSION,&device,&level,&ctx);
    if(FAILED(hr)) {std::printf("D3D11CreateDevice failed %08lx\n",hr);return 1;}
    auto** table=*reinterpret_cast<void***>(ctx.Get());
    auto report=[](const char* stage,MH_STATUS status) {
        std::printf("%s: %s (%d)\n",stage,MH_StatusToString(status),status);return status==MH_OK;
    };
    std::printf("Immediate %p DrawIndexed %p DrawIndexedInstanced %p\n",ctx.Get(),table[12],table[20]);
    if(!report("initialize",MH_Initialize()))return 2;
    if(!report("create DrawIndexed",MH_CreateHook(table[12],reinterpret_cast<void*>(&Draw),reinterpret_cast<void**>(&originalDraw))))return 3;
    if(!report("create DrawIndexedInstanced",MH_CreateHook(table[20],reinterpret_cast<void*>(&Instanced),reinterpret_cast<void**>(&originalInstanced))))return 4;
    if(!report("enable DrawIndexed",MH_EnableHook(table[12])))return 5;
    if(!report("enable DrawIndexedInstanced",MH_EnableHook(table[20])))return 6;
    // Zero-count calls exercise dispatch without submitting geometry.
    ctx->DrawIndexed(0,0,0);ctx->DrawIndexedInstanced(0,0,0,0,0);
    std::printf("Dispatch counts: indexed=%u instanced=%u\n",drawCalls,instancedCalls);
    if(drawCalls!=1 || instancedCalls!=1)return 7;
    // Exercise the production slot helper with existing inline detours in place.
    // Each previous hook must still run, once, with no recursive dispatch.
    DapaVtableSlot indexed,instanced;
    if(!indexed.Install(table+12,reinterpret_cast<void*>(&SlotDraw),reinterpret_cast<void**>(&chainedDraw)) ||
       !instanced.Install(table+20,reinterpret_cast<void*>(&SlotInstanced),reinterpret_cast<void**>(&chainedInstanced)) ||
       indexed.protectionError || instanced.protectionError)return 8;
    ctx->DrawIndexed(0,0,0);ctx->DrawIndexedInstanced(0,0,0,0,0);
    if(drawCalls!=2 || instancedCalls!=2 || slotDrawCalls!=1 || slotInstancedCalls!=1)return 9;
    if(!instanced.Remove() || !indexed.Remove())return 10;
    ctx->DrawIndexed(0,0,0);ctx->DrawIndexedInstanced(0,0,0,0,0);
    if(drawCalls!=3 || instancedCalls!=3 || slotDrawCalls!=1 || slotInstancedCalls!=1)return 11;
    std::puts("PASS: renderer slots chain existing inline detours, both signatures, removal restores previous hooks");
    report("disable indexed",MH_DisableHook(table[12]));
    report("disable instanced",MH_DisableHook(table[20]));
    MH_Uninitialize();
    // Synthetic read-only vtable checks failure, rollback and later-hook ownership.
    auto** page=static_cast<void**>(VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    if(!page)return 12;
    page[0]=reinterpret_cast<void*>(&Draw);DWORD old=0;
    VirtualProtect(page,4096,PAGE_READONLY,&old);
    DapaVtableSlot first,later,bad;void* before=nullptr;void* middle=nullptr;void* ignored=nullptr;
    if(!first.Install(page,reinterpret_cast<void*>(&SlotDraw),&before))return 13;
    if(first.Install(page,reinterpret_cast<void*>(&SlotDraw),&before))return 14;
    if(bad.Install(page+1,reinterpret_cast<void*>(&SlotDraw),&ignored) || bad.error!=ERROR_INVALID_DATA)return 15;
    MEMORY_BASIC_INFORMATION region{};VirtualQuery(page,&region,sizeof(region));
    if(region.Protect!=PAGE_READONLY)return 16;
    if(!later.Install(page,reinterpret_cast<void*>(&Instanced),&middle))return 17;
    if(first.Remove() || first.error!=ERROR_BUSY || page[0]!=reinterpret_cast<void*>(&Instanced))return 18;
    if(!later.Remove() || !first.Remove() || page[0]!=reinterpret_cast<void*>(&Draw))return 19;
    if(bad.Install(nullptr,reinterpret_cast<void*>(&Draw),&ignored) || bad.error!=ERROR_INVALID_PARAMETER)return 20;
    VirtualFree(page,0,MEM_RELEASE);
    std::puts("PASS: failure reported, repeat rejected, read-only protection restored, rollback preserves later hooks");
    return 0;
}
