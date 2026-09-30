#include "../src/DapaHeldRoots.h"
#include "../src/DapaHiggsApi.h"
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>

static void Check(bool value,const char* message) {
    if(!value) { std::cerr<<"FAIL: "<<message<<'\n';std::exit(1); }
}
struct Node { Node* parent=nullptr; };
using Roots=DapaHeldRoots<Node,std::shared_ptr<Node>>;
static bool Owned(Roots& roots,Node* node) {
    const auto snapshot=roots.Read();
    for(unsigned i=0;node && i<64;++i,node=node->parent)
        if(roots.Matches(node,snapshot))return true;
    return false;
}
// Independent raw vtable fixture, NOT a subclass of the adapter: catches slot
// shifts/destructor insertion when compiled with the same Windows x64 ABI.
namespace Abi {
unsigned calls[32]{};
void* table[32]{};
struct Object { void** vtable=table; } object;
unsigned Build(void*) {++calls[0];return 1101000;}
void Drop(void*,DapaHiggs::Interface::ReferenceCallback) {++calls[3];}
void Stash(void*,DapaHiggs::Interface::FormCallback) {++calls[4];}
void Consume(void*,DapaHiggs::Interface::FormCallback) {++calls[5];}
RE::TESObjectREFR* Grabbed(void*,bool left) {
    ++calls[8];return reinterpret_cast<RE::TESObjectREFR*>(std::uintptr_t(left?0x1230:0x4560));
}
void Post(void*,DapaHiggs::Interface::Callback) {++calls[31];}
__declspec(noinline) void Invoke(DapaHiggs::Interface* api) {
    Check(api->GetBuildNumber()==1101000,"build slot");
    api->AddDroppedCallback(nullptr);api->AddStashedCallback(nullptr);api->AddConsumedCallback(nullptr);
    Check(api->GetGrabbedObject(true)==reinterpret_cast<RE::TESObjectREFR*>(0x1230),"left ABI");
    Check(api->GetGrabbedObject(false)==reinterpret_cast<RE::TESObjectREFR*>(0x4560),"right ABI");
    api->AddPreVrikPostHiggsCallback(nullptr);
}
void Run() {
    table[0]=reinterpret_cast<void*>(&Build);table[3]=reinterpret_cast<void*>(&Drop);
    table[4]=reinterpret_cast<void*>(&Stash);table[5]=reinterpret_cast<void*>(&Consume);
    table[8]=reinterpret_cast<void*>(&Grabbed);table[31]=reinterpret_cast<void*>(&Post);
    Invoke(reinterpret_cast<DapaHiggs::Interface*>(&object));
    Check(calls[0]==1 && calls[3]==1 && calls[4]==1 && calls[5]==1 && calls[8]==2 && calls[31]==1,"exact API dispatch");
    Check(DapaHiggs::Supported(1101000) && !DapaHiggs::Supported(1100900) && !DapaHiggs::Supported(2000000),"version guard");
}
}
int main() {
    Roots roots;
    auto bottle=std::make_shared<Node>();auto staff=std::make_shared<Node>();
    Node cap{bottle.get()},ornament{staff.get()},world;
    Check(!Owned(roots,&cap),"HIGGS absent starts empty");
    Check(roots.Set(true,bottle),"left grab");
    Check(!roots.Set(true,bottle),"stable update does not replace root");
    Check(Owned(roots,&cap) && !Owned(roots,&world),"whole held mesh, no world selection");
    roots.Set(false,staff);
    Check(Owned(roots,&cap) && Owned(roots,&ornament),"independent hands");
    auto old=roots.Read();
    roots.Set(true,{});
    Check(!roots.Matches(bottle.get(),old) && !Owned(roots,&cap),"release invalidates stale snapshot");
    Check(Owned(roots,&ornament),"one-hand release leaves other hand intact");
    roots.Set(true,staff);roots.Set(false,{});
    Check(Owned(roots,&ornament),"handoff/dual hand drop keeps remaining owner");
    roots.Clear();
    Check(!Owned(roots,&ornament),"consume/stash/load clears ownership");
    auto replacement=std::make_shared<Node>();roots.Set(false,replacement);
    Node replacedMesh{replacement.get()};
    Check(Owned(roots,&replacedMesh) && !Owned(roots,&ornament),"model/root replacement");
    std::weak_ptr<Node> lifetime=replacement;replacement.reset();
    Check(!lifetime.expired(),"root is pinned while held");
    roots.Clear();Check(lifetime.expired(),"release frees root pin");
    std::thread writer([&]{for(unsigned i=0;i<50000;++i){roots.Set(true,bottle);roots.Set(true,{});}});
    for(unsigned i=0;i<50000;++i)Check(!Owned(roots,&world),"concurrent refresh never classifies world");
    writer.join();Check(!Owned(roots,&cap),"final release after concurrent reads");
    Abi::Run();
    std::cout<<"PASS: HIGGS x64 API slots, version gate, independent hands, handoff, release, stale snapshot, root lifetime, model changes and concurrent classification\n";
}
