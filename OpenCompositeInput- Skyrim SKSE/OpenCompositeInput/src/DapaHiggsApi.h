#pragma once
#include <cstdint>

namespace RE { class TESObjectREFR; class TESForm; class NiObject; }

// HIGGS interface prefix adapted for CommonLib types, 2026-09-06.
// Upstream: adamhynek/higgs, include/higgsinterface001.h (GPL-3.0), commit
// 93bf67b1bc4c4a11a20ccaef0d5012781d0d7eee. See repository LICENSE.
// Keep EVERY preceding virtual slot, including unused methods. No virtual
// destructor: HIGGS owns this object. Only the verified prefix is declared.
namespace DapaHiggs {
struct Interface {
    using ReferenceCallback=void(*)(bool,RE::TESObjectREFR*);
    using FormCallback=void(*)(bool,RE::TESForm*);
    using Callback=void(*)();
    enum class FilterResult : std::uint8_t { Continue,Collide,Ignore };
    virtual unsigned int GetBuildNumber()=0; // 0
    virtual void AddPulledCallback(ReferenceCallback)=0;
    virtual void AddGrabbedCallback(ReferenceCallback)=0;
    virtual void AddDroppedCallback(ReferenceCallback)=0;
    virtual void AddStashedCallback(FormCallback)=0;
    virtual void AddConsumedCallback(FormCallback)=0;
    virtual void AddCollisionCallback(void(*)(bool,float,float))=0;
    virtual void GrabObject(RE::TESObjectREFR*,bool)=0;
    virtual RE::TESObjectREFR* GetGrabbedObject(bool)=0; // 8
    virtual bool IsHandInGrabbableState(bool)=0;
    virtual void DisableHand(bool)=0;
    virtual void EnableHand(bool)=0;
    virtual bool IsDisabled(bool)=0;
    virtual void DisableWeaponCollision(bool)=0;
    virtual void EnableWeaponCollision(bool)=0;
    virtual bool IsWeaponCollisionDisabled(bool)=0;
    virtual bool IsTwoHanding()=0;
    virtual void AddStartTwoHandingCallback(Callback)=0;
    virtual void AddStopTwoHandingCallback(Callback)=0;
    virtual bool CanGrabObject(bool)=0;
    virtual void AddCollisionFilterComparisonCallback(FilterResult(*)(void*,std::uint32_t,std::uint32_t))=0;
    virtual void AddPrePhysicsStepCallback(void(*)(void*))=0;
    virtual std::uint64_t GetHiggsLayerBitfield()=0;
    virtual void SetHiggsLayerBitfield(std::uint64_t)=0;
    virtual RE::NiObject* GetHandRigidBody(bool)=0;
    virtual RE::NiObject* GetWeaponRigidBody(bool)=0;
    virtual RE::NiObject* GetGrabbedRigidBody(bool)=0;
    virtual void ForceWeaponCollisionEnabled(bool)=0;
    virtual bool IsHoldingObject(bool)=0;
    virtual void GetFingerValues(bool,float[5])=0;
    virtual void AddPreVrikPreHiggsCallback(Callback)=0;
    virtual void AddPreVrikPostHiggsCallback(Callback)=0; // 31: after normal HIGGS Update
};
struct Message {
    static constexpr std::uint32_t type=0xF9279A57;
    void* (*getApiFunction)(unsigned int)=nullptr;
};
// Version number encoding is defined by HIGGS src/pluginapi.cpp. This is the
// version whose complete prefix and update timing were verified locally.
inline bool Supported(unsigned int build) { return build>=1101000 && build<2000000; }
}
