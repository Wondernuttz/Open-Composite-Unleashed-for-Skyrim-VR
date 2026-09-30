"""Extract the real producer and frame-boundary code for the GPU lifecycle test."""
from pathlib import Path
import sys

runtime = Path(sys.argv[1]).read_text(encoding="utf-8-sig")
producer = Path(sys.argv[2]).read_text(encoding="utf-8-sig")

def function(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    end, level = brace + 1, 1
    while level:
        level += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]

start = runtime.index("void DX11Compositor::BeginVRSGameFrame()")
start = runtime.index("{", start) + 1
end = runtime.index("// Publish a fresh disabled frame", start)
boundary = "void BeginRealFrame() {" + runtime[start:end] + "}\n"
result = boundary + function(producer, "bool Prepare(") + "\n" + function(producer, "bool Publish(MaskAccess&")
output = Path(sys.argv[3])
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text(result, encoding="utf-8")
