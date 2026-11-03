// TrayPanelProbe — opens the tray panel in a chosen state and saves a picture of
// it, so the panel's look can be checked without a controller or a tray click.
//
//   TrayPanelProbe <state> <out.bmp>
//
// States: manual-on, manual-off, auto, nocontroller, devicedisabled.
// Add "hover" as a third argument to park the pointer on Customize Controls.
#define NOMINMAX
#include <Windows.h>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <thread>
#include <UIAutomation.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include "app/TrayPanel.h"
#include "app/TrayPanelA11y.h"

namespace {

TrayPanel::Model ModelFor(const char* state) {
    TrayPanel::Model m;
    m.connected = true;
    m.manual = true;
    m.controlOn = true;
    m.battery = 82;
    m.startup = true;
    m.enableOnLaunch = true;
    m.notifications = true;
    if (!strcmp(state, "manual-off")) {
        m.controlOn = false;
    } else if (!strcmp(state, "auto")) {
        m.manual = false;
        m.mode = 1;
        m.battery = 64;
        m.enableOnLaunch = false;
    } else if (!strcmp(state, "nocontroller")) {
        m.connected = false;
        m.controlOn = false;
        m.battery = -1;
    } else if (!strcmp(state, "devicedisabled")) {
        m.deviceDisabled = true;
        m.connected = false;
        m.controlOn = false;
        m.battery = -1;
    }
    return m;
}

void Pump(DWORD ms) {
    const ULONGLONG end = GetTickCount64() + ms;
    while (GetTickCount64() < end) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    }
}

bool SaveBmp(const wchar_t* path, int x, int y, int w, int h) {
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, dib);
    BitBlt(mem, 0, 0, w, h, screen, x, y, SRCCOPY | CAPTUREBLT);

    BITMAPFILEHEADER fh{};
    fh.bfType = 0x4d42;
    const DWORD size = static_cast<DWORD>(w) * static_cast<DWORD>(h) * 4;
    fh.bfOffBits = sizeof(fh) + sizeof(bi.bmiHeader);
    fh.bfSize = fh.bfOffBits + size;
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool ok = f != INVALID_HANDLE_VALUE;
    if (ok) {
        DWORD wr = 0;
        WriteFile(f, &fh, sizeof(fh), &wr, nullptr);
        WriteFile(f, &bi.bmiHeader, sizeof(bi.bmiHeader), &wr, nullptr);
        WriteFile(f, bits, size, &wr, nullptr);
        CloseHandle(f);
    }
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return ok;
}

int g_failures = 0;
void Check(bool ok, const char* what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

// Drives the real panel window with the messages a mouse and keyboard would
// send, and checks what comes out: which action fires, whether the panel stays
// up for the settings that should keep it there and goes for the ones that
// should not, and that the face follows the model.
int SelfTest() {
    std::printf("TrayPanel self-test\n");
    TrayPanel::Model model;
    model.connected = true; model.manual = true; model.controlOn = false;
    model.startup = true; model.enableOnLaunch = true; model.notifications = true;

    std::vector<TrayPanel::Action> seen;
    bool openInsideHandler = true;
    TrayPanel panel;
    auto show = [&] {
        panel.Show(GetModuleHandleW(nullptr), POINT{ 1500, 900 }, [&] { return model; },
                   [&](TrayPanel::Action a) {
                       seen.push_back(a);
                       openInsideHandler = panel.IsOpen();
                       if (a == TrayPanel::Action::ToggleControl) model.controlOn = !model.controlOn;
                       if (a == TrayPanel::Action::ModeGame) model.mode = 2;
                       if (a == TrayPanel::Action::ModeSteam) { model.mode = 1; model.manual = false; }
                       if (a == TrayPanel::Action::Startup) model.startup = !model.startup;
                   });
        Pump(150);
    };
    auto click = [&](float x, float y) {
        HWND h = FindWindowW(L"SteamlessTrayPanel", nullptr);
        const float s = static_cast<float>(GetDpiForWindow(h)) / 96.f;
        SendMessageW(h, WM_LBUTTONUP, 0, MAKELPARAM(static_cast<int>(x * s), static_cast<int>(y * s)));
    };
    auto key = [&](WPARAM vk) {
        HWND h = FindWindowW(L"SteamlessTrayPanel", nullptr);
        SendMessageW(h, WM_KEYDOWN, vk, 0);
    };

    show();
    Check(panel.IsOpen(), "opens");

    click(305, 34);   // master switch
    Check(!seen.empty() && seen.back() == TrayPanel::Action::ToggleControl, "master switch sends ToggleControl");
    Check(panel.IsOpen(), "panel stays open after a setting change");

    click(210, 107);  // "In game" segment
    Check(seen.back() == TrayPanel::Action::ModeGame, "In game segment sends ModeGame");
    const size_t before = seen.size();
    click(210, 107);  // already selected
    Check(seen.size() == before, "choosing the selected mode again does nothing");

    click(170, 205);  // Customize Controls
    Check(seen.back() == TrayPanel::Action::Customize, "Customize Controls sends Customize");
    Check(!panel.IsOpen(), "panel closes for Customize");
    Check(!openInsideHandler, "panel is already gone when the handler runs");

    // Leaving Manual removes the Enable on Launch row. The window must glide to
    // the shorter height with its bottom edge where it was, not jump.
    model.mode = 0; model.manual = true;
    show();
    {
        HWND h = FindWindowW(L"SteamlessTrayPanel", nullptr);
        RECT r0{};
        GetWindowRect(h, &r0);
        click(133, 107);  // "Steam" segment
        std::vector<int> heights;
        const ULONGLONG until = GetTickCount64() + 400;
        while (GetTickCount64() < until) {
            Pump(12);
            RECT r{};
            GetWindowRect(h, &r);
            heights.push_back(r.bottom - r.top);
            if (r.bottom != r0.bottom) { Check(false, "bottom edge stays put while animating"); break; }
        }
        bool monotonic = true, between = false;
        for (size_t i = 1; i < heights.size(); ++i) {
            if (heights[i] > heights[i - 1]) monotonic = false;
            if (heights[i] > 385 && heights[i] < 425) between = true;
        }
        Check(r0.bottom - r0.top == 425, "starts at the 425-high manual size");
        Check(between, "passes through heights between the two sizes");
        Check(monotonic, "height only ever shrinks");
        Check(heights.back() == 385, "settles at the 385-high auto size");
        panel.Close();
    }

    // Mode is now an auto mode, so Enable on Launch must have left the panel.
    model.mode = 2; model.manual = false;
    show();
    HWND hw = FindWindowW(L"SteamlessTrayPanel", nullptr);
    RECT rc{};
    GetWindowRect(hw, &rc);
    const float s = static_cast<float>(GetDpiForWindow(hw)) / 96.f;
    Check(static_cast<int>(std::lround(385 * s)) == rc.bottom - rc.top, "auto mode drops the Enable on Launch row (385 high)");

    // With an assistive technology connected the panel opens with focus already on
    // its first control (so there is somewhere to start reading from), and Down
    // would then move past it. Whether one is connected is up to the machine this
    // runs on, so the test follows what the panel was told rather than assuming.
    const bool preFocused = PanelAccessibility::ClientsListening();
    std::printf("  (a UI Automation client is %s; the panel %s with focus on its first control)\n",
                preFocused ? "listening" : "not listening", preFocused ? "opens" : "does not open");
    if (!preFocused) key(VK_DOWN);
    key(VK_RETURN);
    Check(seen.back() != TrayPanel::Action::ToggleControl || !model.manual, "keyboard: Down then Enter activates the first enabled control");
    const auto first = seen.back();
    Check(first == TrayPanel::Action::ModeManual, "…which is the first mode segment, because the master switch is disabled under auto mode");

    key(VK_ESCAPE);
    Check(!panel.IsOpen(), "Esc closes");

    show();
    SendMessageW(FindWindowW(L"SteamlessTrayPanel", nullptr), WM_ACTIVATE, WA_INACTIVE, 0);
    Check(!panel.IsOpen(), "losing activation closes (click-away)");
    Check(panel.RecentlyDismissed(), "…and is remembered as a dismissal");
    Pump(450);
    Check(!panel.RecentlyDismissed(), "the dismissal is forgotten after the grace period");

    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}


// ---- Accessibility ---------------------------------------------------------
// A real UI Automation client — the API Narrator and Accessibility Insights use —
// run on its own thread against the live panel. It has to be a thread of its own:
// the panel's providers answer on the panel's thread, and a client that blocked
// that thread to ask them anything would wait on itself.
using Microsoft::WRL::ComPtr;

std::mutex                g_eventsMutex;
std::vector<std::wstring> g_events;

void NoteEvent(const std::wstring& s) {
    std::lock_guard<std::mutex> lock(g_eventsMutex);
    g_events.push_back(s);
}
size_t CountEvents(const std::wstring& prefix) {
    std::lock_guard<std::mutex> lock(g_eventsMutex);
    size_t n = 0;
    for (const auto& e : g_events) if (e.rfind(prefix, 0) == 0) ++n;
    return n;
}

std::wstring Str(BSTR b) {
    std::wstring s = b ? b : L"";
    SysFreeString(b);
    return s;
}
std::wstring NameOf(IUIAutomationElement* e) {
    BSTR b = nullptr;
    if (FAILED(e->get_CurrentName(&b))) return L"<unavailable>";
    return Str(b);
}

class PropertyHandler final : public Microsoft::WRL::RuntimeClass<
        Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IUIAutomationPropertyChangedEventHandler> {
public:
    HRESULT STDMETHODCALLTYPE HandlePropertyChangedEvent(IUIAutomationElement* sender, PROPERTYID id, VARIANT) override {
        NoteEvent(L"prop:" + std::to_wstring(id) + L":" + NameOf(sender));
        return S_OK;
    }
};
class FocusHandler final : public Microsoft::WRL::RuntimeClass<
        Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IUIAutomationFocusChangedEventHandler> {
public:
    HRESULT STDMETHODCALLTYPE HandleFocusChangedEvent(IUIAutomationElement* sender) override {
        NoteEvent(L"focus:" + NameOf(sender));
        return S_OK;
    }
};
class StructureHandler final : public Microsoft::WRL::RuntimeClass<
        Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IUIAutomationStructureChangedEventHandler> {
public:
    HRESULT STDMETHODCALLTYPE HandleStructureChangedEvent(IUIAutomationElement*, StructureChangeType, SAFEARRAY*) override {
        NoteEvent(L"structure");
        return S_OK;
    }
};

struct Client {
    ComPtr<IUIAutomation>           uia;
    ComPtr<IUIAutomationTreeWalker> walker;

    std::vector<ComPtr<IUIAutomationElement>> Children(IUIAutomationElement* parent) {
        std::vector<ComPtr<IUIAutomationElement>> out;
        ComPtr<IUIAutomationElement> child;
        walker->GetFirstChildElement(parent, child.GetAddressOf());
        while (child) {
            out.push_back(child);
            ComPtr<IUIAutomationElement> next;
            walker->GetNextSiblingElement(child.Get(), next.GetAddressOf());
            child = next;
        }
        return out;
    }
    // Depth first, so an element inside the mode group is found as readily as one
    // outside it.
    ComPtr<IUIAutomationElement> Find(IUIAutomationElement* parent, const std::wstring& name) {
        for (auto& c : Children(parent)) {
            if (NameOf(c.Get()) == name) return c;
            if (auto inner = Find(c.Get(), name)) return inner;
        }
        return nullptr;
    }
};

CONTROLTYPEID TypeOf(IUIAutomationElement* e) {
    CONTROLTYPEID t = 0;
    e->get_CurrentControlType(&t);
    return t;
}
bool IsOn(IUIAutomationElement* e) {
    ComPtr<IUIAutomationTogglePattern> p;
    if (FAILED(e->GetCurrentPatternAs(UIA_TogglePatternId, IID_PPV_ARGS(&p))) || !p) return false;
    ToggleState s = ToggleState_Off;
    p->get_CurrentToggleState(&s);
    return s == ToggleState_On;
}
bool IsSelectedItem(IUIAutomationElement* e) {
    ComPtr<IUIAutomationSelectionItemPattern> p;
    if (FAILED(e->GetCurrentPatternAs(UIA_SelectionItemPatternId, IID_PPV_ARGS(&p))) || !p) return false;
    BOOL b = FALSE;
    p->get_CurrentIsSelected(&b);
    return b != FALSE;
}
bool IsEnabledElement(IUIAutomationElement* e) {
    BOOL b = FALSE;
    e->get_CurrentIsEnabled(&b);
    return b != FALSE;
}
bool Press(IUIAutomationElement* e, PATTERNID pattern) {
    if (pattern == UIA_InvokePatternId) {
        ComPtr<IUIAutomationInvokePattern> p;
        return SUCCEEDED(e->GetCurrentPatternAs(pattern, IID_PPV_ARGS(&p))) && p && SUCCEEDED(p->Invoke());
    }
    if (pattern == UIA_TogglePatternId) {
        ComPtr<IUIAutomationTogglePattern> p;
        return SUCCEEDED(e->GetCurrentPatternAs(pattern, IID_PPV_ARGS(&p))) && p && SUCCEEDED(p->Toggle());
    }
    ComPtr<IUIAutomationSelectionItemPattern> p;
    return SUCCEEDED(e->GetCurrentPatternAs(pattern, IID_PPV_ARGS(&p))) && p && SUCCEEDED(p->Select());
}

void RunAccessibilityClient(HWND hwnd, const std::vector<TrayPanel::Action>& seen, std::atomic<bool>& done) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Client c;
    if (FAILED(CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&c.uia)))) {
        Check(false, "UI Automation is available");
        done = true;
        return;
    }
    c.uia->get_RawViewWalker(&c.walker);

    ComPtr<IUIAutomationElement> root;
    c.uia->ElementFromHandle(hwnd, &root);
    Check(root != nullptr, "the panel's window has an automation element");
    if (!root) { done = true; return; }

    // Listening first: the panel only raises events when somebody is subscribed.
    PROPERTYID watched[] = { UIA_NamePropertyId, UIA_ToggleToggleStatePropertyId,
                             UIA_SelectionItemIsSelectedPropertyId, UIA_IsEnabledPropertyId };
    c.uia->AddPropertyChangedEventHandlerNativeArray(root.Get(), TreeScope_Subtree, nullptr,
        Microsoft::WRL::Make<PropertyHandler>().Get(), watched, 4);
    c.uia->AddFocusChangedEventHandler(nullptr, Microsoft::WRL::Make<FocusHandler>().Get());
    c.uia->AddStructureChangedEventHandler(root.Get(), TreeScope_Subtree, nullptr,
        Microsoft::WRL::Make<StructureHandler>().Get());
    Sleep(300);

    // ---- what it describes ----
    Check(NameOf(root.Get()) == L"SteamlessController", "the panel is named");
    Check(TypeOf(root.Get()) == UIA_PaneControlTypeId, "the panel is a pane");

    struct Want { const wchar_t* name; CONTROLTYPEID type; };
    const Want expected[] = {
        { L"Active, battery 82 percent", UIA_TextControlTypeId },
        { L"Steamless Mode",             UIA_CheckBoxControlTypeId },
        { L"Control mode",               UIA_GroupControlTypeId },
        { L"Customize Controls",         UIA_ButtonControlTypeId },
        { L"Start with Windows",         UIA_CheckBoxControlTypeId },
        { L"Enable on Launch",           UIA_CheckBoxControlTypeId },
        { L"Show Notifications",         UIA_CheckBoxControlTypeId },
        { L"Event log",                  UIA_ButtonControlTypeId },
        { L"Exit",                       UIA_ButtonControlTypeId },
    };
    auto top = c.Children(root.Get());
    bool same = top.size() == std::size(expected);
    for (size_t i = 0; same && i < top.size(); ++i)
        same = NameOf(top[i].Get()) == expected[i].name && TypeOf(top[i].Get()) == expected[i].type;
    Check(same, "reads in order: status, switch, mode group, Customize, three switches, Event log, Exit");

    auto group = c.Find(root.Get(), L"Control mode");
    auto modes = group ? c.Children(group.Get()) : std::vector<ComPtr<IUIAutomationElement>>{};
    const wchar_t* modeNames[] = { L"Manual", L"Steam", L"In game", L"Profile" };
    bool radios = modes.size() == 4;
    for (size_t i = 0; radios && i < 4; ++i)
        radios = NameOf(modes[i].Get()) == modeNames[i] && TypeOf(modes[i].Get()) == UIA_RadioButtonControlTypeId;
    Check(radios, "the mode group holds four radio buttons");
    Check(radios && IsSelectedItem(modes[0].Get()) && !IsSelectedItem(modes[1].Get()), "Manual is the selected mode");
    if (radios) {
        VARIANT pos{};
        modes[2]->GetCurrentPropertyValue(UIA_PositionInSetPropertyId, &pos);
        VARIANT size{};
        modes[2]->GetCurrentPropertyValue(UIA_SizeOfSetPropertyId, &size);
        Check(pos.vt == VT_I4 && pos.lVal == 3 && size.vt == VT_I4 && size.lVal == 4, "'In game' knows it is 3 of 4");
        BSTR help = nullptr;
        modes[0]->get_CurrentHelpText(&help);
        Check(Str(help) == L"You turn it on and off.", "a mode explains itself");
    }

    auto master = c.Find(root.Get(), L"Steamless Mode");
    auto startup = c.Find(root.Get(), L"Start with Windows");
    auto customize = c.Find(root.Get(), L"Customize Controls");
    Check(master && IsOn(master.Get()) && IsEnabledElement(master.Get()), "the master switch is on and usable");
    Check(startup && IsOn(startup.Get()), "Start with Windows reads as on");
    if (customize) {
        BSTR id = nullptr;
        customize->get_CurrentAutomationId(&id);
        Check(Str(id) == L"CustomizeControls", "controls carry stable automation ids");

        RECT r{};
        customize->get_CurrentBoundingRectangle(&r);
        RECT win{};
        GetWindowRect(hwnd, &win);
        Check(r.right > r.left && r.left >= win.left && r.right <= win.right && r.top >= win.top && r.bottom <= win.bottom,
              "bounds fall inside the panel on screen");
        ComPtr<IUIAutomationElement> atPoint;
        c.uia->ElementFromPoint(POINT{ (r.left + r.right) / 2, (r.top + r.bottom) / 2 }, &atPoint);
        Check(atPoint && NameOf(atPoint.Get()) == L"Customize Controls", "hit testing finds the control under a point");
    }

    // ---- what it can do ----
    if (startup) {
        Check(Press(startup.Get(), UIA_TogglePatternId), "toggling a switch succeeds");
        Check(!seen.empty() && seen.back() == TrayPanel::Action::Startup, "...and reaches the panel as Start with Windows");
        Sleep(400);
        Check(!IsOn(startup.Get()), "the switch reads as off afterwards");
        Check(CountEvents(L"prop:" + std::to_wstring(UIA_ToggleToggleStatePropertyId)) > 0, "...and a screen reader is told");
    }

    if (modes.size() == 4) {
        Check(Press(modes[1].Get(), UIA_SelectionItemPatternId), "choosing a mode succeeds");
        Check(seen.back() == TrayPanel::Action::ModeSteam, "...and reaches the panel as Steam");
        Sleep(600);  // longer than the row's ease-out
        Check(IsSelectedItem(modes[1].Get()) && !IsSelectedItem(modes[0].Get()), "Steam is now the selected mode");
        Check(!c.Find(root.Get(), L"Enable on Launch"), "Enable on Launch has left the tree under an auto mode");
        auto m2 = c.Find(root.Get(), L"Steamless Mode");
        Check(m2 && !IsEnabledElement(m2.Get()), "the master switch reads as unavailable under an auto mode");
        Check(CountEvents(L"structure") > 0, "...and a screen reader is told the structure changed");
        Check(CountEvents(L"prop:" + std::to_wstring(UIA_SelectionItemIsSelectedPropertyId)) > 0, "...and that the selection moved");
    }

    // ---- focus ----
    auto log = c.Find(root.Get(), L"Event log");
    if (log) {
        const size_t before = CountEvents(L"focus:");
        log->SetFocus();
        Sleep(300);
        BOOL has = FALSE;
        log->get_CurrentHasKeyboardFocus(&has);
        Check(has != FALSE, "asking for focus on a control gives it");
        Check(CountEvents(L"focus:") > before, "...and a screen reader is told where focus went");
    }

    // ---- going away ----
    if (customize) {
        Check(Press(customize.Get(), UIA_InvokePatternId), "pressing a button succeeds");
        Check(seen.back() == TrayPanel::Action::Customize, "...and reaches the panel as Customize");
        Sleep(400);
        Check(NameOf(root.Get()) == L"<unavailable>", "once the panel has closed its elements report that they are gone");
    }
    done = true;
}

int AccessibilityTest() {
    std::printf("TrayPanel accessibility test\n");
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    TrayPanel::Model model;
    model.connected = true; model.manual = true; model.controlOn = true; model.battery = 82;
    model.startup = true; model.enableOnLaunch = true; model.notifications = true;

    std::vector<TrayPanel::Action> seen;
    TrayPanel panel;
    panel.Show(GetModuleHandleW(nullptr), POINT{ 1500, 900 }, [&] { return model; },
               [&](TrayPanel::Action a) {
                   seen.push_back(a);
                   if (a == TrayPanel::Action::Startup) model.startup = !model.startup;
                   if (a == TrayPanel::Action::ModeSteam) { model.mode = 1; model.manual = false; }
               });
    Pump(300);
    HWND hwnd = FindWindowW(L"SteamlessTrayPanel", nullptr);
    if (!hwnd) { std::printf("  FAIL  panel did not open\n"); return 1; }

    std::atomic<bool> done{ false };
    std::thread client(RunAccessibilityClient, hwnd, std::cref(seen), std::ref(done));
    // The panel's thread has to keep pumping while the client works, or its
    // providers could never answer.
    while (!done) Pump(10);
    client.join();

    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && !strcmp(argv[1], "a11ytest")) {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        return AccessibilityTest();
    }
    if (argc == 2 && !strcmp(argv[1], "selftest")) {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        return SelfTest();
    }
    if (argc < 3) {
        std::printf("usage: TrayPanelProbe <manual-on|manual-off|auto|nocontroller|devicedisabled> <out.bmp> [hover]\n");
        return 2;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const TrayPanel::Model model = ModelFor(argv[1]);
    const bool hover = argc > 3 && !strcmp(argv[3], "hover");

    TrayPanel panel;
    // Anchored toward the bottom-right of the primary monitor's work area, as a
    // tray click would be.
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    POINT anchor{ work.right - 40, work.bottom };
    panel.Show(GetModuleHandleW(nullptr), anchor, [model] { return model; },
               [](TrayPanel::Action) {});
    Pump(500);

    HWND hwnd = FindWindowW(L"SteamlessTrayPanel", nullptr);
    if (!hwnd) { std::printf("panel did not open\n"); return 1; }

    if (hover) {
        RECT rc{};
        GetWindowRect(hwnd, &rc);
        const float s = static_cast<float>(GetDpiForWindow(hwnd)) / 96.f;
        // Customize Controls sits just under the mode section; aim at its middle.
        const int top = model.deviceDisabled ? 52 : 0;
        SetCursorPos(rc.left + static_cast<int>(170 * s), rc.top + static_cast<int>((top + 68 + 100 + 1 + 36) * s));
        Pump(300);
    }

    RECT rc{};
    GetWindowRect(hwnd, &rc);
    const int pad = 32;
    const int x = rc.left - pad, y = rc.top - pad;
    const int w = (rc.right - rc.left) + pad * 2, h = (rc.bottom - rc.top) + pad * 2;
    std::wstring out(argv[2], argv[2] + std::strlen(argv[2]));
    const bool ok = SaveBmp(out.c_str(), x, y, w, h);
    std::printf("panel %dx%d at %d,%d -> %s\n", rc.right - rc.left, rc.bottom - rc.top, rc.left, rc.top,
                ok ? "saved" : "save failed");
    std::printf("fonts: %ls\n", panel.FontFamilies().c_str());
    panel.Close();
    return ok ? 0 : 1;
}
