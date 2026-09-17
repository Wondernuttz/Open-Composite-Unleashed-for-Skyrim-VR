"""Exercise the production mapping opener with controlled mapping lifetimes."""
from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text(encoding="utf-8-sig")
declaration = "static PublishedBridge<OCRenderTargetBridge> s_pBridge;"
if declaration in source:
    declarations = declaration
else:
    # Also accepts the old implementation for the before/after regression.
    start = source.index("static HANDLE s_hBridgeMap =")
    declarations = source[start:source.index("\nstatic void OpenRenderTargetBridge();", start)]
start = source.index("static void OpenRenderTargetBridge()\n{")
end = source.index("\nstruct OCBridgeResourceSnapshot", start)
output = Path(sys.argv[2])
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text(declarations + "\n" + source[start:end], encoding="utf-8")
