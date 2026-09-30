#pragma once
#include <array>

namespace DapaGeometryOwnership {
enum class Kind { None, Player, AttachedArrow, AttachedEquipment, Held };

template<class Node>
struct Roots {
    const Node* first = nullptr;
    const Node* third = nullptr;
    const Node* arrow = nullptr;
    const Node* arrowHold = nullptr;
    const Node* arrowSnap = nullptr;
    std::array<const Node*,11> equipment{};
};

// One bounded ancestry walk for the live draw. No names, form-ID guesses,
// persistent mesh pointers, or ownership of the shared PlayerWorldNode.
// ArrowFireNode is deliberately absent: launch/world geometry is not held.
template<class Node, class Reference, class HeldMatch>
Kind Classify(const Node* geometry, const Roots<Node>& roots,
    HeldMatch&& heldMatch, const Reference*& nearestReference)
{
    nearestReference = nullptr;
    for (unsigned depth = 0; geometry && depth < 64; ++depth, geometry = geometry->parent) {
        if (geometry == roots.first || geometry == roots.third) return Kind::Player;
        if (geometry == roots.arrow || geometry == roots.arrowHold || geometry == roots.arrowSnap)
            return Kind::AttachedArrow;
        for(const auto* root:roots.equipment)
            if(geometry==root)return Kind::AttachedEquipment;
        if (heldMatch(geometry)) return Kind::Held;
        if (!nearestReference) nearestReference = geometry->userData;
    }
    return Kind::None;
}
}
