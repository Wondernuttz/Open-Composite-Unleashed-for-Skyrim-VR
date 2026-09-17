"""Extract the production bridge owner, capture, and publication code unchanged.

The executable supplies a narrow renderer fixture and counted COM resources so
it can verify that frequent probes do not retain or describe unchanged objects.
The real publication sequence, reader pinning, and ownership code run verbatim.
"""
from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text(encoding="utf-8-sig")
start = source.index("struct OCRenderTargetBridge {")
end_marker = "static_assert(offsetof(OCRenderTargetBridge, resourceReaders) % alignof(uint32_t) == 0);"
end = source.index(end_marker, start) + len(end_marker)
protocol = "#pragma pack(push, 1)\n" + source[start:end]
start = source.index("\ttemplate <class T>\n\tT* RetainBridgeResource")
end = source.index("\n\tvoid RefreshBridgeRenderTargets();", start)
owner = source[start:end]
start = source.index("\tvoid RefreshBridgeRenderTargets()\n")
end = source.index("\n\t// =========================================================================\n\t// RendererShadowState", start)
refresh = source[start:end]
output = Path(sys.argv[2])
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text(
    protocol + "\nOCRenderTargetBridge* g_pBridge = nullptr;\n" + owner + "\n" + refresh,
    encoding="utf-8",
)
