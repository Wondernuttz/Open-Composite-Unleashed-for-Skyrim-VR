"""Extract the production mask consumer, pre-injection check, and ASW mask copy."""
from pathlib import Path
import sys

root, output = Path(sys.argv[1]), Path(sys.argv[2])
dx = (root / "OpenOVR/Compositor/dx11compositor.cpp").read_text(encoding="utf-8-sig")
asw = (root / "DrvOpenXR/ASWProvider.cpp").read_text(encoding="utf-8-sig")

def function(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    end, level = brace + 1, 1
    while level:
        level += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]

output.mkdir(parents=True, exist_ok=True)
start = dx.index("\t\t\tbool aswEyeCached = false;")
end = dx.index("\n\t\t\t// Only BeginVRSGameFrame", start)
(output / "DapaMaskConsumerProduction.inc").write_text(dx[start:end], encoding="utf-8")
start = asw.index("\tm_bodyValid[eye] = false;", asw.index("bool ASWProvider::CacheFrame("))
end = asw.index("\n\tm_cachedPose[eye]", start)
(output / "DapaBodyMaskCopyProduction.inc").write_text(asw[start:end], encoding="utf-8")
(output / "DapaMaskCacheValidProduction.inc").write_text(
    function(dx, "bool OCBridge_DapaMaskCacheValid()"), encoding="utf-8")
