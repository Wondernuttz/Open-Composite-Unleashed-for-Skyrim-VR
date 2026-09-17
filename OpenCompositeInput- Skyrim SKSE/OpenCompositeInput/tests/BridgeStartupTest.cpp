// Executes the production initializer/reconciler with the actual bridge layout.
// Only OS allocation, render-resource refresh, UI and logging are fixtures.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <stdexcept>
#include <string>

using HANDLE = void*;
using HWND = void*;
using std::intptr_t;
constexpr unsigned PAGE_READWRITE = 4, FILE_MAP_ALL_ACCESS = 1;
const HANDLE INVALID_HANDLE_VALUE = reinterpret_cast<HANDLE>(intptr_t(-1));
struct OCRenderTargetBridge;
OCRenderTargetBridge* g_pBridge = nullptr;
bool g_bridgeMenuStateObserved = false;
HANDLE g_hBridgeMapFile = nullptr;
HWND g_gameHwnd = nullptr;
std::set<std::string> g_activeTrackedMenus;
HANDLE CreateFileMappingW(HANDLE, void*, unsigned, unsigned, std::size_t, const wchar_t*);
void* MapViewOfFile(HANDLE, unsigned, unsigned, unsigned, std::size_t);
bool CloseHandle(HANDLE);
unsigned GetLastError() { return 1; }
void MemoryBarrier();
void RefreshBridgeRenderTargets();
int publishedProperty = -1;
bool SetPropW(HWND, const wchar_t*, HANDLE value)
{
	publishedProperty = static_cast<int>(reinterpret_cast<intptr_t>(value));
	return true;
}
namespace SKSE::log {
template <class... Args> void info(const char*, Args...) {}
template <class... Args> void error(const char*, Args...) {}
template <class... Args> void debug(const char*, Args...) {}
}
namespace RE {
struct UI {
	inline static UI* current = nullptr;
	bool paused = false;
	bool mainMenu = false, loadingMenu = false, console = false;
	unsigned menuQueries = 0;
	static UI* GetSingleton() { return current; }
	bool GameIsPaused() const { return paused; }
	bool IsMenuOpen(const char* name) {
		++menuQueries;
		return (mainMenu && std::string(name) == "Main Menu") ||
		    (loadingMenu && std::string(name) == "Loading Menu") || (console && std::string(name) == "Console");
	}
};
}
#include "BridgeStartupProduction.inl"

OCRenderTargetBridge storage;
bool createSucceeds = true, mapSucceeds = true;
unsigned checks = 0, refreshes = 0, barriers = 0, maps = 0, closes = 0;
void Check(bool condition, const char* message)
{
	++checks;
	if (!condition) throw std::runtime_error(message);
}
HANDLE CreateFileMappingW(HANDLE, void*, unsigned, unsigned, std::size_t bytes, const wchar_t*)
{
	Check(bytes == sizeof(storage), "production mapping size retained");
	return createSucceeds ? &storage : nullptr;
}
void* MapViewOfFile(HANDLE, unsigned, unsigned, unsigned, std::size_t)
{
	++maps;
	return mapSucceeds ? &storage : nullptr;
}
bool CloseHandle(HANDLE) { ++closes; return true; }
void MemoryBarrier()
{
	++barriers;
	Check(g_pBridge && g_pBridge->isMenuOpen == 1, "menu protection precedes header publication barrier");
	Check(g_pBridge->magic == 0 && g_pBridge->byteSize == 0, "header is not yet valid at protection barrier");
}
void RefreshBridgeRenderTargets()
{
	++refreshes;
	Check(g_pBridge->isMenuOpen == 1, "render resources cannot publish gameplay before a menu observation");
	Check(g_pBridge->magic == OCRenderTargetBridge::MAGIC &&
	    g_pBridge->version == OCRenderTargetBridge::VERSION && g_pBridge->byteSize == sizeof(storage),
	    "initialized bridge retains valid protocol header");
	Check(barriers == 1, "protected startup state precedes valid resource publication");
	g_pBridge->status = 1;
}
int main()
{
	try {
		createSucceeds = false;
		CreateRenderTargetBridge();
		Check(!g_pBridge && maps == 0 && refreshes == 0, "mapping creation failure publishes nothing");
		createSucceeds = true;
		mapSucceeds = false;
		CreateRenderTargetBridge();
		Check(!g_pBridge && !g_hBridgeMapFile && closes == 1 && refreshes == 0, "view failure closes its handle without publication");
		mapSucceeds = true;
		std::memset(&storage, 0xcd, sizeof(storage));
		CreateRenderTargetBridge();
		Check(g_pBridge->status == 1 && g_pBridge->isMenuOpen == 1, "resource-ready startup is still menu-protected");
		Check(g_pBridge->isMainMenu == 0 && g_pBridge->isLoadingScreen == 0 && g_pBridge->isConsoleOpen == 0,
		    "startup protection does not invent named menu events");
		g_gameHwnd = reinterpret_cast<HWND>(1);
		RefreshMenuActivityFromGameState();
		Check(g_pBridge->isMenuOpen == 1 && publishedProperty == 1, "missing UI preserves protected startup");
		RE::UI ui;
		ui.paused = true;
		RE::UI::current = &ui;
		RefreshMenuActivityFromGameState();
		Check(g_pBridge->isMenuOpen == 1, "observed pause retains protection");
		ui.paused = false;
		RefreshMenuActivityFromGameState();
		Check(g_pBridge->isMenuOpen == 1 && ui.menuQueries == 0, "event-safe reconciliation awaits maintenance snapshot");
		ui.mainMenu = true;
		ObserveInitialBridgeMenuState();
		RefreshMenuActivityFromGameState();
		Check(g_pBridge->isMenuOpen == 1 && g_pBridge->isMainMenu == 1, "already-open nonpausing main menu stays protected");
		ObserveInitialBridgeMenuState();
		Check(ui.menuQueries == 3, "startup menu snapshot only occurs once");
		ui.mainMenu = false;
		g_pBridge->isMainMenu = 0; // Subsequent close event uses the existing watcher.
		RefreshMenuActivityFromGameState();
		Check(g_pBridge->isMenuOpen == 0 && publishedProperty == 0, "observed gameplay clears startup without another menu event");
		for (unsigned menu = 0; menu < 2; ++menu) {
			// Rearm a startup observation with each other already-open menu.
			g_bridgeMenuStateObserved = false;
			ui.loadingMenu = menu == 0;
			ui.console = menu == 1;
			const auto queries = ui.menuQueries;
			RefreshMenuActivityFromGameState();
			Check(g_pBridge->isMenuOpen == 1 && ui.menuQueries == queries, "event dispatch holds unknown state without querying menus");
			ObserveInitialBridgeMenuState();
			RefreshMenuActivityFromGameState();
			Check(g_pBridge->isMenuOpen == 1 &&
			    (menu == 0 ? g_pBridge->isLoadingScreen : g_pBridge->isConsoleOpen),
			    "missed nonpausing loading or console menu stays protected");
			Check(ui.menuQueries == queries + 3, "maintenance alone queries the initial menu state");
			ui.loadingMenu = ui.console = false;
			g_pBridge->isLoadingScreen = g_pBridge->isConsoleOpen = 0;
			RefreshMenuActivityFromGameState();
			Check(g_pBridge->isMenuOpen == 0, "later close event releases initial loading or console protection");
		}
		g_activeTrackedMenus.insert("FavoritesMenu");
		RefreshMenuActivityFromGameState();
		Check(g_pBridge->isMenuOpen == 1, "nonpausing tracked menu remains protected");
		g_activeTrackedMenus.clear();
		RefreshMenuActivityFromGameState();
		Check(g_pBridge->isMenuOpen == 0, "existing menu reconciliation restores gameplay");
		std::printf("Bridge startup menu state: %u checks passed\n", checks);
		return 0;
	} catch (const std::exception& error) {
		std::fprintf(stderr, "Bridge startup menu state FAILED: %s\n", error.what());
		return 1;
	}
}
