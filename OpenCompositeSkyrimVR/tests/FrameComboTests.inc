// Production combo parsing and timing, with only the clock and key delivery mocked.
devices[0].profile.Configure("frame"); devices[1].profile.Configure("frame");
devicePresent[0]=devicePresent[1]=true;
struct ComboCase { const char* token; int hand; int bit; };
const ComboCase frameCombos[] = {
    {"frame_dpad_left",0,5}, {"frame_dpad_right",0,6},
    {"frame_x",1,5}, {"frame_y",1,6},
    {"frame_l_bumper",0,3}, {"frame_r_bumper",1,3},
    {"frame_view",0,35}, {"frame_menu",1,35},
    {"a",1,7}, {"b",1,1}, {"x",0,7}, {"y",0,1},
    {"left_stick",0,32}, {"right_stick",1,32},
    {"left_grip",0,2}, {"right_grip",1,2},
    {"left_trigger",0,33}, {"right_trigger",1,33}
};
auto downs=[] { return std::count_if(comboKeys.begin(),comboKeys.end(),[](auto k){return !k.second;}); };
for (const auto& c:frameCombos) {
    ComboBinding::BtnReq req{};
    const bool extra = std::string(c.token).find("frame_") == 0;
    check(ParseComboButton(c.token,req) && req.ctrl==c.hand && req.mask==(uint64_t{1}<<c.bit) && req.frameOnly==extra,"Frame shortcut token maps to its physical hand and ID");
    for (std::string mode:{"press","hold","long_press","double_tap","triple_tap","quadruple_tap"}) {
        ComboBinding combo; combo.buttons={req}; combo.mode=mode; combo.scancode=37; combo.timingMs=500;
        s_combos={combo}; comboKeys.clear(); comboNow=1000;
        VRControllerState_t state[2]{}; bool valid[2]={true,true};
        auto poll=[&](bool held,unsigned advance=0) {
            comboNow+=advance; state[c.hand].ulButtonPressed=held?req.mask:0;
            ProcessCombos(nullptr,state,valid);
        };
        poll(true);
        if (mode=="press" || mode=="hold") {
            check(downs()==1,"Frame shortcut press/hold delivers key down");
            poll(true,100); check(downs()==1,"Held shortcut does not repeat key down");
            poll(false,100);
        } else if (mode=="long_press") {
            poll(true,400); check(downs()==0,"Frame long press waits for its threshold");
            poll(true,150); check(downs()==1,"Frame long press fires at threshold");
            poll(false,100);
        } else {
            int taps=mode=="double_tap"?2:mode=="triple_tap"?3:4;
            check(downs()==0,"Frame multi-tap does not fire on first press");
            for(int tap=1;tap<taps;++tap) { poll(false,70); poll(true,70); }
            check(downs()==1,"Frame multi-tap delivers one key down");
            poll(false,100);
        }
        check(comboKeys.size()==2 && comboKeys[0]==std::make_pair(37,false) && comboKeys[1]==std::make_pair(37,true),"Every Frame shortcut mode balances key down/up");
    }
    ComboBinding combo; combo.buttons={req}; combo.mode="hold"; combo.scancode=37;
    s_combos={combo}; comboKeys.clear();
    VRControllerState_t state[2]{}; bool valid[2]={true,true};
    state[1-c.hand].ulButtonPressed=req.mask;
    ProcessCombos(nullptr,state,valid); check(comboKeys.empty(),"Frame shortcut ignores the same ID on the wrong hand");
    state[c.hand].ulButtonPressed=req.mask;
    if (extra) {
        devices[c.hand].profile.Configure("index");
        ProcessCombos(nullptr,state,valid); check(comboKeys.empty(),"Frame-only shortcut ignores Index controls reusing its ID");
    }
    devices[c.hand].profile.Configure("frame");
    ProcessCombos(nullptr,state,valid); check(downs()==1,"Frame profile allows its own shortcut");
    valid[c.hand]=false;
    ProcessCombos(nullptr,state,valid); check(comboKeys.size()==2 && comboKeys.back().second,"Disconnect releases a held shortcut key");
}
ComboBinding chord; chord.mode="press"; chord.scancode=37;
ComboBinding::BtnReq left{},right{},unknown{};
check(ParseComboButton("frame_l_bumper",left) && ParseComboButton("frame_y",right),"Cross-hand Frame chord parses");
check(!ParseComboButton("frame_not_a_button",unknown),"Unknown Frame shortcut rejected");
chord.buttons={left,right}; s_combos={chord};comboKeys.clear();
VRControllerState_t chordState[2]{}; bool chordValid[2]={true,true};
chordState[0].ulButtonPressed=left.mask;
ProcessCombos(nullptr,chordState,chordValid);check(comboKeys.empty(),"Frame chord requires both hands");
chordState[1].ulButtonPressed=right.mask;
ProcessCombos(nullptr,chordState,chordValid);check(downs()==1,"Cross-hand Frame chord fires once");
