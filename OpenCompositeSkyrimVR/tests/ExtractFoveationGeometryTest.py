"""Compile the actual pre-render admission and bridge routing with narrow fixtures.

The extracted code ends before backend initialization. No predicate or return is
rewritten: the test observes whether production reached the selected scene route.
Real DX11 resources supply descriptors; hardware hooks are tested separately.
"""
from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text(encoding="utf-8-sig")
start = source.index("void DX11Compositor::BeginVRSGameFrame()")
end = source.index("\n\tconst bool mayUseVrs =", start)
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
}
"""
out = Path(sys.argv[2])
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(body, encoding="utf-8")
