// Included inside the plugin namespace after the bridge definitions.
// Mask format 2 adds a nonblocking lifetime/content lease in reserved bridge words.
namespace PlayerMask {
    using GeometryFn=void(__fastcall*)(void*,RE::BSRenderPass*,uint32_t);
    using InstancedFn=void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,UINT,UINT,UINT,INT,UINT);
    GeometryFn originalSetup[2]{},originalRestore[2]{};
    DapaPlayerMaskGpu gpu;
    using MaskAccess = DapaMaskBridge::Access<OCRenderTargetBridge>;
    DapaVtableSlot hooks[4];
    std::array<DapaEngineDraw::Hook,DapaEngineDraw::sites.size()> engineHooks{};
    DapaCsxDraw::Adapter csxAdapter;
    DapaCsxApi::Client csxApi;
    bool csxPrepared=false;
    uint64_t csxAccepted=0,csxPlayerAccepted=0;
    std::atomic<bool> ready=false;
    bool attempted=false;
    bool warningShown=false; // Accessed only by queued game-thread notifications.
    std::string startupFailure="Player/body correction could not initialize.";

    std::string ModuleAt(const void* address) {
        HMODULE module=nullptr;
        if(!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCSTR>(address),&module))return "unidentified hook/trampoline";
        char path[MAX_PATH]{};
        if(!GetModuleFileNameA(module,path,MAX_PATH))return "unidentified module";
        return path;
    }
    void NotifyUnavailableAfterLoad() {
        // No render-thread UI calls, polling, or per-frame log traffic. Startup
        // may fail at DataLoaded; present the result only after loading a game.
        SKSE::GetTaskInterface()->AddTask([] {
            if(warningShown || ready.load(std::memory_order_acquire))return;
            warningShown=true;
            const std::string message="OCU DAPA compatibility warning\n\n"+startupFailure+
                "\n\nPlayer/body/held-item correction is inactive; hands and weapons may ghost. "
                "World correction remains enabled.\n\n"
                "Check OpenCompositeInput.log in the Skyrim VR SKSE log folder for draw-hook details. "
                "Include that log and your renderer DLL when reporting this.";
            SKSE::log::error("DAPA compatibility notification: {}",startupFailure);
            RE::DebugMessageBox(message.c_str());
        });
    }
    ID3D11DeviceContext* immediate=nullptr;
    // Classification is scoped to the render pass, never cached by mesh/buffer size.
    thread_local bool owned=false;
    bool needsClear=true;
    uint64_t draws=0,classified=0;
    uint64_t higgsClassified=0;
    uint64_t spellWheelClassified=0;
    uint64_t vrArrowClassified=0;
    uint64_t crossbowClassified=0;
    uint64_t vrEquipmentClassified=0;
    uint64_t setups=0,callbacks=0,ownedCallbacks=0;
    // Only counters on draw rejection; no per-draw logging/timing/readback.
    std::array<uint64_t,8> rejected{};
    std::array<uint64_t,DapaEngineDraw::sites.size()> siteDraws{};
    std::chrono::steady_clock::time_point lastLog{};

    bool IsPlayerGeometry(const RE::NiAVObject* geometry) {
        auto* player=RE::PlayerCharacter::GetSingleton();
        if(!player || !geometry)return false;
        const auto* vr=player->GetVRNodeData();
        const DapaGeometryOwnership::Roots<RE::NiAVObject> roots{
            player->Get3D(true),player->Get3D(false),
            vr?vr->ArrowNode.get():nullptr,
            vr?vr->ArrowHoldNode.get():nullptr,
            vr?vr->ArrowSnapNode.get():nullptr,
            {vr?vr->LeftWeaponOffsetNode.get():nullptr,vr?vr->RightWeaponOffsetNode.get():nullptr,
             vr?vr->LeftCrossbowOffsetNode.get():nullptr,vr?vr->RightCrossbowOffsetNode.get():nullptr,
             vr?vr->LeftMeleeWeaponOffsetNode.get():nullptr,vr?vr->RightMeleeWeaponOffsetNode.get():nullptr,
             vr?vr->LeftStaffWeaponOffsetNode.get():nullptr,vr?vr->RightStaffWeaponOffsetNode.get():nullptr,
             vr?vr->LeftShieldOffsetNode.get():nullptr,vr?vr->RightShieldOffsetNode.get():nullptr,
             vr?vr->BowRotationNode.get():nullptr}};
        const auto held=DapaHiggs::roots.Read();
        // VR's arrow containers live below PlayerWorldNode, outside both body
        // skeletons. Read them afresh so ammo/model changes and detach on fire
        // cannot leave a cached mesh classified as hand-attached geometry.
        const RE::TESObjectREFR* reference=nullptr;
        const auto kind=DapaGeometryOwnership::Classify(geometry,roots,
            [&](const RE::NiAVObject* node){return DapaHiggs::roots.Matches(node,held);},reference);
        switch(kind) {
        case DapaGeometryOwnership::Kind::Player:return true;
        case DapaGeometryOwnership::Kind::Held:++higgsClassified;return true;
        case DapaGeometryOwnership::Kind::AttachedArrow:
            if(++vrArrowClassified==1)
                SKSE::log::info("DAPA VR ARROW MASK v1: first live held/nocked arrow geometry classified");
            return true;
        case DapaGeometryOwnership::Kind::AttachedEquipment:
            if(++vrEquipmentClassified==1)
                SKSE::log::info("DAPA VR EQUIPMENT MASK v1: first native controller-attached weapon/shield geometry classified");
            return true;
        case DapaGeometryOwnership::Kind::None:break;
        }
        if(DapaSpellWheel::Owns(reference)) {++spellWheelClassified;return true;}
        if(DapaCrossbow::Owns(reference)) {
            if(++crossbowClassified==1)
                SKSE::log::info("DAPA CROSSBOW MASK v1: first hand-positioned reload bolt geometry classified");
            return true;
        }
        return false;
    }
    template<int Type> void __fastcall Setup(void* shader,RE::BSRenderPass* pass,uint32_t flags) {
        owned=false;
        originalSetup[Type](shader,pass,flags);
        owned=pass && IsPlayerGeometry(pass->geometry);
        if(owned && ++classified==1)
            SKSE::log::info("DAPA BODY MASK v4: first player-owned geometry detected ({} shader)",Type==0?"lighting":"effect");
        if((++setups & 0x1ffff)==0 && g_diagnosticLogging.load(std::memory_order_relaxed)) {
            SKSE::log::debug("DAPA BODY MASK v4: setup={} owned={} callbacks={} ownedCallbacks={} maskDraws={} CSX(accepted={},player={}) rejects(context={},notReady={},resources={},noDSV={},wrongDepth={},format={},allocation={},ownerNotAccepted={})",
                setups,classified,callbacks,ownedCallbacks,draws,csxAccepted,csxPlayerAccepted,rejected[0],rejected[1],rejected[2],rejected[3],rejected[4],rejected[5],rejected[6],rejected[7]);
        }
    }
    template<int Type> void __fastcall Restore(void* shader,RE::BSRenderPass* pass,uint32_t flags) {
        owned=false;originalRestore[Type](shader,pass,flags);
    }
    bool Prepare(ID3D11DeviceContext* ctx,bool playerDraw,MaskAccess& access,ID3D11Texture2D* apiSceneDepth=nullptr) {
        if(!playerDraw)return false;
        ++ownedCallbacks;
        if(ctx!=immediate) {++rejected[0];return false;}
        if(!ready.load(std::memory_order_acquire) || !g_pBridge) {++rejected[1];return false;}
        auto resources=AcquirePublishedBridgeResources();
        if(!resources.depthTexture || !resources.d3dDevice) {++rejected[2];return false;}
        // v1 API must describe the same canonical packed-stereo scene consumed
        // by OCU's depth bridge; never silently treat a capture texture as scene.
        if(apiSceneDepth && apiSceneDepth!=resources.depthTexture) {++rejected[4];return false;}
        Microsoft::WRL::ComPtr<ID3D11DepthStencilView> dsv;
        ctx->OMGetRenderTargets(0,nullptr,&dsv);
        if(!dsv) {++rejected[3];return false;}
        Microsoft::WRL::ComPtr<ID3D11Resource> actual;
        dsv->GetResource(&actual);
        if(actual.Get()!=resources.depthTexture) {++rejected[4];return false;} // no shadow/reflection passes
        D3D11_TEXTURE2D_DESC desc{};resources.depthTexture->GetDesc(&desc);
        if(desc.SampleDesc.Count!=1 || desc.ArraySize!=1) {++rejected[5];return false;}
        // Reject non-scene/deferred draws before touching the mask gate: their
        // concurrent callbacks must not poison actual scene player coverage.
        if(!access.TryAcquire(g_pBridge,DapaMaskBridge::AccessMode::Write) || !access.Clean()) {++rejected[6];return false;}
        const auto* previous=gpu.Texture();
        const bool hadCapture=access.Valid();
        // The lease remains held through replay/publication. Neither a different
        // draw thread nor the compositor can replace, clear or copy this mask.
        if(!gpu.Size(resources.d3dDevice,desc.Width,desc.Height)) {access.Poison();++rejected[6];return false;}
        if(previous!=gpu.Texture() || needsClear || !hadCapture) {
            gpu.Clear(ctx);needsClear=false;
        }
        return true;
    }
    bool Publish(MaskAccess& access) {
        if(!access.Publish(reinterpret_cast<uint64_t>(gpu.Texture())))return false;
        ++draws;
        if(!g_diagnosticLogging.load(std::memory_order_relaxed))return true;
        const auto now=std::chrono::steady_clock::now();
        if(now-lastLog>std::chrono::seconds(5)) {
            SKSE::log::debug("DAPA BODY MASK v4: ownedPasses={} maskDraws={} HIGGS-ownedPasses={} SpellWheel-ownedPasses={} VR-arrow-ownedPasses={} crossbow-reload-ownedPasses={} VR-equipment-ownedPasses={} (live ownership, no IB guessing)",classified,draws,higgsClassified,spellWheelClassified,vrArrowClassified,crossbowClassified,vrEquipmentClassified);
            lastLog=now;
        }
        return true;
    }
    template<size_t Site> void STDMETHODCALLTYPE Draw(ID3D11DeviceContext* ctx,UINT n,UINT start,INT base) {
        ++callbacks;const bool playerDraw=owned;
        // Fetch CURRENT dispatch, never retain/overwrite D3D11's mutable slots.
        ctx->DrawIndexed(n,start,base);
        if(!playerDraw)return;
        MaskAccess access;
        if(Prepare(ctx,playerDraw,access)) {
            if(!gpu.Replay(ctx,[&]{ctx->DrawIndexed(n,start,base);})) {access.Poison();++rejected[6];return;}
            if(!Publish(access))return;
            if(++siteDraws[Site]==1)SKSE::log::info("DAPA BODY MASK v4: first mask at mesh draw RVA 0x{:X}",DapaEngineDraw::sites[Site].rva);
        }
    }
    template<size_t Site> void STDMETHODCALLTYPE Instanced(ID3D11DeviceContext* ctx,UINT n,UINT instances,UINT start,INT base,UINT first) {
        ++callbacks;const bool playerDraw=owned;
        DapaAcceptedDraw::Scope scope({ctx,n,instances,start,base,first},playerDraw);
        auto* previous=reinterpret_cast<InstancedFn>(engineHooks[Site].chained);
        if(previous)previous(ctx,n,instances,start,base,first); // CSX draw/suppression chain, exactly once
        else ctx->DrawIndexedInstanced(n,instances,start,base,first);
        // The CSX adapter observes accepted draws while their geometry state is
        // still bound. No acceptance means suppression/redirection: no mask.
        if(previous && playerDraw && !scope.consumed)++rejected[7];
        if(previous || !playerDraw)return;
        MaskAccess access;
        if(Prepare(ctx,playerDraw,access)) {
            if(!gpu.Replay(ctx,[&]{ctx->DrawIndexedInstanced(n,instances,start,base,first);})) {access.Poison();++rejected[6];return;}
            if(!Publish(access))return;
            if(++siteDraws[Site]==1)SKSE::log::info("DAPA BODY MASK v4: first mask at mesh draw RVA 0x{:X}",DapaEngineDraw::sites[Site].rva);
        }
    }
    void STDMETHODCALLTYPE CsxAccepted(ID3D11DeviceContext* ctx,UINT n,UINT instances,UINT start,INT base,UINT first) {
        // Claim before dispatch to prevent nested draws/replays inheriting it.
        ++csxAccepted;
        DapaAcceptedDraw::Dispatch({ctx,n,instances,start,base,first},
            [&]{ctx->DrawIndexedInstanced(n,instances,start,base,first);},
            [&]{
                ++csxPlayerAccepted;
                MaskAccess access;
                if(Prepare(ctx,true,access)) {
                    if(!gpu.Replay(ctx,[&]{ctx->DrawIndexedInstanced(n,instances,start,base,first);})) {access.Poison();++rejected[6];return;}
                    if(!Publish(access))return;
                    if(++siteDraws[0]==1)
                        SKSE::log::info("DAPA BODY MASK v4: first CSX-accepted player draw MASKED (primary mesh site, live geometry state)");
                }
            });
    }
    void ApiFault(void*) {
        startupFailure="The accepted-draw API consumer encountered an exception and stopped body-mask delivery.";
        ready.store(false,std::memory_order_release);
        SKSE::log::error("DAPA BODY MASK: {}; no hot switch to legacy hooks",startupFailure);
        NotifyUnavailableAfterLoad(); // Schedules UI on the game thread, never invokes it here.
    }
    void __cdecl ApiAccepted(const CSXAcceptedDrawAPI::Draw* event,void*) {
        if(!ready.load(std::memory_order_acquire))return;
        ++csxAccepted;
        if(!IsPlayerGeometry(static_cast<const RE::BSGeometry*>(event->geometry)))return;
        ++classified;
        MaskAccess access;
        if(!Prepare(event->context,true,access,event->sceneDepth))return;
        uint32_t replayResult=CSXAcceptedDrawAPI::Failed;
        const bool restored=gpu.Replay(event->context,[&] {
            replayResult=event->replay(event->replayToken);
        });
        if(!restored || replayResult!=CSXAcceptedDrawAPI::Success) {access.Poison();++rejected[6];return;}
        if(!Publish(access))return;
        if(++csxPlayerAccepted==1)
            SKSE::log::info("DAPA BODY MASK API v1: first player-owned accepted scene draw MASKED");
    }
    template<size_t Site> uintptr_t Callback() {
        if constexpr(DapaEngineDraw::sites[Site].instanced)return reinterpret_cast<uintptr_t>(&Instanced<Site>);
        else return reinterpret_cast<uintptr_t>(&Draw<Site>);
    }
    template<size_t... Site> auto Callbacks(std::index_sequence<Site...>) {
        return std::array<uintptr_t,sizeof...(Site)>{Callback<Site>()...};
    }
    bool InstallSlot(unsigned index,void** slot,void* target,void** original,const char* name) {
        const bool ok=hooks[index].Install(slot,target,original);
        if(ok)SKSE::log::info("DAPA BODY MASK v4: {} chained; slot={} previous={}",name,static_cast<void*>(slot),*original);
        else SKSE::log::error("DAPA BODY MASK v4: {} install failed; Win32={}",name,hooks[index].error);
        if(hooks[index].protectionError)
            SKSE::log::error("DAPA BODY MASK v4: {} page protection restore failed; Win32={}",name,hooks[index].protectionError);
        return ok && !hooks[index].protectionError;
    }
    template<int Type> bool HookGeometry(uintptr_t address) {
        auto** table=reinterpret_cast<void**>(address);
        return InstallSlot(Type*2,table+6,reinterpret_cast<void*>(&Setup<Type>),
                   reinterpret_cast<void**>(&originalSetup[Type]),Type==0?"lighting setup":"effect setup") &&
            InstallSlot(1+Type*2,table+7,reinterpret_cast<void*>(&Restore<Type>),
                   reinterpret_cast<void**>(&originalRestore[Type]),Type==0?"lighting restore":"effect restore");
    }
    void RollBack() {
        ready.store(false,std::memory_order_release);
        if(!csxApi.Stop())SKSE::log::error("DAPA BODY MASK: API unsubscribe failed; consumer inactive");
        if(!csxAdapter.Remove())
            SKSE::log::error("DAPA BODY MASK v4: CSX observer rollback failed; Win32={}",csxAdapter.error);
        for(auto& hook:engineHooks)if(!hook.Remove())
            SKSE::log::error("DAPA BODY MASK v4: engine hook rollback failed at 0x{:X}, Win32={}",hook.spec->rva,hook.error);
        for(int i=3;i>=0;--i) {
            const bool removed=hooks[i].Remove();
            if(!removed || hooks[i].protectionError)
                SKSE::log::error("DAPA BODY MASK v4: rollback slot {} failed; Win32={} protection={}",i,hooks[i].error,hooks[i].protectionError);
        }
        // Keep callback originals/context alive even if another mod already
        // chained through us. With ready=false the callbacks only pass through.
        SKSE::log::error("DAPA BODY MASK v4: startup incomplete; body correction inactive, world correction unchanged");
    }
}

void InstallSetupGeometryHook() {
    using namespace PlayerMask;
    if(attempted)return;
    attempted=true;
    if(REL::Module::get().version()!=SKSE::RUNTIME_VR_1_4_15) {
        SKSE::log::error("DAPA BODY MASK v4: this mesh-call map requires Skyrim VR 1.4.15; no hooks applied");return;
    }
    auto resources=AcquirePublishedBridgeResources();
    SKSE::log::info("DAPA BODY MASK v4: startup; bridge={} device={} rendererContext={}",
        static_cast<void*>(g_pBridge),static_cast<void*>(resources.d3dDevice),static_cast<void*>(resources.d3dContext));
    if(!g_pBridge || !resources.d3dDevice || !resources.d3dContext) {
        attempted=false; // Retry once scene resources are available at game load.
        SKSE::log::warn("DAPA BODY MASK v4: renderer resources not ready; will retry at game load");return;
    }
    // Use the exact renderer context rather than a possibly unwrapped context
    // returned by a device proxy. The draw filter must match Skyrim's caller.
    immediate=resources.d3dContext;immediate->AddRef();
    if(immediate->GetType()!=D3D11_DEVICE_CONTEXT_IMMEDIATE) {
        startupFailure="The renderer supplied a deferred context; DAPA requires Skyrim's immediate draw context.";
        SKSE::log::error("DAPA BODY MASK v4: renderer context is deferred; body correction inactive");return;
    }
    if(!gpu.Initialize(resources.d3dDevice)) {
        startupFailure="DAPA's GPU mask shader/state initialization failed.";
        SKSE::log::error("DAPA BODY MASK v4: GPU shader/state initialization failed; body correction inactive");return;
    }
    // Prefer a versioned, full-coverage provider. Never install the engine,
    // geometry or private CSX hooks when the API route owns mask delivery.
    const auto shaderModule=GetModuleHandleW(L"CommunityShaders.dll");
    const auto query=shaderModule ? reinterpret_cast<CSXAcceptedDrawAPI::QueryFn>(
        GetProcAddress(shaderModule,CSXAcceptedDrawAPI::ExportName)) : nullptr;
    const auto apiResult=csxApi.Connect(query,immediate,&ApiAccepted,nullptr,&ApiFault);
    if(apiResult==DapaCsxApi::ConnectResult::Connected) {
        ready.store(true,std::memory_order_release);
        SKSE::log::info("DAPA BODY MASK: accepted-draw API v1 registered; private CSX/engine/geometry patches bypassed; awaiting coverage verification");
        SKSE::log::info("DAPA VR ARROW MASK v1 / VR EQUIPMENT MASK v1: native arrow and controller equipment ancestry enabled");
        return;
    }
    if(apiResult!=DapaCsxApi::ConnectResult::Missing)
        SKSE::log::warn("DAPA BODY MASK: proposed accepted-draw API unavailable/incompatible (result={}); trying verified legacy adapter",static_cast<int>(apiResult));
    const auto imageBase=REL::Module::get().base();
    const auto callbackAddresses=Callbacks(std::make_index_sequence<DapaEngineDraw::sites.size()>{});
    // Validate every site BEFORE changing any executable instruction.
    for(size_t i=0;i<engineHooks.size();++i) {
        const auto& site=DapaEngineDraw::sites[i];
        if(!engineHooks[i].Prepare(site,reinterpret_cast<uint8_t*>(imageBase+site.rva))) {
            startupFailure="An engine draw-call patch is incompatible with DAPA. The renderer/mod responsible is not yet identified.";
            SKSE::log::error("DAPA BODY MASK v4: mesh call signature/chain rejected at RVA 0x{:X}, Win32={}; no engine patches applied",site.rva,engineHooks[i].error);return;
        }
    }
    // Inspect the actual owner. ENB/proxy hooks must not be mistaken for CSX
    // merely because CommunityShaders.dll is also present in the process.
    for(size_t i=0;i<engineHooks.size();++i)if(engineHooks[i].chained) {
        auto* module=GetModuleHandleW(L"CommunityShaders.dll");
        if(i!=0 || !module || !csxAdapter.Prepare(reinterpret_cast<uintptr_t>(module),
               engineHooks[i].chained,reinterpret_cast<uintptr_t>(&CsxAccepted))) {
            const auto ownerAddress=reinterpret_cast<uintptr_t>(engineHooks[i].chained);
            const auto moduleBase=reinterpret_cast<uintptr_t>(module);
            HMODULE actualModule=nullptr;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(ownerAddress),&actualModule);
            const bool isCsx=module && actualModule==module;
            startupFailure=isCsx ? "Community Shaders/Open Shaders draw-call integration could not be validated or installed for this build." :
                "Another renderer/mod owns a draw call that DAPA cannot safely observe. This is not automatically a Community Shaders failure.";
            SKSE::log::error("DAPA BODY MASK compatibility: meshRva=0x{:X}, ownerModule={}, owner=0x{:X}, CommunityShadersOwnsCall={}, relativeToCsx=0x{:X}, validatedContracts={}, adapterError={}. Player/body/held-item correction INACTIVE; world correction unchanged. Report this log and the DLL identified as owner; no unknown owner was bypassed.",
                engineHooks[i].spec->rva,ModuleAt(engineHooks[i].chained),ownerAddress,isCsx,isCsx?ownerAddress-moduleBase:0,DapaCsxDraw::builds.size(),csxAdapter.error);
            return;
        }
        csxPrepared=true;
        SKSE::log::info("DAPA BODY MASK v4: validated {} accepted-draw adapter; codeRebased={}, actualOwnerRva=0x{:X}",csxAdapter.build->name,csxAdapter.relocated,csxAdapter.build->ownerRva);
    }
    SKSE::AllocTrampoline(2048);
    for(size_t i=0;i<engineHooks.size();++i) {
        auto* stub=static_cast<uint8_t*>(SKSE::GetTrampoline().allocate(48));
        if(!engineHooks[i].Build(stub,callbackAddresses[i])) {
            SKSE::log::error("DAPA BODY MASK v4: trampoline failed at RVA 0x{:X}, Win32={}; no engine patches applied",engineHooks[i].spec->rva,engineHooks[i].error);return;
        }
    }
    // CommonLibVR BSShader declares SetupGeometry / RestoreGeometry at 6 / 7.
    if(!HookGeometry<0>(REL::VariantID(305261,255053,0x19050d0).address()) ||
       !HookGeometry<1>(REL::VariantID(305447,255194,0x1905f58).address())) {
        RollBack();return;
    }
    if(csxPrepared && !csxAdapter.Install()) {
        SKSE::log::error("DAPA BODY MASK v4: accepted-draw observer install failed, Win32={}",csxAdapter.error);
        RollBack();return;
    }
    for(auto& hook:engineHooks) {
        if(!hook.Install()) {
            SKSE::log::error("DAPA BODY MASK v4: engine call install failed at RVA 0x{:X}, Win32={}",hook.spec->rva,hook.error);RollBack();return;
        }
        SKSE::log::info("DAPA BODY MASK v4: stable mesh draw RVA 0x{:X}, existingOwner={}",hook.spec->rva,hook.chained);
    }
    ready.store(true,std::memory_order_release);
    SKSE::log::info("DAPA BODY MASK v4: 17 mesh sites + 4 geometry hooks + {} CSX accepted-draw observers installed; mask=depth-tested scene-sampled; no D3D11 vtable writes; awaiting coverage verification",csxPrepared?2:0);
    SKSE::log::info("DAPA VR ARROW MASK v1: live ArrowNode/ArrowHoldNode/ArrowSnapNode ancestry enabled; fired/world projectiles retain world correction");
    SKSE::log::info("DAPA VR EQUIPMENT MASK v1: native left/right weapon, crossbow, melee, staff, shield and bow rotation roots enabled");
}
