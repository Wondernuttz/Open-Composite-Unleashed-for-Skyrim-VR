from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text(encoding="utf-8-sig")
start = source.index("\tbool RepairJournalSystemFocus(")
end = source.index("\n\t// StatsPage", start)
pump = source[source.index("\tvoid LaserCursorPumpOnce()"):source.index("\tvoid ConsoleWorldPickOnce()")]
assert "RepairJournalSystemFocus(" not in pump.replace("QueueJournalSystemFocusRepair(", "")
assert "now >= s_clickRearmNotBefore" in pump
Path(sys.argv[2]).write_text(source[start:end], encoding="utf-8")
