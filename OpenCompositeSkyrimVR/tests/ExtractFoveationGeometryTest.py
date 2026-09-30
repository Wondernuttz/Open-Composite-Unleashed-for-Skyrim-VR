"""Compile production admission, blackout preparation and submitted-copy geometry.

The prefix includes backend selection, hook admission and prepareBlackout. Only
external backend/hook/renderer readiness is controlled by the fixture. The final
call supplies scene eye sizes that normally come from each backend's atlas route;
no gate, guard calculation or return in the extracted code is rewritten. Drawing,
arming and final presentation have their own GPU fixtures.
"""
from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text(encoding="utf-8-sig")
start = source.index("void DX11Compositor::BeginVRSGameFrame()")
prepare = source.index("\n\tconst auto prepareBlackout =", start)
end = source.index("\n\tif (useHardwareVrs) {", prepare)
body = source[start:end]
body += """
    observed.admitted = true;
    observed.shared = hasStereoGeometry;
    observed.depth = separateEyeSceneDepth.Get();
    observed.width = separateEyeSceneDesc.Width;
    observed.height = separateEyeSceneDesc.Height;
    observed.mode = vrsMode;
    observed.radii = profileRadii;
    observed.rates = profileRates;
    prepareBlackout(testSceneEyeSize[0][0], testSceneEyeSize[0][1],
        testSceneEyeSize[1][0], testSceneEyeSize[1][1]);
    observed.blackout = blackout;
}
"""
# Extract the exact function used by ordinary submit copies, DAPA caching and
# next-frame eye geometry. Copy/readback tests exercise its returned rectangle.
region_start = source.index("static bool ResolveSubmittedTextureRegion(")
region_end = source.index("\n}\n", region_start) + len("\n}\n")
body = source[region_start:region_end] + "\n" + body
out = Path(sys.argv[2])
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(body, encoding="utf-8")
