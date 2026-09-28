#include "OpenOVR/Misc/CableTracking.h"
#include <cstdio>
#include <limits>
#include <stdexcept>
static int checks;
static void Check(bool value,const char* why) { ++checks; if(!value)throw std::runtime_error(why); }
static void Yaw(ocu_cable::Counter& c,double angle,double t) { c.Sample(0,std::sin(angle/2),0,std::cos(angle/2),t,true); }
int main() {
    try {
        constexpr double pi=3.14159265358979323846;
        ocu_cable::Counter c;
        for(int i=0;i<=720;++i) Yaw(c,i*pi/180,i/90.);
        Check(std::abs(c.Turns()-2)<1e-9,"two left rotations unwrap across +/-pi");
        Check(!c.Uncertain(),"normal rotation retains confidence");
        for(int i=719;i>=0;--i) Yaw(c,i*pi/180,(1440-i)/90.);
        Check(std::abs(c.Turns())<1e-9,"unwinding returns to zero");
        for(int i=1;i<=360;++i) Yaw(c,-i*pi/180,(1440+i)/90.);
        Check(std::abs(c.Turns()+1)<1e-9,"one right rotation has negative sign");
        c.Rebase(); Yaw(c,1.,21.);
        Check(std::abs(c.Turns()+1)<1e-9&&!c.Uncertain(),"known recenter preserves count without false turn");
        c.Sample(0,0,0,1,21.1,false); Yaw(c,-2.,21.2);
        Check(std::abs(c.Turns()+1)<1e-9&&c.Uncertain(),"tracking loss ignores heading jump and marks uncertainty");
        c.Reset(); Check(c.Turns()==0&&!c.Uncertain(),"deliberate zero clears total and uncertainty");
        Yaw(c,2.,30.);
        Check(c.Turns()==0&&c.Uncertain(),"long render/loading gap never invents turns");
        c.Reset(); Yaw(c,2.,30.); Yaw(c,-2.,30.001);
        Check(c.Turns()==0&&c.Uncertain(),"impossible fast pose jump rejected");
        c.Reset(); c.Sample(.70710678118,0,0,.70710678118,30.1,true);
        Check(!c.Tracked()&&c.Uncertain(),"vertical gaze does not yield arbitrary heading");
        c.Reset(); c.Sample(0,std::numeric_limits<double>::quiet_NaN(),0,1,30.2,true);
        Check(!c.Tracked(),"nonfinite quaternion rejected");
        Yaw(c,0,31); Yaw(c,.1,31.01);const auto before=c.Turns();Yaw(c,.2,31.01);
        Check(c.Turns()==before,"same predicted timestamp cannot double count");
        ocu_cable::requests=0;
        Check(!ocu_cable::Shortcut(0x3b,false),"ordinary key still delivered to Skyrim");
        Check(ocu_cable::Shortcut(ocu_cable::ShowAction,false),"show controller action intercepted");
        Check(ocu_cable::Shortcut(ocu_cable::ResetAction,true),"internal key-up never reaches game");
        Check(ocu_cable::requests.exchange(0)==1,"key-up cannot reset");
        ocu_cable::Shortcut(ocu_cable::ResetAction,false);
        Check(ocu_cable::requests.exchange(0)==2,"reset controller action delivered once");
        printf("Cable tracking: %d checks passed\n",checks);return 0;
    } catch(const std::exception& e) { fprintf(stderr,"FAIL: %s\n",e.what());return 1; }
}
