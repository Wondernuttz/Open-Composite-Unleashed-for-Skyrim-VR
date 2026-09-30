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
    check(bound(ctrl.grip,"squeeze/value"), "Frame grip axis keeps analog squeeze");
    check(bound(ctrl.gripClick,"squeeze/value"), "Frame gameplay grip uses runtime-thresholded squeeze, not the end-stop click");
    check(!bound(ctrl.gripClick,"squeeze/click"), "Frame gameplay grip must not require the physical click");
    check(bound(ctrl.triggerClick,"trigger/click"), "Frame physical trigger click");
    check(bound(ctrl.stickX,"thumbstick/x") && bound(ctrl.stickY,"thumbstick/y"), "Frame stick axes match Valve paths");
    check(bound(ctrl.stickBtn,"thumbstick/click") && bound(ctrl.stickBtnTouch,"thumbstick/touch"), "Frame stick click/touch paths");
    check(bound(ctrl.trigger,"trigger/value") && bound(ctrl.triggerTouch,"trigger/touch"), "Frame trigger value/touch paths");
    check(bound(ctrl.btnATouch,hand==0?"dpad_down/touch":"a/touch"), "Frame primary touch path");
    check(bound(ctrl.menuTouch,hand==0?"dpad_up/touch":"b/touch"), "Frame secondary touch path");
    for (int i=0;i<4;++i) {
        const std::string left[]={"dpad_left/touch","dpad_right/touch","bumper/touch","view/touch"};
        const std::string right[]={"x/touch","y/touch","bumper/touch","menu/touch"};
        check(bound(ctrl.frameExtraTouch[i],hand==0?left[i]:right[i]),"Frame extra touch path");
    }
    // Explicit expected OpenVR IDs independently match the Configurator audit.
    // Test all nine controls per hand, including pairs, without inventing
    // hardware thresholds: the mock supplies OpenXR's boolean action states.
    struct ButtonCase { XrAction press, touch; uint64_t mask; };
    const ButtonCase allButtons[] = {
        {ctrl.stickBtn,ctrl.stickBtnTouch,uint64_t{1}<<32},
        {ctrl.btnA,ctrl.btnATouch,uint64_t{1}<<7},
        {ctrl.menu,ctrl.menuTouch,uint64_t{1}<<1},
        {ctrl.triggerClick,ctrl.triggerTouch,uint64_t{1}<<33},
        {ctrl.gripClick,ctrl.gripTouch,(uint64_t{1}<<2)|(uint64_t{1}<<34)},
        {ctrl.frameExtra[0],ctrl.frameExtraTouch[0],uint64_t{1}<<5},
        {ctrl.frameExtra[1],ctrl.frameExtraTouch[1],uint64_t{1}<<6},
        {ctrl.frameExtra[2],ctrl.frameExtraTouch[2],uint64_t{1}<<3},
        {ctrl.frameExtra[3],ctrl.frameExtraTouch[3],uint64_t{1}<<35}
    };
    auto resetSamples=[&] { for (auto& [action,s]:samples) { s.boolean=false; s.scalar=0; s.active=true; } };
    for (const auto& button:allButtons) {
        resetSamples();
        samples.at(button.press).boolean=true;
        samples.at(button.touch).boolean=true;
        VRControllerState_t state{},other{};
        for (int poll=0;poll<120;++poll) {
            input.GetLegacyControllerState(hand+1,&state);
            check(state.ulButtonPressed==button.mask && state.ulButtonTouched==button.mask,"Every Frame button preserves its own press/touch during a hold");
        }
        input.GetLegacyControllerState(2-hand,&other);
        check(other.ulButtonPressed==0 && other.ulButtonTouched==0,"Every Frame button stays on its physical hand");
        samples.at(button.press).boolean=false;
        input.GetLegacyControllerState(hand+1,&state);
        check(state.ulButtonPressed==0 && state.ulButtonTouched==button.mask,"Every Frame button releases independently of touch");
        // OpenXR requires inactive action state to be false (not stale true).
        samples.at(button.press).boolean=false;
        samples.at(button.touch).boolean=false;
        samples.at(button.press).active=false;
        samples.at(button.touch).active=false;
        input.GetLegacyControllerState(hand+1,&state);
        check(state.ulButtonPressed==0 && state.ulButtonTouched==0,"Inactive Frame controls cannot stay pressed or touched");
    }
    for (size_t a=0;a<std::size(allButtons);++a)
    for (size_t b=a+1;b<std::size(allButtons);++b) {
        resetSamples();
        samples.at(allButtons[a].press).boolean=true;
        samples.at(allButtons[b].press).boolean=true;
        VRControllerState_t state{};
        input.GetLegacyControllerState(hand+1,&state);
        check(state.ulButtonPressed==(allButtons[a].mask|allButtons[b].mask),"Frame two-button combinations do not collide");
    }
    resetSamples();
    samples.at(ctrl.stickX).scalar=.7f; samples.at(ctrl.stickY).scalar=-.8f;
    samples.at(ctrl.trigger).scalar=.4f; samples.at(ctrl.grip).scalar=.3f;
    VRControllerState_t axes{};
    input.GetLegacyControllerState(hand+1,&axes);
    check(std::abs(axes.rAxis[0].x-.7f)<.001f && std::abs(axes.rAxis[0].y+.8f)<.001f,"Frame analog stick retains both signed axes");
    check(std::abs(axes.rAxis[1].x-.4f)<.001f && std::abs(axes.rAxis[2].x-.3f)<.001f,"Frame trigger and squeeze axes remain independent");
    for (int i=0;i<4;++i) {
        for (auto& [action,s]:samples) { s.boolean=false; s.scalar=0; s.active=true; }
        samples.at(ctrl.frameExtra[i]).boolean=true;
        samples.at(ctrl.frameExtraTouch[i]).boolean=true;
        VRControllerState_t frame{}, other{};
        check(input.GetLegacyControllerState(hand+1,&frame),"Frame input available");
        const auto mask=uint64_t{1}<<LegacyControllerActions::FrameButtonIds[i];
        check(frame.ulButtonPressed==mask && frame.ulButtonTouched==mask,"Frame extra emits exactly its own press/touch bits");
        for (int poll=0;poll<120;++poll) {
            input.GetLegacyControllerState(hand+1,&frame);
            check(frame.ulButtonPressed==mask,"Frame extra stays down across polls for Skyrim hold actions");
        }
        samples.at(ctrl.frameExtra[i]).boolean=false;
        input.GetLegacyControllerState(hand+1,&frame);
        check(frame.ulButtonPressed==0 && frame.ulButtonTouched==mask,"Frame extra release does not require finger lift");
        samples.at(ctrl.frameExtra[i]).boolean=true;
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
    const auto gripMask=ButtonMaskFromId(k_EButton_Grip)|ButtonMaskFromId(k_EButton_Axis2);
    check(gripState.ulButtonPressed==gripMask,"Frame digital grip reaches both legacy grip consumers");
    check((gripState.ulButtonPressed & (uint64_t{1}<<LegacyControllerActions::FrameButtonIds[2]))==0,"grip must not activate bumper");
    check(gripState.ulButtonTouched==(ButtonMaskFromId(k_EButton_Grip)|ButtonMaskFromId(k_EButton_Axis2)),"Frame grip touch does not need Index dropout filtering");
    // Mock OpenXR supplies the converted boolean; actual device thresholds are
    // runtime-owned. Exercise downstream hold/release without inventing them.
    samples.at(ctrl.grip).scalar=0.65f;
    for (int poll=0;poll<10;++poll) {
        input.GetLegacyControllerState(hand+1,&gripState);
        check(gripState.ulButtonPressed==gripMask,"Frame grip stays held across repeated polls");
        check(std::abs(gripState.rAxis[2].x-0.65f)<0.001f,"Frame retains partial analog squeeze");
    }
    samples.at(ctrl.gripClick).boolean=false;
    samples.at(ctrl.grip).scalar=0;
    input.GetLegacyControllerState(hand+1,&gripState);
    check(gripState.ulButtonPressed==0,"Frame release clears both legacy grip bits");
    check(gripState.ulButtonTouched==gripMask,"Frame release leaves independent finger touch intact");
}
xr_valveFrameController=false;
BaseInput unsupported;
unsupported.CreateLegacyActions();
for (const auto& ctrl:unsupported.legacyControllers)
    for (auto action:ctrl.frameExtra) check(action==XR_NULL_HANDLE,"runtime without Frame extension creates no Frame actions");
