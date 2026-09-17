// Executes the exact Main.cpp helper. Fixtures replace only the UI singleton,
// bridge storage and Win32 property output; the production policy is unchanged.
#include <cstdint>
#include <cstdio>
#include <set>
#include <stdexcept>
#include <string>

using HWND = void*;
using HANDLE = void*;
using std::intptr_t;
namespace SKSE::log {
template <class... Args> void debug(const char*, Args...) {}
}
namespace RE {
struct UI {
    static UI* current;
    bool paused = false;
    bool mainMenu = false;
    unsigned menuQueries = 0;
    static UI* GetSingleton() { return current; }
    bool GameIsPaused() const { return paused; }
    bool IsMenuOpen(const char* name) { ++menuQueries; return mainMenu && std::string(name) == "Main Menu"; }
};
UI* UI::current = nullptr;
}
struct ObservedByte {
    int value = 0;
    unsigned writes = 0;
    operator int() const { return value; }
    ObservedByte& operator=(int next) { value = next; ++writes; return *this; }
};
struct Bridge { ObservedByte isMenuOpen; uint8_t isMainMenu = 0, isLoadingScreen = 0, isConsoleOpen = 0; };
Bridge* g_pBridge = nullptr;
bool g_bridgeMenuStateObserved = false;
HWND g_gameHwnd = nullptr;
std::set<std::string> g_activeTrackedMenus;
unsigned propertyWrites = 0;
int propertyValue = -1;
HWND propertyWindow = nullptr;
bool failPropertyWrite = false;
bool SetPropW(HWND window, const wchar_t* name, HANDLE value)
{
    if (std::wstring(name) != L"OC_MENU_ACTIVE")
        throw std::runtime_error("Unexpected property name");
    ++propertyWrites;
    if (failPropertyWrite)
        return false;
    propertyWindow = window;
    propertyValue = static_cast<int>(reinterpret_cast<intptr_t>(value));
    return true;
}

#include "MenuActivityProduction.inl"

unsigned checks = 0;
void Check(bool condition, const char* message)
{
    ++checks;
    if (!condition)
        throw std::runtime_error(message);
}
void CheckActive(bool active, const char* message)
{
    Check(g_pBridge && g_pBridge->isMenuOpen == (active ? 1 : 0), message);
    Check(propertyValue == (active ? 1 : 0), "Window and bridge must agree");
}
int main()
{
    try {
        // Initial observation without UI/window/bridge must not dereference any.
        RefreshMenuActivityFromGameState();
        Check(propertyWrites == 0, "No window means no SetPropW");
        Bridge firstBridge;
        g_pBridge = &firstBridge;
        g_gameHwnd = reinterpret_cast<HWND>(1);
        RefreshMenuActivityFromGameState();
        CheckActive(true, "Unknown startup must protect menus");
        Check(firstBridge.isMenuOpen.writes == 1, "Late bridge must receive active state");

        RE::UI ui;
        RE::UI::current = &ui;
        RefreshMenuActivityFromGameState();
        CheckActive(true, "Unpaused UI alone cannot disprove a missed startup menu");
        Check(ui.menuQueries == 0, "Event-safe reconciliation never queries locked menus");
        ui.mainMenu = true;
        ObserveInitialBridgeMenuState();
        RefreshMenuActivityFromGameState();
        CheckActive(true, "Maintenance detects a nonpausing main menu opened before registration");
        Check(ui.menuQueries == 3, "Initial main/loading/console snapshot happens once");
        ObserveInitialBridgeMenuState();
        Check(ui.menuQueries == 3, "Startup observation does not add recurring menu queries");
        ui.mainMenu = false;
        firstBridge.isMainMenu = 0; // The existing main-menu close event publishes this.
        RefreshMenuActivityFromGameState();
        CheckActive(false, "Live unpaused gameplay must clear startup protection");
        const auto quietPropertyWrites = propertyWrites;
        const auto quietBridgeWrites = firstBridge.isMenuOpen.writes;
        for (unsigned i = 0; i < 100; ++i)
            RefreshMenuActivityFromGameState();
        Check(propertyWrites == quietPropertyWrites, "Stable gameplay must not rewrite window property");
        Check(firstBridge.isMenuOpen.writes == quietBridgeWrites, "Stable gameplay must not rewrite bridge");

        // A closing event can precede Bethesda's pause-count update. No later
        // event is supplied: the maintenance observation alone must recover.
        g_activeTrackedMenus.insert("InventoryMenu");
        ui.paused = true;
        RefreshMenuActivityFromGameState();
        CheckActive(true, "Inventory must remain protected");
        g_activeTrackedMenus.clear();
        RefreshMenuActivityFromGameState();
        CheckActive(true, "Close event cannot prematurely ignore a still-live pause");
        ui.paused = false;
        RefreshMenuActivityFromGameState();
        CheckActive(false, "Post-event pause release must recover without another event");

        // Nested and non-pausing tracked menus still protect their contents.
        g_activeTrackedMenus.insert("Dialogue Menu");
        g_activeTrackedMenus.insert("Book Menu");
        RefreshMenuActivityFromGameState();
        CheckActive(true, "Non-pausing tracked menus must be protected");
        g_activeTrackedMenus.erase("Book Menu");
        RefreshMenuActivityFromGameState();
        CheckActive(true, "Closing only the top menu must preserve the underlying one");
        g_activeTrackedMenus.clear();
        RefreshMenuActivityFromGameState();
        CheckActive(false, "Last non-pausing menu close must restore gameplay");

        // Untracked menus that really pause remain covered by the old policy.
        ui.paused = true;
        RefreshMenuActivityFromGameState();
        CheckActive(true, "A real pause without tracked menus must remain protected");
        RE::UI::current = nullptr;
        RefreshMenuActivityFromGameState();
        CheckActive(true, "Missing UI must preserve a known active pause");
        ui.paused = false;
        RE::UI::current = &ui;
        RefreshMenuActivityFromGameState();
        CheckActive(false, "UI recovery must clear an ended pause");

        RE::UI::current = nullptr;
        RefreshMenuActivityFromGameState();
        CheckActive(false, "Missing UI must preserve known unpaused gameplay");
        g_activeTrackedMenus.insert("FavoritesMenu");
        RefreshMenuActivityFromGameState();
        CheckActive(true, "Tracked menu events must protect even without UI");
        g_activeTrackedMenus.clear();
        RefreshMenuActivityFromGameState();
        CheckActive(true, "Missing UI cannot confirm the final close released pause");
        RE::UI::current = &ui;
        RefreshMenuActivityFromGameState();
        CheckActive(false, "Live gameplay must recover after UI absence");

        // A newly available bridge/window must see the current state even if
        // there is no transition. Correct bridge bytes need no extra writes.
        g_pBridge = nullptr;
        g_gameHwnd = nullptr;
        RefreshMenuActivityFromGameState();
        Bridge secondBridge;
        secondBridge.isMenuOpen.value = 1;
        g_pBridge = &secondBridge;
        g_gameHwnd = reinterpret_cast<HWND>(2);
        RefreshMenuActivityFromGameState();
        CheckActive(false, "Late bridge must clear stale protection");
        Check(secondBridge.isMenuOpen.writes == 1, "Late bridge must be synchronized once");
        Check(propertyWindow == g_gameHwnd, "Recreated HWND must receive current state");
        const auto writesAfterNewWindow = propertyWrites;
        RefreshMenuActivityFromGameState();
        Check(propertyWrites == writesAfterNewWindow, "Unchanged new HWND must not be rewritten");
        Check(secondBridge.isMenuOpen.writes == 1, "Unchanged new bridge must not be rewritten");

        // A failed SetPropW must not poison the cache and suppress retries.
        g_gameHwnd = reinterpret_cast<HWND>(3);
        failPropertyWrite = true;
        RefreshMenuActivityFromGameState();
        Check(propertyWindow != g_gameHwnd, "Fixture must fail the first property write");
        failPropertyWrite = false;
        RefreshMenuActivityFromGameState();
        Check(propertyWindow == g_gameHwnd, "Failed property write must retry on next refresh");
        CheckActive(false, "Window retry must retain current gameplay state");
        std::printf("Menu activity reconciliation: %u checks passed\n", checks);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Menu activity reconciliation FAILED: %s\n", error.what());
        return 1;
    }
}
