#pragma once
#include <cstdint>

namespace DapaAcceptedDraw {
struct Arguments {
    void* context;
    uint32_t indices,instances,start;
    int32_t base;
    uint32_t first;
    bool operator==(const Arguments&) const = default;
};

// Only an accepted draw inside the matching engine call may claim its ownership.
// A nested non-player call shadows the outer player, and a replay cannot re-claim it.
struct Scope {
    inline static thread_local Scope* current=nullptr;
    Arguments arguments;
    bool player,consumed=false;
    Scope* previous;
    Scope(Arguments args,bool owned):arguments(args),player(owned),previous(current) {current=this;}
    ~Scope() {current=previous;}
    Scope(const Scope&)=delete;
    Scope& operator=(const Scope&)=delete;
    static bool Claim(const Arguments& args) {
        auto* scope=current;
        if(!scope || scope->consumed || scope->arguments!=args)return false;
        scope->consumed=true;
        return scope->player;
    }
};
template<class Draw,class Mask> void Dispatch(const Arguments& args,Draw&& draw,Mask&& mask) {
    const bool player=Scope::Claim(args);
    draw();
    if(player)mask();
}
}
