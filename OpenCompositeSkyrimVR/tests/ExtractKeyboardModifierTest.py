from pathlib import Path
import sys
source=Path(sys.argv[1]).read_text(encoding='utf-8-sig')
def function(signature):
    a=source.index(signature); b=source.index('{',a); level=1; end=b+1
    while level:
        if source[end]=='{':level+=1
        elif source[end]=='}':level-=1
        end+=1
    return source[a:end]
functions=[function(s) for s in ['static void SendSingleVK(', 'static void SendPCVirtualKeyState(',
    'void VRKeyboard::PressHeldPCKey(', 'void VRKeyboard::PressControlKey(',
    'void VRKeyboard::ReleaseHeldPCKey(', 'void VRKeyboard::ToggleCtrlLatch(',
    'void VRKeyboard::ReleaseCtrlLatch(', 'void VRKeyboard::ReleaseAllHeldPCKeys(', 'bool VRKeyboard::HandleConsoleShortcut(', 'void VRKeyboard::PasteConsoleClipboard(']]
assert 'ReleaseAllHeldPCKeys();' in function('VRKeyboard::~VRKeyboard()')
assert 'PressControlKey((int)side, key.id, vk);' in source
assert 'if (HandleConsoleShortcut(ch)) { dirty = true; return; }' in source
assert 'if (sendInputOnly || consoleActive)\n\t\t\t\t\tToggleCtrlLatch' in source
mapping='CharToVK(ctrlLatched && caseMode == ECaseMode::LOCK ? key.ch : ch)'
assert mapping in source
functions.append('VkMapping VRKeyboard::MapPrintable(wchar_t base, wchar_t ch) { struct {wchar_t ch;} key{base}; return '+mapping+'; }')
Path(sys.argv[2]).write_text('\n\n'.join(functions)+'\n',encoding='utf-8')
