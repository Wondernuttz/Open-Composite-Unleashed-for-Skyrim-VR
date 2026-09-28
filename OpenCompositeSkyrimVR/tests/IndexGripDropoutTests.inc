template<class Check>
void RunGripDropoutTests(BaseInput& input, Check check)
{
    const auto grip=ButtonMaskFromId(k_EButton_Grip);
    const auto axis=ButtonMaskFromId(k_EButton_Axis2);
    autoAdvanceTime=false;
    // Exercise real production translation, not just the hold helper: both
    // threshold backends, runtime default, both hands and input-smoothing states.
    for(bool custom:{false,true}) for(bool valve:{false,true})
    for(bool smoothing:{false,true}) for(int hand=0;hand<2;++hand) {
        oovr_global_configuration={};
        oovr_global_configuration.customGrip=custom;
        oovr_global_configuration.releaseThreshold=.01f;
        oovr_global_configuration.smoothing=smoothing;
        input.indexGripRuntimeThresholds=valve;
        for(auto& hold:input.indexGripHolds)hold.Reset();
        for(auto& device:devices)device.profile.Configure("index");
        for(auto& entry:samples){entry.second.active=true;entry.second.boolean=false;entry.second.scalar=0;}
        focused=true;
        smoothInput=SmoothInput(3);
        VRControllerState_t state{}, other{};
        const auto& ctrl=input.legacyControllers[hand];
        auto poll=[&](uint64_t time,bool active,float value,bool digital) {
            mockNowMs=time;
            samples.at(ctrl.grip).active=active;
            samples.at(ctrl.grip).scalar=value;
            samples.at(ctrl.gripTouch).active=active;
            samples.at(ctrl.gripTouch).boolean=digital;
            check(input.GetLegacyControllerState(hand+1,&state),"dropout fixture controller read succeeds");
            check(!(state.ulButtonPressed&grip),"touch filtering never generates squeeze press");
            check(bool(state.ulButtonTouched&grip)==bool(state.ulButtonTouched&axis),"Grip/Axis2 touch stay coherent");
            return bool(state.ulButtonTouched&grip);
        };
        check(!poll(1000,false,0,false),"inactive startup cannot create a grab");
        check(poll(1001,true,.8f,true),"fresh grab has no added delay");
        for(unsigned i=0;i<100;++i)check(poll(1002,true,.2f,true),"stable held value survives repeated reads");
        check(poll(1010,false,0,false),"one inactive report is bridged");
        for(unsigned i=0;i<50;++i)check(poll(1010,false,0,false),"poll count cannot consume grace");
        // With custom thresholds the runtime boolean can still be false after
        // interruption; the recovered analogue value must retain local hysteresis.
        check(poll(1024,true,.2f,!custom),"one 72Hz dropout recovers without crossing grab again");
        input.GetLegacyControllerState(2-hand,&other);
        check(!(other.ulButtonTouched&grip),"one hand's grace does not leak to the other");
        check(poll(1030,true,0,false),"one zero sample is bridged");
        check(poll(1044,true,.2f,!custom),"one zero recovers at relaxed hold value");
        check(poll(1050,false,0,false),"sustained dropout begins confirmation");
        check(poll(1069,false,0,false),"nineteen milliseconds retains hold");
        check(!poll(1070,false,0,false),"twenty milliseconds without input releases");
        check(!poll(1071,true,.2f,false),"expired hold requires new grab");
        check(poll(1072,true,.8f,true),"intentional regrab works");
        check(poll(1080,true,0,false),"opening hand begins confirmation");
        check(!poll(1100,true,0,false),"opening hand releases at deadline");
        check(poll(1101,true,.8f,true),"grab before focus loss");
        focused=false;
        check(!poll(1102,true,.8f,true),"focus loss immediately releases even with active sample");
        focused=true;
        check(!poll(1103,true,.2f,false),"focus return does not reuse old hold");
        check(poll(1104,true,.8f,true),"grab before gap");
        check(!poll(1305,true,.2f,false),"long polling gap cannot preserve relaxed hold");
        check(poll(1306,true,.8f,true),"grab before missing device");
        devicePresent[hand]=false;
        check(!input.GetLegacyControllerState(hand+1,&state)&&!(state.ulButtonTouched&grip),"missing physical device immediately clears state");
        devicePresent[hand]=true;
        check(!poll(1307,true,.2f,false),"reconnect cannot reuse hold");
        check(poll(1308,true,.8f,true),"grab before profile switch");
        devices[hand].profile.Configure("touch");
        check(!poll(1309,true,.8f,true),"non-Index profile cannot retain grip touch");
        devices[hand].profile.Configure("index");
        check(!poll(1310,true,.2f,false),"profile return requires fresh grab");
        check(poll(1311,true,.8f,true),"grab before explicit sync/session reset");
        input.indexGripHolds[hand].Reset();
        check(!poll(1312,true,.2f,false),"hard sync/session reset cannot inherit relaxed hold");
        check(poll(1313,true,.8f,true),"grab before repeat-glitch test");
        for(unsigned i=0;i<10;++i) {
            check(poll(1320+i*15,false,0,false),"brief repeated glitch begins independent grace");
            check(poll(1327+i*15,true,.2f,!custom),"valid recovery resets grace");
        }
        if(custom) {
            check(poll(1480,true,std::numeric_limits<float>::quiet_NaN(),false),"single invalid float is bridged");
            check(poll(1494,true,.2f,false),"finite recovery keeps hold");
            check(poll(1500,true,std::numeric_limits<float>::infinity(),false),"invalid float starts bounded confirmation");
            check(!poll(1520,true,std::numeric_limits<float>::infinity(),false),"persistent invalid float releases");
        }
        check(poll(2000,true,.8f,true),"grab before late recovery");
        check(poll(2010,false,0,false),"late recovery begins with one missing sample");
        check(!poll(2030,true,.2f,false),"recovery at expired deadline cannot revive relaxed hold");
    }
    // A reset between XR query and delivery invalidates that in-flight sample.
    ocu_index_grip::HoldState hold;
    auto oldGeneration=hold.Generation();
    hold.Reset();
    check(!hold.Update(true,true,.8f,true,true,{},100,oldGeneration).held,"in-flight pre-reset sample cannot resurrect hold");
    auto current=hold.Generation();
    check(hold.Update(true,true,.8f,true,true,{},101,current).held,"new generation can grab");
    check(!hold.Update(true,true,0,false,true,{},102,oldGeneration).held,"old generation is rejected");
    check(hold.Update(true,true,.3f,false,true,{},103,current).held,"rejected old sample leaves newer hold intact");
    check(hold.Update(true,true,0,false,true,{},90,current).held,"older concurrent read cannot clear a newer hold");
    check(hold.Update(true,true,.3f,false,true,{},104,current).held,"rejected older read cannot start release confirmation");
    autoAdvanceTime=true;
    focused=true;
    for(auto& state:input.indexGripHolds)state.Reset();
}
