#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <cstdint>
#include <vector>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <algorithm>
static int checks;
static void Check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
static std::vector<INPUT> events;
static std::array<bool,256> down{};
static bool suppress=false;
static UINT CaptureSendInput(UINT n,INPUT* inputs,int size) {
    Check(size==sizeof(INPUT),"native input structure size");
    for(UINT i=0;i<n;++i){events.push_back(inputs[i]);auto& k=inputs[i].ki;
        down[(k.dwFlags&KEYEVENTF_SCANCODE)?k.wScan:k.wVk]=!(k.dwFlags&KEYEVENTF_KEYUP);}
    return n;
}
static UINT MockMapVirtualKeyW(UINT key,UINT){return key;}
#define SendInput CaptureSendInput
#define MapVirtualKeyW MockMapVirtualKeyW
#define OOVR_DEBUG_LOGF(...) ((void)0)
#define OOVR_LOGF(...) ((void)0)
static bool ShouldSuppressSkyrimInput(){return suppress;}
static bool IsExtendedKey(WORD key){return key==VK_LEFT;}
static void EnsureGameForeground(){}
static int s_pressedKey[2]{};
struct VkMapping {uint16_t vk;bool needsShift;};
static VkMapping CharToVK(wchar_t ch){return {uint16_t(ch>='a'&&ch<='z'?ch-32:ch),ch>='A'&&ch<='Z'};}
class VRKeyboard {
public:
    enum ECaseMode {LOWER,SHIFT,LOCK};ECaseMode caseMode=LOWER;
    struct HeldPCKey {uint16_t vk=0;int keyId=-1;bool shift=false,scanOnly=false;};
    HeldPCKey heldPCKeys[2];bool releaseCtrlAfterHeldPCKey[2]{};
    bool ctrlLatched=false,ctrlSentToPC=false,dirty=false,sendInputOnly=true,consoleActive=false;int ctrlLatchSide=-1;
    std::wstring text,consoleStatus; int cursorPos=0; uint32_t maxLength=256; bool consoleDirty=false;
    void PasteConsoleClipboard();
    bool HandleConsoleShortcut(wchar_t);
    void PressHeldPCKey(int,int,uint16_t,bool,bool);
    void PressControlKey(int,int,uint16_t);
    void ReleaseHeldPCKey(int);
    void ToggleCtrlLatch(int);
    void ReleaseCtrlLatch();
    void ReleaseAllHeldPCKeys();
    VkMapping MapPrintable(wchar_t,wchar_t);
};
#include "ConsoleClipboardMocks.inc"
#include "KeyboardModifierProduction.inc"
static void Event(size_t index,WORD vk,bool pressed){Check(index<events.size(),"event exists");const auto k=events[index].ki;
    Check(k.wVk==vk && bool(!(k.dwFlags&KEYEVENTF_KEYUP))==pressed,"modifier/key order");}
int main(){try{
    VRKeyboard kb;
    kb.ToggleCtrlLatch(0);kb.caseMode=VRKeyboard::SHIFT;kb.PressControlKey(1,8,VK_F8);
    Check(down[VK_CONTROL]&&down[VK_SHIFT]&&down[VK_F8],"VR Ctrl+Shift+F8 held as real Windows chord");
    Check(kb.caseMode==VRKeyboard::LOWER,"one-shot Shift consumed");
    Event(0,VK_CONTROL,true);Event(1,VK_SHIFT,true);Event(2,VK_F8,true);
    kb.ReleaseHeldPCKey(1);
    Event(3,VK_F8,false);Event(4,VK_SHIFT,false);Event(5,VK_CONTROL,false);
    Check(!down[VK_CONTROL]&&!down[VK_SHIFT]&&!down[VK_F8],"release leaves no stuck modifier");
    events.clear();kb.caseMode=VRKeyboard::LOCK;kb.PressControlKey(0,8,VK_F8);
    Check(!down[VK_SHIFT]&&down[VK_F8]&&kb.caseMode==VRKeyboard::LOCK,"Caps does not become Shift+F8");
    kb.ReleaseHeldPCKey(0);Check(events.size()==2,"Caps produces only F8 press/release");
    events.clear();kb.ToggleCtrlLatch(0);kb.caseMode=VRKeyboard::SHIFT;kb.PressControlKey(0,8,VK_F8);
    kb.caseMode=VRKeyboard::SHIFT;kb.PressControlKey(1,9,VK_F9);
    kb.ReleaseHeldPCKey(0);
    Check(down[VK_CONTROL]&&down[VK_SHIFT]&&down[VK_F9]&&!down[VK_F8],"first hand release preserves second hand chord");
    kb.ReleaseHeldPCKey(1);Check(!down[VK_CONTROL]&&!down[VK_SHIFT]&&!down[VK_F9],"last hand releases shared modifiers");
    kb.ToggleCtrlLatch(0);kb.caseMode=VRKeyboard::SHIFT;kb.PressControlKey(1,8,VK_F8);kb.ReleaseAllHeldPCKeys();
    Check(!down[VK_CONTROL]&&!down[VK_SHIFT]&&!down[VK_F8],"keyboard-close cleanup releases all keys");
    kb.ToggleCtrlLatch(0);kb.ReleaseAllHeldPCKeys();Check(!down[VK_CONTROL],"closing with unused Ctrl latch releases it");
    kb.caseMode=VRKeyboard::SHIFT;kb.PressHeldPCKey(0,1,'A',true,true);kb.PressControlKey(1,8,VK_F8);
    kb.ReleaseHeldPCKey(0);Check(down[VK_SHIFT]&&down[VK_F8],"printable and F-key share Shift");
    kb.ReleaseHeldPCKey(1);Check(!down[VK_SHIFT],"mixed scancode/VK release clears Shift");
    events.clear();kb.sendInputOnly=false;kb.caseMode=VRKeyboard::SHIFT;kb.PressControlKey(0,8,VK_F8);
    Event(0,VK_SHIFT,true);Event(1,VK_F8,true);Event(2,VK_F8,false);Event(3,VK_SHIFT,false);
    Check(!down[VK_SHIFT]&&!down[VK_F8],"tap-mode modified key balanced");
    events.clear();suppress=true;SendSingleVK(VK_LEFT,false,true);Check(events.empty(),"text-focus suppression retained");
    SendSingleVK(VK_F8,false,true);Check(events.size()==4,"function-key bypass keeps modifier sequence");suppress=false;
    kb.ctrlLatched=true;kb.caseMode=VRKeyboard::LOCK;
    auto mapping=kb.MapPrintable('a','A');Check(mapping.vk=='A'&&!mapping.needsShift,"Caps+Ctrl+A does not invent Shift");
    kb.caseMode=VRKeyboard::SHIFT;mapping=kb.MapPrintable('a','A');Check(mapping.needsShift,"explicit Shift retained for Ctrl+Shift+A");
    kb.ctrlLatched=false;kb.caseMode=VRKeyboard::LOCK;mapping=kb.MapPrintable('a','A');Check(mapping.needsShift,"Caps still emits uppercase text");
    TestClipboard(kb);
    printf("Keyboard modifier and clipboard tests: %d checks passed\n",checks);return 0;
}catch(const std::exception& e){fprintf(stderr,"FAIL: %s\n",e.what());return 1;}}
