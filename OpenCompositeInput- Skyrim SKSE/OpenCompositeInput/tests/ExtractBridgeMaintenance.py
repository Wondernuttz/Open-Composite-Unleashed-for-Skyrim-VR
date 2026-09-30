"""Extract the actual coalesced game-thread maintenance queue for testing."""
from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text(encoding="utf-8-sig")
start = source.index("\tvoid QueueBridgeMaintenance()")
end = source.index("\n\t// Scheduler:", start)
scheduler = source[source.index("\tvoid StartLaserPumpScheduler()", end):]
scheduler = scheduler[:scheduler.index('\n\t// =====')]
assert scheduler.count("QueueBridgeMaintenance();") == 1
assert "rtRefreshTick" not in scheduler
assert "std::chrono::milliseconds(8)" in scheduler
output = Path(sys.argv[2])
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text(source[start:end], encoding="utf-8")
