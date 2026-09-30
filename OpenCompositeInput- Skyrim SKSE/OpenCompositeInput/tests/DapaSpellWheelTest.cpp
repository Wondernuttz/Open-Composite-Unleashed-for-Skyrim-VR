#include "../src/DapaSpellWheelForms.h"
#include <cstdlib>
#include <iostream>
static void Check(bool ok,const char* why) {
    if(!ok){std::cerr<<"FAIL: "<<why<<'\n';std::exit(1);}
}
int main() {
    using namespace DapaSpellWheel;
    Check(localIds.size()==247,"catalog count");
    Check(std::is_sorted(localIds.begin(),localIds.end()),"catalog sorted");
    Check(std::adjacent_find(localIds.begin(),localIds.end())==localIds.end(),"catalog unique");
    Check(IsUiLocalId(0x809) && IsUiLocalId(0x801),"both center orbs");
    Check(IsUiLocalId(0x14be) && IsUiLocalId(0x66ee),"wrist bars");
    ResolvedForms absent;absent.Seal();
    Check(!absent.Matches(0x12300809,true),"absent mod does not match");
    for(auto prefix:{0x01000000u,0x7A000000u,0xFD000000u}) {
        ResolvedForms full;
        for(auto id:localIds)full.Add(prefix|id);
        full.Seal();
        for(auto id:localIds) {
            Check(full.Matches(prefix|id,true),"resolved projectile");
            Check(!full.Matches(prefix|id,false),"non-projectile rejection");
            Check(!full.Matches((prefix^0x01000000u)|id,true),"same local ID from another mod");
        }
        Check(!full.Matches(prefix|0xFFFFFFu,true),"unknown projectile in same mod");
    }
    // Runtime receives full IDs from CommonLib, not hand-assembled load bytes.
    ResolvedForms remapped;remapped.Add(0xFE123801);remapped.Seal();
    Check(remapped.Matches(0xFE123801,true) && !remapped.Matches(0xFE124801,true),"resolved light-plugin identity");
    Check(!remapped.Matches(0,true),"null ID");
    std::cout<<"PASS: 247 unique UI records; load-order and resolved light-ID isolation; absent mod, non-projectile and unrelated-form rejection\n";
}
