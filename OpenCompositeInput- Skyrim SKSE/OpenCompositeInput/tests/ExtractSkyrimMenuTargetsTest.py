"""Extract the actual COM validation/ownership helpers for a standalone test."""
from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text(encoding="utf-8-sig")
start = source.index("bool EmptyRequest(")
end = source.index("} // namespace", start)
body = source[start:end]
for symbol in ("bool ValidTexture(", "bool ValidateTargets(", "bool CommitTargets("):
    if symbol not in body:
        raise RuntimeError(f"Missing production helper: {symbol}")
output = Path(sys.argv[2])
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text("// Generated from SkyrimMenuTargets.cpp; do not edit.\n" + body,
                  encoding="utf-8")
