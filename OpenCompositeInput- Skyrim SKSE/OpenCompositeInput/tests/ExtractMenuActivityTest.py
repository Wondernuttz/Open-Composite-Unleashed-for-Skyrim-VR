"""Extract the production menu publication helper without rewriting its policy."""
from pathlib import Path
import sys

source = Path(sys.argv[1]).read_text(encoding="utf-8-sig")
startup_start = source.index("\tvoid ObserveInitialBridgeMenuState()")
startup_end = source.index("\n\t// Reconcile on the game thread", startup_start)
startup = source[startup_start:startup_end]
start = source.index("\tvoid RefreshMenuActivityFromGameState()")
end = source.index("\n\tclass MenuWatcher", start)
body = source[start:end]
for unsafe in ("IsMenuOpen(", "IsShowingMenus(", "GetMenu(", "uiMovie", "MovieDef"):
    if unsafe in body:
        raise RuntimeError(f"Menu reconciliation must not query locked menu/movie state: {unsafe}")
watcher_start = source.index("\n\tclass MenuWatcher", end)
watcher_end = source.index("\n\t// =========================================================================", watcher_start)
watcher = source[watcher_start:watcher_end]
if "ObserveInitialBridgeMenuState" in watcher:
    raise RuntimeError("Initial locked-menu snapshot must never run from menu event dispatch")
if "RefreshMenuActivityFromGameState();" not in watcher:
    raise RuntimeError("Menu events must call the same reconciliation helper")
if "if (!g_gameHwnd || !a_event)" in watcher:
    raise RuntimeError("Menu events must be recorded before the HWND is available")
maintenance_start = source.index("\tvoid QueueBridgeMaintenance()")
maintenance_end = source.index("\n\t// Scheduler:", maintenance_start)
maintenance = source[maintenance_start:maintenance_end]
if maintenance.index("ObserveInitialBridgeMenuState();") > maintenance.index("RefreshMenuActivityFromGameState();"):
    raise RuntimeError("Game-thread maintenance must observe startup menus before reconciliation")
output = Path(sys.argv[2])
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text(startup + "\n" + body, encoding="utf-8")
