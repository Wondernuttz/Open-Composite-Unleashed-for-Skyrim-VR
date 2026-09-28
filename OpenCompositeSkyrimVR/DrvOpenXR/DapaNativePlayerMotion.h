#pragma once
#include "DapaMotion.h"
#include "DapaCharacterTiming.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>

// A private snapshot of the game's vectors. No game resource is modified.
class DapaNativePlayerMotion {
public:
    struct Constants {
        float clipToPrevious[16]{};
        float clipToCurrent[16]{};
        float vectorSize[2]{};
        float fraction=0, enabled=0;
    };
    static_assert(sizeof(Constants)==144);
    struct Camera {
        float jittered[16]{}, current[16]{}, previous[16]{};
        float position[3]{}, previousPosition[3]{};
    };
    void Reset();
    void Shutdown();
    void BeginPair();
    bool Copy(int eye, ID3D11DeviceContext*, ID3D11Texture2D*, const D3D11_BOX*, unsigned width, unsigned height);
    bool Capture(int eye,ID3D11DeviceContext*,ID3D11Texture2D*,const D3D11_BOX*,unsigned,unsigned,
        ID3D11ShaderResourceView* mask,ID3D11ShaderResourceView* depth,unsigned outputWidth,unsigned outputHeight,bool flip);
    bool Apply(int eye,ID3D11DeviceContext*,std::int64_t target,ID3D11ShaderResourceView* color,
        ID3D11ShaderResourceView* mask,ID3D11ShaderResourceView* depth,ID3D11UnorderedAccessView* output);
    bool BudgetExceeded() const { return budget.disabled; }
    unsigned RecoveryAttempts() const { return budget.recoveryAttempts; }
    std::uint64_t RetryRemainingMs() const { return budget.RemainingMs(DapaCharacterTiming::NowMs()); }
    double TripCostMs() const { return budget.tripMs; }
    unsigned OverBudgetSamples() const { return budget.consecutiveOver; }
    double CaptureCostMs() const { return costs[0]+costs[1]; }
    double CorrectionCostMs() const { return costs[2]+costs[3]; }
    double CostMs() const { return costs[0]+costs[1]+costs[2]+costs[3]; }
    void Observe(int eye, const Camera&, std::uint32_t frame, std::int64_t time);
    bool Ready(std::int64_t target) const;
    ID3D11Buffer* Upload(int eye, ID3D11DeviceContext*, std::int64_t target, bool allowed);
    ID3D11ShaderResourceView* View(int eye) const { return eyes[eye].srv.Get(); }
    static bool CameraTransforms(const Camera&, Constants&);
    static float Fraction(std::int64_t current, std::int64_t previous, std::int64_t target);
private:
    template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
    struct Eye {
        Ptr<ID3D11Texture2D> texture,residual;
        Ptr<ID3D11ShaderResourceView> residualSrv;
        Ptr<ID3D11UnorderedAccessView> residualUav;
        bool residualReady=false;
        Ptr<ID3D11ShaderResourceView> srv;
        ID3D11Texture2D* source=nullptr; // identity only, never dereferenced after Copy
        unsigned width=0,height=0;
        DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
        Camera last{};
        Constants data{};
        std::uint32_t lastFrame=0,frame=0;
        std::int64_t lastTime=0,time=0,previousTime=0;
        bool copied=false,ready=false,tilesReady=false;
        Ptr<ID3D11Buffer> tiles,args,tileConstants;
        Ptr<ID3D11UnorderedAccessView> tileUav;
        Ptr<ID3D11ShaderResourceView> tileSrv;
        unsigned outWidth=0,outHeight=0;
        bool flipped=false;
    } eyes[2];
    Ptr<ID3D11Buffer> constants;
    Ptr<ID3D11Device> pipelineDevice;
    Ptr<ID3D11ComputeShader> tileShader,characterShader,residualShader;
    Ptr<ID3D11SamplerState> sampler;
    DapaCharacterTiming timing;
    double costs[4]{};
    DapaCharacterBudget budget;
    bool Prepare(int,ID3D11DeviceContext*,ID3D11ShaderResourceView*,ID3D11ShaderResourceView*,unsigned,unsigned,bool);
    bool Pipeline(ID3D11DeviceContext*);
};
