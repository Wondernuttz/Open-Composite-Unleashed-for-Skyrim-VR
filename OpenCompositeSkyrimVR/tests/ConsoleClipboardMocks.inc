// Mock the clipboard and game queue: tests never read the desktop clipboard.
static std::wstring clipboard = L"player.additem 0000000F 10";
static std::wstring delivered;
static bool busy=false, nontext=false, lockFailure=false, terminated=true, prisma=false;
static int closes=0, unlocks=0, bridge=1, failAt=-1;
static BOOL MockOpen(HWND){return !busy;}
static BOOL MockClose(){++closes;return TRUE;}
static HANDLE MockGet(UINT format){Check(format==CF_UNICODETEXT,"Unicode clipboard requested");return nontext?nullptr:HANDLE(1);}
static SIZE_T MockSize(HANDLE){return (clipboard.size()+(terminated?1:0))*sizeof(wchar_t);}
static void* MockLock(HANDLE){return lockFailure?nullptr:clipboard.data();}
static BOOL MockUnlock(HANDLE){++unlocks;return TRUE;}
static HWND GetGameWindow(){return HWND(1);}
int OCBridge_ConsoleState(){return bridge;}
static bool IsPrismaTextFocused(){return prisma;}
static constexpr UINT WM_OC_CHAR=WM_APP+0x4F45;
static BOOL MockPost(HWND,UINT message,WPARAM value,LPARAM mode){
    Check(message==WM_OC_CHAR && mode==0,"paste only emits character events");
    Check(value!=VK_RETURN && value!='\n',"paste never submits a command");
    Check(!down[VK_CONTROL]&&!down[VK_SHIFT],"paste has no held OS modifiers");
    if(failAt>=0 && int(delivered.size())==failAt)return FALSE;
    delivered+=wchar_t(value);return TRUE;
}
#define OpenClipboard MockOpen
#define CloseClipboard MockClose
#define GetClipboardData MockGet
#define GlobalSize MockSize
#define GlobalLock MockLock
#define GlobalUnlock MockUnlock
#define PostMessageW MockPost
#include "OpenOVR/Misc/Keyboard/ConsoleClipboard.h"

static void TestClipboard(VRKeyboard& kb){
    std::wstring out;
    Check(!ConsoleClipboard::Prepare(L"player.additem 0000000F 10\r\n",256,out) && out==clipboard,"trailing CRLF removed");
    Check(!ConsoleClipboard::Prepare(L"help\tIron",256,out)&&out==L"help Iron","tabs become spaces");
    Check(ConsoleClipboard::Prepare(L"one\ntwo",256,out)&&out.empty(),"multiline rejected before delivery");
    Check(ConsoleClipboard::Prepare(L"",256,out)&&out.empty(),"empty clipboard");
    Check(ConsoleClipboard::Prepare(L"\r\n",256,out)&&out.empty(),"empty line clipboard");
    Check(ConsoleClipboard::Prepare(L"abcd",3,out)&&out.empty(),"length overflow is not truncated");
    Check(!ConsoleClipboard::Prepare(L"abc",3,out)&&out==L"abc","exact capacity accepted");
    Check(ConsoleClipboard::Prepare(std::wstring(1,wchar_t(0xD800)),256,out)!=nullptr,"unpaired surrogate rejected");
    Check(ConsoleClipboard::Prepare(L"a\x1b",256,out)!=nullptr,"control characters rejected");
    Check(ConsoleClipboard::Prepare(std::wstring_view(L"a\0b",3),256,out)!=nullptr,"embedded NUL rejected");
    Check(!ConsoleClipboard::Prepare(L"help \"épée\"",256,out),"BMP Unicode text preserved");
    const int before=closes;
    busy=true;Check(ConsoleClipboard::Read(HWND(1),256,out)!=nullptr&&closes==before,"busy clipboard not closed or retried");busy=false;
    nontext=true;Check(ConsoleClipboard::Read(HWND(1),256,out)!=nullptr&&closes==before+1,"nontext closes clipboard");nontext=false;
    lockFailure=true;const int unlockBefore=unlocks;
    Check(ConsoleClipboard::Read(HWND(1),256,out)!=nullptr&&unlocks==unlockBefore,"failed lock is not unlocked");lockFailure=false;
    terminated=false;Check(ConsoleClipboard::Read(HWND(1),256,out)!=nullptr&&unlocks==unlockBefore+1,"unterminated buffer bounded and unlocked");terminated=true;
    Check(!ConsoleClipboard::Read(HWND(1),256,out)&&out==clipboard,"clipboard copied successfully");
    clipboard=std::wstring(5000,L'a');Check(ConsoleClipboard::Read(HWND(1),256,out)!=nullptr&&out.empty(),"large data scan bounded");
    clipboard=L"player.additem 0000000F 10\r\n";
    kb.ctrlLatched=false;kb.ctrlSentToPC=false;kb.consoleActive=true;kb.caseMode=VRKeyboard::LOWER;events.clear();
    Check(!kb.HandleConsoleShortcut(L'v'),"ordinary V still types normally");
    kb.ToggleCtrlLatch(0);Check(kb.ctrlLatched&&!down[VK_CONTROL]&&events.empty(),"console Ctrl is a local latch");
    Check(kb.HandleConsoleShortcut(L'v'),"console paste consumes V");
    Check(delivered==L"player.additem 0000000F 10" && kb.text==delivered,"exact command delivered and preview updated");
    Check(kb.cursorPos==int(delivered.size())&&!kb.ctrlLatched&&events.empty(),"no extra V or OS events");
    kb.ToggleCtrlLatch(0);kb.ToggleCtrlLatch(0);Check(!kb.ctrlLatched&&events.empty(),"tap Ctrl twice cancels local latch");
    delivered.clear();kb.text=L"ab";kb.cursorPos=1;clipboard=L"X";
    kb.ToggleCtrlLatch(1);kb.caseMode=VRKeyboard::SHIFT;kb.HandleConsoleShortcut(L'V');
    Check(kb.text==L"aXb"&&kb.cursorPos==2&&delivered==L"X"&&kb.caseMode==VRKeyboard::LOWER,"paste respects caret and consumes Shift");
    delivered.clear();kb.text=L"abc";kb.maxLength=3;
    kb.ToggleCtrlLatch(0);kb.HandleConsoleShortcut(L'v');Check(delivered.empty()&&kb.text==L"abc","full preview prevents partial paste");kb.maxLength=256;
    clipboard=L"one\ntwo";kb.text.clear();kb.cursorPos=0;
    kb.ToggleCtrlLatch(0);kb.HandleConsoleShortcut(L'v');Check(delivered.empty()&&kb.text.empty(),"multiline injects nothing");
    clipboard=L"valid";bridge=0;kb.PasteConsoleClipboard();Check(delivered.empty(),"closed real console rejects paste");
    bridge=-1;kb.PasteConsoleClipboard();Check(delivered.empty(),"missing bridge rejects paste");bridge=1;
    prisma=true;kb.PasteConsoleClipboard();Check(delivered.empty(),"Prisma focus rejects console paste");prisma=false;
    failAt=2;kb.PasteConsoleClipboard();Check(delivered==L"va"&&kb.text==L"va"&&kb.consoleStatus.find(L"interrupted")!=std::wstring::npos,"queue failure stops and reports partial text");failAt=-1;
    delivered.clear();kb.ToggleCtrlLatch(0);kb.HandleConsoleShortcut(L'c');Check(delivered.empty()&&!kb.ctrlLatched,"unsupported console chord never types a stray letter");
    kb.consoleActive=false;kb.ToggleCtrlLatch(0);Check(!kb.HandleConsoleShortcut(L'v')&&down[VK_CONTROL],"PC Ctrl+V retains native key path");kb.ReleaseAllHeldPCKeys();
    kb.consoleActive=true;kb.ToggleCtrlLatch(0);kb.PressControlKey(0,0,VK_LEFT);Check(!kb.ctrlLatched,"console navigation consumes local Ctrl latch");
}
