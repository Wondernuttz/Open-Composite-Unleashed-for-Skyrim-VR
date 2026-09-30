#include <array>
#include <cmath>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

static int checks = 0;
void Check(bool ok) { ++checks; if (!ok) throw std::runtime_error("check " + std::to_string(checks)); }
namespace RE {
struct Object;
struct GFxValue {
    enum Type { Undefined, Number, Boolean, Obj } type = Undefined;
    double number = 0; bool boolean = false;
    std::shared_ptr<Object> object;
    bool IsObject() const { return type == Obj; }
    bool IsDisplayObject() const { return IsObject(); }
    bool IsNumber() const { return type == Number; }
    bool IsBool() const { return type == Boolean; }
    double GetNumber() const { return number; }
    bool GetBool() const { return boolean; }
    void SetNumber(double n) { type = Number; number = n; }
    void SetBoolean(bool b) { type = Boolean; boolean = b; }
    bool GetMember(const char*, GFxValue*);
    bool SetMember(const char*, const GFxValue&);
    bool Invoke(const char*, GFxValue*, GFxValue*, unsigned);
    bool operator==(const GFxValue& v) const { return type == v.type && object == v.object; }
};
struct Object {
    std::map<std::string, GFxValue> members;
    std::function<bool(const std::string&, GFxValue*, GFxValue*, unsigned)> invoke;
    double left = 0, top = 0, right = 0, bottom = 0;
};
bool GFxValue::GetMember(const char* n, GFxValue* out) {
    if (!object || !object->members.count(n)) return false;
    *out = object->members[n]; return true;
}
bool GFxValue::SetMember(const char* n, const GFxValue& v) {
    if (!object) return false; object->members[n] = v; return true;
}
bool GFxValue::Invoke(const char* n, GFxValue* out, GFxValue* args, unsigned count) {
    return object && object->invoke && object->invoke(n, out, args, count);
}
struct GFxMovieView {
    GFxValue panel;
    bool journalVisible = false;
    float viewportLeft = 40, viewportTop = 20, scale = 2;
    int parked = 0;
    bool GetVariable(GFxValue* out, const char*) { *out = panel; return panel.IsObject(); }
    void NotifyMouseState(float x, float y, unsigned buttons, unsigned) {
        Check(x < 0 && y < 0 && buttons == 0); ++parked;
    }
};
}
RE::GFxValue Number(double n) { RE::GFxValue v; v.SetNumber(n); return v; }
RE::GFxValue Bool(bool b) { RE::GFxValue v; v.SetBoolean(b); return v; }
RE::GFxValue Obj(double l=0, double t=0, double r=0, double b=0) {
    RE::GFxValue v; v.type = RE::GFxValue::Obj; v.object = std::make_shared<RE::Object>();
    v.object->left=l; v.object->top=t; v.object->right=r; v.object->bottom=b;
    v.SetMember("_visible", Bool(true)); return v;
}
bool JournalMainFaderIsInteractive(RE::GFxMovieView& m) { return m.journalVisible; }
bool DisplayObjectIsUsable(RE::GFxValue& v) {
    RE::GFxValue visible, disabled;
    return v.IsObject() && !(v.GetMember("_visible", &visible) && visible.IsBool() && !visible.GetBool()) &&
        !(v.GetMember("disabled", &disabled) && disabled.IsBool() && disabled.GetBool());
}
bool ViewportToMovieRootPoint(RE::GFxMovieView& m, float x, float y, float& rx, float& ry) {
    rx=(x-m.viewportLeft)/m.scale; ry=(y-m.viewportTop)/m.scale;
    return std::isfinite(rx) && std::isfinite(ry);
}
bool DisplayObjectBoundsHitAtRootPoint(RE::GFxMovieView&, RE::GFxValue& v, float x, float y) {
    return v.object && x>=v.object->left && x<=v.object->right && y>=v.object->top && y<=v.object->bottom;
}
#include "../src/MCMLaser.inl"

struct Fixture {
    RE::GFxMovieView movie;
    RE::GFxValue list = Obj(), panel = Obj();
    std::vector<RE::GFxValue> rows;
    int presses=0, lastPress=-1, focusCalls=0, clipCalls=0;
    bool replaceOnHover=false;
    Fixture() {
        movie.panel=panel;
        panel.SetMember("_state", Number(0)); panel.SetMember("_focus", Number(1));
        panel.SetMember("_optionsList", list);
        panel.SetMember("_modList", Obj()); panel.SetMember("_subList", Obj());
        list.SetMember("background", Obj(100,100,500,580));
        list.SetMember("_listIndex", Number(24)); list.SetMember("selectedIndex", Number(0));
        for (int i=0; i<24; ++i) {
            auto row=Obj();
            row.SetMember("enabled", Bool(true)); row.SetMember("itemIndex", Number(i+40));
            row.SetMember("background", Obj(100+(i%2)*200,100+(i/2)*40,295+(i%2)*200,138+(i/2)*40));
            rows.push_back(row);
        }
        list.object->invoke=[this](const std::string& n, RE::GFxValue* out, RE::GFxValue* a, unsigned count) {
            if (n=="getClipByIndex") {
                Check(count==1); ++clipCalls; auto i=static_cast<int>(a[0].GetNumber());
                if (i<0 || i>=static_cast<int>(rows.size())) return false; *out=rows[i]; return true;
            }
            if (n=="onItemRollOver") {
                Check(count==1); list.SetMember("selectedIndex", a[0]); list.SetMember("isMouseDrivenNav", Bool(true));
                panel.SetMember("_focus", Number(1));
                if (replaceOnHover) panel.SetMember("_state", Number(1));
                return true;
            }
            if (n=="onItemPress") {
                Check(count==2 && a[1].GetNumber()==0);
                lastPress=static_cast<int>(list.object->members["selectedIndex"].GetNumber()); ++presses; return true;
            }
            return false;
        };
        panel.object->invoke=[this](const std::string& n, RE::GFxValue*, RE::GFxValue* a, unsigned count) {
            Check(n=="changeFocus" && count==1 && (a[0].GetNumber()==0 || a[0].GetNumber()==1));
            ++focusCalls; return true;
        };
    }
    float X(int i) { return movie.viewportLeft + movie.scale*(120+(i%2)*200); }
    float Y(int i) { return movie.viewportTop + movie.scale*(115+(i/2)*40); }
};
int main() {
    for (float scale : {1.0f, 1.5f, 2.0f}) {
        Fixture f; f.movie.scale=scale;
        for (int i=0;i<24;++i) {
            MCMLaserTarget target; bool area=false;
            Check(ResolveMCMOptionTarget(f.movie,f.X(i),f.Y(i),target,&area));
            Check(area && target.index==40+i);
            Check(ActivateMCMOption(f.movie,f.X(i),f.Y(i)));
            Check(f.lastPress==40+i && f.presses==i+1);
            Check(RestoreMCMControllerFocus(f.movie));
            Check(f.list.object->members["selectedIndex"].GetNumber()==40+i);
            Check(!f.list.object->members["isMouseDrivenNav"].GetBool());
        }
    }
    for (int failure=0; failure<12; ++failure) {
        Fixture f;
        switch(failure) {
        case 0: f.movie.journalVisible=true; break;
        case 1: f.panel.SetMember("_state",Number(7)); break;
        case 2: f.panel.SetMember("_state",Number(1)); break;
        case 3: f.panel.SetMember("_bRemapMode",Bool(true)); break;
        case 4: f.list.SetMember("disableInput",Bool(true)); break;
        case 5: f.list.SetMember("disableSelection",Bool(true)); break;
        case 6: f.list.SetMember("isListAnimating",Bool(true)); break;
        case 7: f.rows[23].SetMember("enabled",Bool(false)); break;
        case 8: f.rows[23].SetMember("_visible",Bool(false)); break;
        case 9: f.list.SetMember("_listIndex",Number(1000000)); break;
        case 10: f.list.SetMember("_listIndex",Number(NAN)); break;
        case 11: f.replaceOnHover=true; break;
        }
        Check(!ActivateMCMOption(f.movie,f.X(23),f.Y(23))); Check(f.presses==0);
        if(failure<4) { Check(!RestoreMCMControllerFocus(f.movie)); Check(f.movie.parked==0 && f.focusCalls==0); }
    }
    Fixture f; MCMLaserTarget target; bool area=false;
    f.rows[23].SetMember("enabled",Bool(false));
    Check(!ResolveMCMOptionTarget(f.movie,f.X(23),f.Y(23),target,&area) && area);
    Check(!ActivateMCMOption(f.movie,-1,-1)); Check(f.presses==0);
    f.panel.SetMember("_focus",Number(0)); Check(RestoreMCMControllerFocus(f.movie));
    Check(f.panel.object->members["_focus"].GetNumber()==0);
    std::cout << "PASS: " << checks << " MCM targeting and controller handoff checks\n";
}
