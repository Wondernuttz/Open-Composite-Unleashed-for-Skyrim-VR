"""Exercise the production movement entry point with controlled runtime lifetimes."""
from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text(encoding="utf-8-sig")
start = source.index("BaseInput::ExternalMovement BaseInput::ReadExternalMovement(")
end = source.index("\nbool BaseInput::HasWalkInPlaceActivationButton()", start)
output = Path(sys.argv[2])
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text(source[start:end], encoding="utf-8")
