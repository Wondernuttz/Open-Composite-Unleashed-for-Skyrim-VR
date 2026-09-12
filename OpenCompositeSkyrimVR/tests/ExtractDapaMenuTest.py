"""Extract production menu, stereo-cache lifecycle, and synthetic scheduler code."""
from pathlib import Path
import sys

root, output = Path(sys.argv[1]), Path(sys.argv[2])
dx = (root / "OpenOVR/Compositor/dx11compositor.cpp").read_text(encoding="utf-8-sig")
header = (root / "DrvOpenXR/ASWProvider.h").read_text(encoding="utf-8-sig")
provider = (root / "DrvOpenXR/ASWProvider.cpp").read_text(encoding="utf-8-sig")
backend = (root / "DrvOpenXR/XrBackend.cpp").read_text(encoding="utf-8-sig")

def function(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    end, level = brace + 1, 1
    while level:
        level += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]

output.mkdir(parents=True, exist_ok=True)
(output / "DapaMenuBridge.inc").write_text(function(dx, "bool OCBridge_DapaMenuPaused()"), encoding="utf-8")
methods = "\n".join(function(header, signature) for signature in (
    "void InvalidateCachedFrame()", "void SetPaused(bool paused)", "void SetInjectionWanted(bool wanted)"))
cache = function(provider, "bool ASWProvider::CacheFrame(")
cache_prefix = cache[cache.index("{") + 1:cache.index("\n\tauto validRegion")]
cache_suffix = cache[cache.index("\tm_cacheBuildEyeMask |="):]
methods += "\nbool CacheEye(int eye) { const float nearZ=.1f,farZ=1000;" + cache_prefix + "\n" + cache_suffix
(output / "DapaMenuProvider.inc").write_text(methods, encoding="utf-8")
start = dx.index("if (g_aswProvider && g_aswProvider->IsReady()", dx.index("// OCU ASW: cache frame data"))
condition = dx[start + 3:dx.index(" {", start)]
(output / "DapaMenuCacheAdmission.inc").write_text("bool CanCache() { return " + condition + "; }", encoding="utf-8")
start = backend.index("\tconst bool canInject = prepareInjection && pacing.backoffMs == 0.0;")
end = backend.index("asw_done:", start) + len("asw_done:")
(output / "DapaMenuScheduler.inc").write_text("void RunScheduler() {\n" + backend[start:end] + "\nreturn;\n}", encoding="utf-8")
