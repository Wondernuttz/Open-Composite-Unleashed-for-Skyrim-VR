#pragma once
#include <array>
#include <atomic>
#include <mutex>
#include <utility>

// Two independently owned roots. Render reads are identity comparisons only;
// they never dereference a cached node. Only a potential match takes the lock
// and revalidates against the pinned current roots (no stale pointer match).
template<class Node,class Pointer>
class DapaHeldRoots {
    mutable std::mutex mutex;
    std::array<Pointer,2> pins{};
    std::array<std::atomic<Node*>,2> published{};
public:
    using Snapshot=std::array<Node*,2>;
    bool Set(bool left,Pointer root) {
        std::scoped_lock lock(mutex);
        const auto i=left?0:1;
        if(pins[i].get()==root.get())return false;
        // Invalidate before releasing ownership of the previous root.
        published[i].store(nullptr,std::memory_order_release);
        pins[i]=std::move(root);
        published[i].store(pins[i].get(),std::memory_order_release);
        return true;
    }
    void Clear() { Set(true,{});Set(false,{}); }
    Snapshot Read() const {
        return {published[0].load(std::memory_order_acquire),
                published[1].load(std::memory_order_acquire)};
    }
    bool Matches(const Node* node,const Snapshot& snapshot) const {
        if(!node || (node!=snapshot[0] && node!=snapshot[1]))return false;
        std::scoped_lock lock(mutex);
        return node==pins[0].get() || node==pins[1].get();
    }
};
