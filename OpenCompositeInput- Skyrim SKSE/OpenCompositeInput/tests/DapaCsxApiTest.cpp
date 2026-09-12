#include "../src/DapaCsxApi.h"
#include <cstdio>
#include <stdexcept>
using namespace CSXAcceptedDrawAPI;
void Check(bool value,const char* reason) {if(!value)throw std::runtime_error(reason);}
namespace {
ObserverFn observer=nullptr;
void* observerUser=nullptr;
bool registrationFails=false,unregisterFails=false,throwReceiver=false,owned=true;
unsigned received=0,replayed=0,nativeDraws=0,registrations=0;
unsigned faults=0;
void Fault(void*) {++faults;}
uint64_t nextId=0;
Draw current{};
uint32_t __cdecl Register(ObserverFn callback,void* user,uint64_t* token) {
    ++registrations;*token=0;
    if(registrationFails)return Failed;
    observer=callback;observerUser=user;*token=42;
    observer(&current,observerUser); // Consumer must ignore callbacks before Connect completes.
    return Success;
}
uint32_t __cdecl Unregister(uint64_t token) {
    Check(token==42,"subscription token changed");
    if(unregisterFails)return Failed;
    observer=nullptr;observerUser=nullptr;return Success;
}
API table{sizeof(API),Version,RequiredCapabilities,&Register,&Unregister};
const API* __cdecl Query(uint32_t version,uint32_t size) {
    Check(version==Version && size==sizeof(API),"query contract wrong");return &table;
}
uint32_t __cdecl Replay(void*) {
    ++replayed;
    // Deliberately faulty provider recursion must not recursively mask.
    auto nested=current;nested.drawId=++nextId;
    observer(&nested,observerUser);
    return Success;
}
void __cdecl Receive(const Draw* draw,void*) {
    ++received;
    if(throwReceiver)throw std::runtime_error("receiver failure");
    if(owned)Check(draw->replay(draw->replayToken)==Success,"replay failed");
}
Draw MakeDraw() {
    return {sizeof(Draw),Version,++nextId,reinterpret_cast<ID3D11DeviceContext*>(1),
        reinterpret_cast<void*>(2),reinterpret_cast<ID3D11Texture2D*>(3),MainScene,PackedStereo2D,
        {IndexedInstanced,123,2,7,-9,17},&Replay,nullptr};
}
void Emit(bool accepted=true) {
    current=MakeDraw();
    if(accepted) {++nativeDraws;if(observer)observer(&current,observerUser);}
}
}
int main() {
    try {
        using DapaCsxApi::ConnectResult;
        DapaCsxApi::Client client;
        auto* context=reinterpret_cast<ID3D11DeviceContext*>(1);
        current=MakeDraw();
        Check(client.Connect(nullptr,context,&Receive,nullptr)==ConnectResult::Missing,"missing API not detected");
        for(uint64_t bit=1;bit<=PackedStereoDepth;bit<<=1) {
            table.capabilities=RequiredCapabilities&~bit;
            Check(client.Connect(&Query,context,&Receive,nullptr)==ConnectResult::Incompatible,"partial coverage/capability accepted");
        }
        table.capabilities=RequiredCapabilities;table.version=2;
        Check(client.Connect(&Query,context,&Receive,nullptr)==ConnectResult::Incompatible,"wrong version accepted");
        table.version=Version;table.structSize=sizeof(API)-1;
        Check(client.Connect(&Query,context,&Receive,nullptr)==ConnectResult::Incompatible,"short table accepted");
        table.structSize=sizeof(API);table.unregisterObserver=nullptr;
        Check(client.Connect(&Query,context,&Receive,nullptr)==ConnectResult::Incompatible,"missing unregister accepted");
        table.unregisterObserver=&Unregister;
        Check(registrations==0,"incompatible provider registered");
        registrationFails=true;
        Check(client.Connect(&Query,context,&Receive,nullptr)==ConnectResult::RegistrationFailed,"registration failure hidden");
        registrationFails=false;
        Check(client.Connect(&Query,context,&Receive,nullptr)==ConnectResult::Connected,"valid API rejected");
        Check(received==0,"premature registration callback delivered");
        Check(client.Connect(&Query,context,&Receive,nullptr)==ConnectResult::AlreadyConnected,"double registration");
        Emit();Check(nativeDraws==1 && received==1 && replayed==1,"accepted draw/replay count or recursion wrong");
        observer(&current,observerUser);Check(received==1,"duplicate draw delivered");
        Emit(false);Check(nativeDraws==1 && received==1,"suppressed draw delivered");
        owned=false;Emit();Check(received==2 && replayed==1,"non-player replayed");owned=true;
        const auto valid=MakeDraw();
        auto reject=[&](Draw draw) {const auto before=received;observer(&draw,observerUser);Check(received==before,"invalid event delivered");};
        auto bad=valid;bad.structSize=0;reject(bad);
        bad=valid;bad.version=2;reject(bad);
        bad=valid;bad.context=nullptr;reject(bad);
        bad=valid;bad.geometry=nullptr;reject(bad);
        bad=valid;bad.sceneDepth=nullptr;reject(bad);
        for(auto pass:{Shadow,Reflection,UICapture,Other}) {bad=valid;bad.pass=pass;reject(bad);}
        bad=valid;bad.stereoLayout=2;reject(bad);
        bad=valid;bad.replay=nullptr;reject(bad);
        bad=valid;bad.arguments.kind=99;reject(bad);
        bad=valid;bad.arguments.indexCount=0;reject(bad);
        bad=valid;bad.arguments.instanceCount=0;reject(bad);
        bad=valid;bad.arguments.kind=Indexed;reject(bad);
        current=MakeDraw();current.arguments={Indexed,3,1,0,0,0};
        observer(&current,observerUser);Check(received==3 && replayed==2,"indexed event rejected");
        unregisterFails=true;Check(!client.Stop(),"failed unregister hidden");
        Emit();Check(received==3,"callback delivered after Stop failed");
        unregisterFails=false;Check(client.Stop() && !observer,"unsubscribe failed");
        Check(client.Connect(&Query,context,&Receive,nullptr,&Fault)==ConnectResult::Connected,"reconnect failed");
        throwReceiver=true;Emit();throwReceiver=false;
        const auto before=received;Emit();Check(received==before && faults==1,"throwing receiver remained active or failure was not reported exactly once");
        Check(client.Stop(),"final unsubscribe failed");
        std::puts("PASS proposed API v1: negotiation, required coverage, registration, live events, exact-once, replay recursion, malformed/non-scene events, unsubscribe/reconnect and exception boundary");
        return 0;
    } catch(const std::exception& error) {std::printf("FAIL: %s\n",error.what());return 1;}
}
