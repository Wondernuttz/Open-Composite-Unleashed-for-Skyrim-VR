"""Compile production gaze sampling/reset and quaternion math against a fake runtime."""
from pathlib import Path
import sys

runtime = Path(sys.argv[1]).read_text(encoding="utf-8-sig")
math = Path(sys.argv[2]).read_text(encoding="utf-8-sig")

def function(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    end, level = brace + 1, 1
    while level:
        level += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]

result = "\n".join(function(math, signature) for signature in (
    "float v3_dot(", "XrVector3f v3_cross(", "XrVector3f operator*(",
    "XrVector3f operator+(", "void rotate_vector_by_quaternion("))
result += "\n" + function(runtime, "void BaseInput::DestroyEyeGazeSpace()")
result += "\n" + function(runtime, "bool BaseInput::SampleEyeGazeDirection(")
output = Path(sys.argv[3])
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text(result, encoding="utf-8")
