#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <chrono>
#include <cstdint>

// One sample contains both capture and correction intervals from the SAME pair.
// Missing stages discard the sample; polling never flushes or waits for the GPU.
class DapaCharacterTiming {
    using Query=Microsoft::WRL::ComPtr<ID3D11Query>;
    Query disjoint;
    std::array<Query,4> begin,end;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> activeContext;
    std::uint64_t serial=0,sampledSerial=0,nextMs=0;
    unsigned completed=0;
    bool recording=false,pending=false,unavailable=false;
    static std::uint64_t Now() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    void Finish() {
        if(recording){activeContext->End(disjoint.Get());recording=false;pending=true;activeContext.Reset();}
    }
    bool Begin(ID3D11DeviceContext* ctx,unsigned stage,bool warm) {
        if(!ctx || stage>=4 || unavailable || pending)return false;
        if(stage==0 && warm && !recording && Now()>=nextMs) {
            if(!disjoint) {
                Microsoft::WRL::ComPtr<ID3D11Device> device;ctx->GetDevice(&device);
                D3D11_QUERY_DESC desc{D3D11_QUERY_TIMESTAMP_DISJOINT,0};
                HRESULT hr=device->CreateQuery(&desc,&disjoint);
                desc.Query=D3D11_QUERY_TIMESTAMP;
                for(unsigned i=0;i<4 && SUCCEEDED(hr);++i) {
                    hr=device->CreateQuery(&desc,&begin[i]);
                    if(SUCCEEDED(hr))hr=device->CreateQuery(&desc,&end[i]);
                }
                if(FAILED(hr)){unavailable=true;return false;}
            }
            sampledSerial=serial;completed=0;recording=true;activeContext=ctx;
            nextMs=Now()+1000;ctx->Begin(disjoint.Get());
        }
        if(!warm || !recording || sampledSerial!=serial || (completed&(1u<<stage)))return false;
        ctx->End(begin[stage].Get());return true;
    }
    void End(ID3D11DeviceContext* ctx,unsigned stage) {
        ctx->End(end[stage].Get());completed|=1u<<stage;
        if(stage==3)Finish();
    }
public:
    static std::uint64_t NowMs(){return Now();}
    class Scope {
        DapaCharacterTiming& owner;ID3D11DeviceContext* ctx;unsigned stage;bool active;
    public:
        Scope(DapaCharacterTiming& o,ID3D11DeviceContext* c,unsigned s,bool warm)
            :owner(o),ctx(c),stage(s),active(o.Begin(c,s,warm)){}
        ~Scope(){if(active)owner.End(ctx,stage);}
        Scope(const Scope&)=delete;
    };
    Scope Measure(ID3D11DeviceContext* ctx,unsigned stage,bool warm){return Scope(*this,ctx,stage,warm);}
    void BeginPair(){Finish();++serial;}
    void Reset(){Finish();disjoint.Reset();begin={};end={};activeContext.Reset();serial=sampledSerial=nextMs=0;completed=0;recording=pending=unavailable=false;}
    ~DapaCharacterTiming(){Finish();}
    template<class Report> void Poll(ID3D11DeviceContext* ctx,Report report) {
        if(!ctx || !pending)return;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT frequency{};
        HRESULT hr=ctx->GetData(disjoint.Get(),&frequency,sizeof(frequency),D3D11_ASYNC_GETDATA_DONOTFLUSH);
        if(hr==S_FALSE)return;
        if(FAILED(hr)||frequency.Disjoint||!frequency.Frequency||completed!=15){pending=false;return;}
        std::array<double,4> costs{};
        for(unsigned i=0;i<4;++i) {
            std::uint64_t a{},b{};
            hr=ctx->GetData(begin[i].Get(),&a,sizeof(a),D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if(hr==S_FALSE)return;
            if(SUCCEEDED(hr))hr=ctx->GetData(end[i].Get(),&b,sizeof(b),D3D11_ASYNC_GETDATA_DONOTFLUSH);
            if(hr==S_FALSE)return;
            if(FAILED(hr)||b<a){pending=false;return;}
            costs[i]=double(b-a)*1000.0/frequency.Frequency;
        }
        pending=false;report(costs);
    }
};

struct DapaCharacterBudget {
    unsigned consecutiveOver=0,stableSamples=0,recoveryAttempts=0;
    bool disabled=false;
    double tripMs=0;
    std::uint64_t retryAtMs=0,nextDelayMs=2000;
    void Observe(double total,std::uint64_t now=0) {
        if(disabled)return;
        if(total<=1.5) {
            consecutiveOver=0;
            if(++stableSamples>=10)nextDelayMs=2000;
            return;
        }
        stableSamples=0;
        if(++consecutiveOver>=3) {
            disabled=true;tripMs=total;retryAtMs=now+nextDelayMs;
            nextDelayMs=(nextDelayMs<15000)?nextDelayMs*2:30000;
        }
    }
    bool TryRecover(std::uint64_t now) {
        if(!disabled || now<retryAtMs)return false;
        disabled=false;consecutiveOver=stableSamples=0;++recoveryAttempts;
        return true;
    }
    std::uint64_t RemainingMs(std::uint64_t now) const {
        return disabled && now<retryAtMs ? retryAtMs-now : 0;
    }
};
