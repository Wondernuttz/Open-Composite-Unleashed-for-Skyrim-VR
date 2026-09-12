"""Extract the actual INI handler; tests omit only constructor I/O and logging."""
from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text(encoding="utf-8-sig")
helpers = source[source.index("static string str_tolower("):source.index("static int parse_dlss_model(")]
handler = source[source.index("int Config::ini_handler("):source.index("static float dlss_preset_render_scale(")]
output = Path(sys.argv[2])
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text(helpers + "\n" + handler, encoding="utf-8")
