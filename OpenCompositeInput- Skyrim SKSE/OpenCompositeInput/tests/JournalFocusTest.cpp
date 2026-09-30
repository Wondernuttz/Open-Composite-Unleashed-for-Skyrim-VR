// Exercise the extracted production guards and UI dispatch with controllable movies.
#include <atomic>
#include <cstdint>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
static int checks = 0, invokes = 0;
void Check(bool ok) { ++checks; if (!ok) throw std::runtime_error("Journal focus check " + std::to_string(checks)); }
namespace RE {
struct GFxValue {
    bool display = false, boolean = false, value = false, usable = true;
    double number = 0;
    std::map<std::string, bool> flags;
    bool IsDisplayObject() const { return display; }
    bool IsBool() const { return boolean; }
    bool GetBool() const { return value; }
    bool GetMember(const char* n, GFxValue* out) {
        auto it = flags.find(n); if (it == flags.end()) return false;
        out->boolean = true; out->value = it->second; return true;
    }
    void SetNumber(double n) { number = n; }
    bool Invoke(const char* name, GFxValue* result, GFxValue*, int count) {
        Check(std::string(name) == "UpdateStateFocus" && result && count == 1);
        ++invokes; return true;
    }
};
struct GFxMovieView {
    int tab = 2, state = 3;
    bool interactive = true;
    std::map<std::string, GFxValue> variables;
    bool GetVariable(GFxValue* out, const char* path) {
        auto it = variables.find(path); if (it == variables.end()) return false;
        *out = it->second; return true;
    }
};
template<class T> using GPtr = std::shared_ptr<T>;
struct Menu { GPtr<GFxMovieView> uiMovie; };
struct UI {
    bool open = true;
    std::shared_ptr<Menu> menu;
    static UI* GetSingleton();
    bool IsMenuOpen(const char*) { return open; }
    std::shared_ptr<Menu> GetMenu(const char*) { return menu; }
};
UI ui;
UI* UI::GetSingleton() { return &ui; }
}
namespace SKSE {
struct Tasks {
    std::vector<std::function<void()>> pending;
    void AddUITask(std::function<void()> f) { pending.push_back(std::move(f)); }
    void Run() { auto work = std::move(pending); pending.clear(); for (auto& f : work) f(); }
} tasks;
Tasks* GetTaskInterface() { return &tasks; }
namespace log { template<class... T> void debug(const char*, T...) {} }
}
bool DisplayObjectIsUsable(RE::GFxValue& v) { return v.usable; }
bool JournalMainFaderIsInteractive(RE::GFxMovieView& m) { return m.interactive; }
bool GetNumberVariable(RE::GFxMovieView& m, const char* path, const char*, int& out) {
    out = std::string(path).find("iCurrentTab") != std::string::npos ? m.tab : m.state; return true;
}
#include "JournalFocusProduction.inl"
const std::string primary = "_root.QuestJournalFader.Menu_mc.SystemFader.Page_mc";
std::shared_ptr<RE::GFxMovieView> Ready(const std::string& path = primary) {
    auto m = std::make_shared<RE::GFxMovieView>();
    m->variables[path].display = true;
    m->variables[path + ".SettingsList"].display = true;
    RE::ui = {true, std::make_shared<RE::Menu>(RE::Menu{m})};
    return m;
}
int main() {
    auto m = Ready(); QueueJournalSystemFocusRepair(m, 3); Check(invokes == 0);
    SKSE::tasks.Run(); Check(invokes == 1);
    for (int failure = 0; failure < 11; ++failure) {
        m = Ready(); QueueJournalSystemFocusRepair(m, 3); int before = invokes;
        switch (failure) {
        case 0: CancelJournalSystemFocusRepair(); break;
        case 1: RE::ui.open = false; break;
        case 2: RE::ui.menu.reset(); break;
        case 3: RE::ui.menu->uiMovie = std::make_shared<RE::GFxMovieView>(); break;
        case 4: m->state = 13; break;
        case 5: m->tab = 0; break;
        case 6: m->interactive = false; break;
        case 7: m->variables.erase(primary + ".SettingsList"); break;
        case 8: m->variables[primary].flags["bMenuClosing"] = true; break;
        case 9: m->variables[primary].flags["pageWasEnded"] = true; break;
        case 10: m->variables[primary].flags["bSavingSettings"] = true; break;
        }
        SKSE::tasks.Run(); Check(invokes == before);
    }
    m = Ready(); QueueJournalSystemFocusRepair(m, 3); QueueJournalSystemFocusRepair(m, 3);
    int before = invokes; SKSE::tasks.Run(); Check(invokes == before + 1);
    m = Ready("_root.Menu_mc.SystemFader.Page_mc");
    Check(RepairJournalSystemFocus(*m, 3));
    Check(!RepairJournalSystemFocus(*m, 13)); Check(!RepairJournalSystemFocus(*m, -1));
    std::cout << "PASS: " << checks << " Journal focus dispatch and lifecycle checks\n";
}
