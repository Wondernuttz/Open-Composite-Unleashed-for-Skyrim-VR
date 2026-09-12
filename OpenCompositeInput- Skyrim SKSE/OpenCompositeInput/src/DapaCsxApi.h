#pragma once
#include "../include/CSXAcceptedDrawAPI.h"
#include <atomic>

namespace DapaCsxApi {
using namespace CSXAcceptedDrawAPI;
enum class ConnectResult { Connected, Missing, Incompatible, RegistrationFailed, AlreadyConnected };
class Client {
    const API* api=nullptr;
    uint64_t subscription=0, lastDrawId=0;
    ID3D11DeviceContext* expectedContext=nullptr;
    ObserverFn receiver=nullptr;
    void* receiverUser=nullptr;
    void(*onFault)(void*)=nullptr;
    std::atomic<bool> active=false;
    inline static thread_local bool delivering=false;
    static void __cdecl Observe(const Draw* draw,void* user) noexcept {
        auto& self=*static_cast<Client*>(user);
        if(!self.active.load(std::memory_order_acquire) || delivering || !draw)return;
        if(draw->structSize<sizeof(Draw) || draw->version!=Version ||
           !draw->drawId || draw->drawId<=self.lastDrawId ||
           draw->context!=self.expectedContext || !draw->geometry || !draw->sceneDepth ||
           draw->pass!=MainScene || draw->stereoLayout!=PackedStereo2D || !draw->replay ||
           !draw->arguments.indexCount)return;
        const auto& a=draw->arguments;
        if(a.kind!=Indexed && a.kind!=IndexedInstanced)return;
        if((a.kind==Indexed && (a.instanceCount!=1 || a.startInstance!=0)) ||
           (a.kind==IndexedInstanced && !a.instanceCount))return;
        self.lastDrawId=draw->drawId;
        delivering=true;
        // Never unwind through the provider's ABI. The receiver must also restore
        // any modified GPU state before returning (including failed replay).
        try { self.receiver(draw,self.receiverUser); } catch(...) {
            self.active.store(false,std::memory_order_release);
            try {if(self.onFault)self.onFault(self.receiverUser);} catch(...) {}
        }
        delivering=false;
    }
public:
    Client()=default;
    Client(const Client&)=delete;
    Client& operator=(const Client&)=delete;
    // Explicit Stop required before destruction/module unload, not from callback.
    ConnectResult Connect(QueryFn query,ID3D11DeviceContext* context,ObserverFn callback,void* user,void(*fault)(void*)=nullptr) {
        if(api)return ConnectResult::AlreadyConnected;
        if(!query)return ConnectResult::Missing;
        const auto* candidate=query(Version,sizeof(API));
        if(!context || !callback || !candidate || candidate->structSize<sizeof(API) || candidate->version!=Version ||
           (candidate->capabilities&RequiredCapabilities)!=RequiredCapabilities ||
           !candidate->registerObserver || !candidate->unregisterObserver)return ConnectResult::Incompatible;
        expectedContext=context;receiver=callback;receiverUser=user;onFault=fault;lastDrawId=0;
        uint64_t token=0;
        const auto result=candidate->registerObserver(&Observe,this,&token);
        if(result!=Success || !token) {
            // Conforming failure registers nothing. Retain an unexpected token
            // if cleanup fails so a second subscription cannot be attempted.
            if(token && candidate->unregisterObserver(token)!=Success) {api=candidate;subscription=token;}
            return ConnectResult::RegistrationFailed;
        }
        api=candidate;subscription=token;
        active.store(true,std::memory_order_release);
        return ConnectResult::Connected;
    }
    bool Stop() {
        active.store(false,std::memory_order_release);
        if(!api)return true;
        if(api->unregisterObserver(subscription)!=Success)return false;
        api=nullptr;subscription=0;receiver=nullptr;receiverUser=nullptr;
        return true;
    }
};
}
