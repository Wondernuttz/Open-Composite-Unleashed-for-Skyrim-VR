"""Extract the actual bridge layout, initialization and menu reconciliation."""
from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text(encoding="utf-8-sig")
start = source.index("struct OCRenderTargetBridge {")
end_marker = "static_assert(offsetof(OCRenderTargetBridge, resourceReaders) % alignof(uint32_t) == 0);"
end = source.index(end_marker, start) + len(end_marker)
protocol = "#pragma pack(push, 1)\n" + source[start:end]
start = source.index("\tvoid CreateRenderTargetBridge()")
end = source.index("\n\t// Probe the game's MV", start)
creation = source[start:end]
startup = "void ObserveInitialBridgeMenuState() {}\n"  # Old producer had no startup observer.
if "\tvoid ObserveInitialBridgeMenuState()" in source:
    start = source.index("\tvoid ObserveInitialBridgeMenuState()")
    end = source.index("\n\t// Reconcile on the game thread", start)
    startup = source[start:end]
start = source.index("\tvoid RefreshMenuActivityFromGameState()")
end = source.index("\n\tclass MenuWatcher", start)
menu = source[start:end]
output = Path(sys.argv[2])
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text(protocol + "\n" + creation + "\n" + startup + "\n" + menu, encoding="utf-8")
