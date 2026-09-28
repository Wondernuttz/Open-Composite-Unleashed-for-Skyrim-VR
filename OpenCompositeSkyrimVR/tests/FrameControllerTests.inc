// Exercises production Frame suggestions and state export, including mixed controllers.
oovr_global_configuration = {};
for (int hand=0; hand<2; ++hand) {
    devices[hand].profile.Configure("frame");
    devices[1-hand].profile.Configure("touch");
    const auto& ctrl=input.legacyControllers[hand];
    std::vector<XrActionSuggestedBinding> bindings;
    devices[hand].profile.AddLegacyBindings(ctrl, bindings);
    auto bound=[&](XrAction action, std::string suffix) {
        return std::any_of(bindings.begin(), bindings.end(), [&](const auto& b) {
            return b.action==action && b.binding==xrPaths.at(ctrl.handPath+"/input/"+suffix);
        });
    };
    check(bound(ctrl.btnA, hand==0?"dpad_down/click":"a/click"), "Frame primary binding");
    check(bound(ctrl.menu, hand==0?"dpad_up/click":"b/click"), "Frame secondary binding");
    check(bound(ctrl.frameExtra[0], hand==0?"dpad_left/click":"x/click"), "Frame independent left/X");
    check(bound(ctrl.frameExtra[1], hand==0?"dpad_right/click":"y/click"), "Frame independent right/Y");
    check(bound(ctrl.frameExtra[2], "bumper/click"), "Frame bumper distinct from trigger and grip");
    check(bound(ctrl.frameExtra[3], hand==0?"view/click":"menu/click"), "Frame View/Menu are not system buttons");
    check(bound(ctrl.gripTouch,"squeeze/touch"), "Frame actual capacitive grip path");
    check(bound(ctrl.triggerClick,"trigger/click"), "Frame physical trigger click");
    for (int i=0;i<4;++i) {
        for (auto& [action,s]:samples) { s.boolean=false; s.scalar=0; s.active=true; }
        samples.at(ctrl.frameExtra[i]).boolean=true;
        samples.at(ctrl.frameExtraTouch[i]).boolean=true;
        VRControllerState_t frame{}, other{};
        check(input.GetLegacyControllerState(hand+1,&frame),"Frame input available");
        const auto mask=uint64_t{1}<<LegacyControllerActions::FrameButtonIds[i];
        check(frame.ulButtonPressed==mask && frame.ulButtonTouched==mask,"Frame extra emits exactly its own press/touch bits");
        check(input.GetLegacyControllerState(2-hand,&other) && other.ulButtonPressed==0,"Frame input cannot leak to other controller");
        devices[hand].profile.Configure("touch");
        VRControllerState_t fallback{};
        check(input.GetLegacyControllerState(hand+1,&fallback) && fallback.ulButtonPressed==0,"stale Frame actions ignored after profile switch");
        devices[hand].profile.Configure("frame");
    }
    for (auto& [action,s]:samples) { s.boolean=false; s.scalar=0; s.active=true; }
    samples.at(ctrl.gripClick).boolean=true;
    samples.at(ctrl.gripTouch).boolean=true;
    VRControllerState_t gripState{};
    input.GetLegacyControllerState(hand+1,&gripState);
    check((gripState.ulButtonPressed & (uint64_t{1}<<LegacyControllerActions::FrameButtonIds[2]))==0,"grip must not activate bumper");
    check(gripState.ulButtonTouched==(ButtonMaskFromId(k_EButton_Grip)|ButtonMaskFromId(k_EButton_Axis2)),"Frame grip touch does not need Index dropout filtering");
}
xr_valveFrameController=false;
BaseInput unsupported;
unsupported.CreateLegacyActions();
for (const auto& ctrl:unsupported.legacyControllers)
    for (auto action:ctrl.frameExtra) check(action==XR_NULL_HANDLE,"runtime without Frame extension creates no Frame actions");
