#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "TrayPanel.h"
#include "EmbeddedFonts.h"
#include "TrayPanelA11y.h"

#include <d2d1.h>
#include <dwmapi.h>
#include <dwrite.h>
#include <dwrite_1.h>
#include <dwrite_3.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using Action = TrayPanel::Action;
using Model  = TrayPanel::Model;

// Older SDKs predate the Windows 11 window attributes; the values are fixed.
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif

namespace {

constexpr wchar_t    kClassName[]   = L"SteamlessTrayPanel";
constexpr UINT_PTR   kRefreshTimer  = 1;
constexpr UINT_PTR   kAnimTimer     = 2;
constexpr UINT       kAnimFrameMs   = 16;
constexpr ULONGLONG  kAnimMs        = 160;
constexpr UINT       kRefreshMs     = 500;
constexpr ULONGLONG  kDismissGraceMs = 350;

// Layout is in DIPs; the render target's DPI turns them into pixels.
constexpr float kWidth = 344.f;
constexpr float kPad   = 16.f;
constexpr float kEdge  = 12.f;  // gap kept to the work area

// Palette. Shared with the Customize window's v2 tokens.
constexpr UINT32 kBg       = 0x141d28;
constexpr UINT32 kInset    = 0x0e151d;
constexpr UINT32 kFooter   = 0x101821;
constexpr UINT32 kText     = 0xe6edf3;
constexpr UINT32 kText2    = 0xa9b6c2;
constexpr UINT32 kText3    = 0x8794a1;
constexpr UINT32 kAccent   = 0x66c0f4;
constexpr UINT32 kOn       = 0x7fc241;
constexpr UINT32 kOff      = 0x3a4757;
constexpr UINT32 kIdle     = 0x6b7785;
constexpr UINT32 kWarn     = 0xe0b04a;
constexpr UINT32 kWarnText = 0xf0d28f;
constexpr UINT32 kBorder   = 0x2b3541;

D2D1_COLOR_F Col(UINT32 rgb, float a = 1.f) { return D2D1::ColorF(rgb, a); }

D2D1_RECT_F Inset(const D2D1_RECT_F& r, float d) {
    return D2D1::RectF(r.left + d, r.top + d, r.right - d, r.bottom - d);
}

bool Contains(const D2D1_RECT_F& r, float x, float y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

// Actions that leave the panel behind: they open something else, or end the
// process. The rest change a setting and the panel stays up to show it.
bool ClosesPanel(Action a) {
    switch (a) {
    case Action::Customize:
    case Action::OpenLog:
    case Action::Exit:
    case Action::EnableDevice:
        return true;
    default:
        return false;
    }
}

const wchar_t* ModeLabel(int i) {
    static const wchar_t* const labels[4] = { L"Manual", L"Steam", L"In game", L"Profile" };
    return labels[i];
}

const wchar_t* ModeCaption(int mode) {
    switch (mode) {
    case 1:  return L"Off while Steam is running.";
    case 2:  return L"Off only while you are in a Steam game.";
    case 3:  return L"Off unless a game profile is running.";
    default: return L"You turn it on and off.";
    }
}

const wchar_t* RowLabel(Action a) {
    switch (a) {
    case Action::Startup:        return L"Start with Windows";
    case Action::EnableOnLaunch: return L"Enable on Launch";
    default:                     return L"Show Notifications";
    }
}

// Ids for the things a screen reader is told about that are not controls. A
// control's id is its Action's number, which is small, so these sit well above it.
constexpr int kA11yStatus = 100;
constexpr int kA11yBanner = 101;
constexpr int kA11yModes  = 102;

// Stable names for each control, for tools that find an element by id rather than
// by what it is called, which can change.
const wchar_t* AutomationId(Action a) {
    switch (a) {
    case Action::ToggleControl:  return L"MasterSwitch";
    case Action::ModeManual:     return L"Mode.Manual";
    case Action::ModeSteam:      return L"Mode.Steam";
    case Action::ModeGame:       return L"Mode.InGame";
    case Action::ModeProfile:    return L"Mode.Profile";
    case Action::Customize:      return L"CustomizeControls";
    case Action::Startup:        return L"StartWithWindows";
    case Action::EnableOnLaunch: return L"EnableOnLaunch";
    case Action::Notifications:  return L"ShowNotifications";
    case Action::OpenLog:        return L"EventLog";
    case Action::Exit:           return L"Exit";
    case Action::EnableDevice:   return L"ReenableDevice";
    }
    return L"";
}

std::wstring StatusText(const Model& m) {
    if (m.deviceDisabled) return L"Controller unavailable";
    if (!m.connected)     return L"No controller detected";
    if (!m.manual)        return m.controlOn ? L"On · Auto Mode" : L"Off · Auto Mode";
    return m.controlOn ? L"Active" : L"Off";
}

struct Hit {
    D2D1_RECT_F r{};
    Action      a = Action::Exit;
    bool        enabled = true;
    float       radius = 9.f;
};

struct Layout {
    float height = 0.f;

    bool        banner = false;
    D2D1_RECT_F bannerR{}, bannerBtn{}, bannerText{};

    D2D1_RECT_F tile{}, mainSwitch{}, title{}, status{};

    D2D1_RECT_F modeLabel{}, segTrack{}, seg[4]{}, caption{};
    float       div1 = 0.f, div2 = 0.f;

    D2D1_RECT_F customize{};

    int         rowCount = 0;
    Action      rowAct[3]{};
    D2D1_RECT_F row[3]{};

    D2D1_RECT_F footer{}, logBtn{}, exitBtn{};
};

}  // namespace

struct TrayPanel::Impl : PanelAccessibility::Host {
    HINSTANCE hInst = nullptr;
    HWND      hwnd  = nullptr;

    Model         model;
    ModelProvider provider;
    ActionHandler handler;

    ComPtr<ID2D1Factory>          d2d;
    ComPtr<IDWriteFactory>        dw;
    ComPtr<ID2D1StrokeStyle>      stroke;
    ComPtr<ID2D1PathGeometry>     pad;     // the app's gamepad glyph, see BuildPadGeometry
    ComPtr<ID2D1HwndRenderTarget> rt;
    ComPtr<ID2D1SolidColorBrush>  brush;
    std::map<unsigned, ComPtr<IDWriteTextFormat>> formats;
    std::wstring sans, mono;
    // The fonts built into the exe, and the collection that holds them. Null on a
    // Windows too old to build one, in which case the system's fonts are used.
    ComPtr<IDWriteInMemoryFontFileLoader> fontLoader;
    ComPtr<IDWriteFontCollection>         bundled;
    IDWriteFontCollection* sansCollection = nullptr;  // null means the system's
    IDWriteFontCollection* monoCollection = nullptr;

    ~Impl() {
        // The loader is registered with a factory that can outlive this object.
        ComPtr<IDWriteFactory5> f5;
        if (dw && fontLoader && SUCCEEDED(dw.As(&f5)))
            f5->UnregisterFontFileLoader(fontLoader.Get());
    }

    Layout           lay;
    std::vector<Hit> hits;
    int   hover = -1, focus = -1;
    // Whether the focus is drawn. Focus can exist without being shown: with an
    // assistive technology running it starts on the first control, so there is a
    // place to start reading from, but a mouse user is not shown a ring they never
    // asked for. It appears once somebody navigates.
    bool  focusVisible = false;
    float scale = 1.f;
    bool  closing = false;   // we are the ones closing it, so not a dismissal
    bool  rounded = false;   // DWM is rounding and outlining the window for us
    bool  tracking = false;
    ULONGLONG dismissedAt = 0;

    // How present the Enable on Launch row is, 0 (gone) to 1 (full height).
    // Changing control mode adds or removes that row, and the window is its
    // height; snapping between the two sizes read as the panel jumping. So the
    // row eases in and out instead, and the window follows it a frame at a time.
    float     launchT = 1.f;
    float     animFrom = 1.f, animTo = 1.f;
    ULONGLONG animStart = 0;

    // What a screen reader sees; made with the window and gone with it.
    std::unique_ptr<PanelAccessibility> a11y;

    // ---- accessibility ----------------------------------------------------
    // The panel's side of PanelAccessibility::Host. A screen reader can press
    // things and move focus; both go through the same paths the mouse and
    // keyboard do.
    HWND  Window() const override { return hwnd; }
    float Scale() const override { return scale; }
    void  Invoke(int id) override {
        const int i = HitIndexFor(static_cast<Action>(id));
        if (i >= 0) Activate(i);
    }
    void  SetFocus(int id) override {
        const int i = HitIndexFor(static_cast<Action>(id));
        if (i < 0 || !hits[i].enabled) return;
        focus = i;
        focusVisible = true;  // a screen reader moved it: show where, for anyone looking
        PublishAccess();
        if (hwnd) InvalidateRect(hwnd, nullptr, FALSE);
    }

    // Describes what is on screen right now, in the order it reads. Everything a
    // screen reader is told comes from here, so it has to be kept in step with
    // Paint(): a thing drawn but not listed is invisible to it.
    A11yTree BuildAccessTree() const {
        using Kind = A11yElement::Kind;
        A11yTree t;
        t.name = L"SteamlessController";
        const Model& m = model;

        auto place = [](A11yElement& e, const D2D1_RECT_F& r) {
            e.left = r.left; e.top = r.top; e.right = r.right; e.bottom = r.bottom;
        };
        auto text = [&](int id, const wchar_t* automationId, std::wstring name, const D2D1_RECT_F& r) {
            A11yElement e;
            e.id = id; e.kind = Kind::Text; e.name = std::move(name); e.automationId = automationId;
            place(e, r);
            t.elements.push_back(std::move(e));
        };
        // A control that is also a hit target: its bounds and whether it can be used
        // come from there, so they cannot disagree with what the mouse does.
        auto control = [&](Action a, Kind kind, const wchar_t* name, const std::wstring& help,
                           bool on, int parent, int position, int size) {
            const int i = HitIndexFor(a);
            if (i < 0) return;
            A11yElement e;
            e.id = static_cast<int>(a); e.parent = parent; e.kind = kind;
            e.name = name; e.help = help; e.automationId = AutomationId(a);
            e.enabled = hits[i].enabled; e.on = on; e.position = position; e.size = size;
            place(e, hits[i].r);
            t.elements.push_back(std::move(e));
        };

        if (lay.banner) {
            text(kA11yBanner, L"Banner", L"Controller device is switched off in Windows.", lay.bannerText);
            control(Action::EnableDevice, Kind::Button, L"Re-enable controller device", L"", false, -1, 0, 0);
        }

        // Battery belongs to the status line: it is shown there, and a screen reader
        // has no other place to find it.
        std::wstring status = StatusText(m);
        if (m.battery >= 0) status += L", battery " + std::to_wstring(m.battery) + L" percent";
        text(kA11yStatus, L"Status", status, lay.status);

        // The switch says why it cannot be used when it cannot: the same line the
        // header shows under the title.
        control(Action::ToggleControl, Kind::Toggle, L"Steamless Mode", StatusText(m), m.controlOn, -1, 0, 0);

        {
            A11yElement g;
            g.id = kA11yModes; g.kind = Kind::Group; g.name = L"Control mode";
            g.help = ModeCaption(m.mode); g.automationId = L"ControlMode";
            place(g, lay.segTrack);
            t.elements.push_back(std::move(g));
        }
        static const Action modeActs[4] = { Action::ModeManual, Action::ModeSteam, Action::ModeGame, Action::ModeProfile };
        for (int i = 0; i < 4; ++i)
            control(modeActs[i], Kind::Radio, ModeLabel(i), ModeCaption(i), m.mode == i, kA11yModes, i + 1, 4);

        control(Action::Customize, Kind::Button, L"Customize Controls",
                L"Bindings, trackpads, triggers, sticks", false, -1, 0, 0);
        for (int i = 0; i < lay.rowCount; ++i)
            control(lay.rowAct[i], Kind::Toggle, RowLabel(lay.rowAct[i]), L"", RowOn(lay.rowAct[i]), -1, 0, 0);
        control(Action::OpenLog, Kind::Button, L"Event log", L"", false, -1, 0, 0);
        control(Action::Exit, Kind::Button, L"Exit", L"", false, -1, 0, 0);

        t.focusId = (focus >= 0 && focus < static_cast<int>(hits.size())) ? static_cast<int>(hits[focus].a) : -1;
        return t;
    }
    void PublishAccess() {
        if (a11y) a11y->Update(BuildAccessTree());
    }

    // ---- setup ------------------------------------------------------------

    bool EnsureFactories() {
        if (d2d && dw) return true;
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.ReleaseAndGetAddressOf())))
            return false;
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown**>(dw.ReleaseAndGetAddressOf()))))
            return false;

        D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
        d2d->CreateStrokeStyle(sp, nullptr, 0, stroke.ReleaseAndGetAddressOf());
        BuildPadGeometry();

        LoadBundledFonts();

        // Barlow and JetBrains Mono from the exe when they could be loaded, so the
        // panel matches the Customize window on every machine. If they could not
        // be (a Windows older than 10 1703 has no in-memory font loading), fall
        // back to Barlow if it happens to be installed, then to what ships with
        // Windows.
        ComPtr<IDWriteFontCollection> sys;
        dw->GetSystemFontCollection(sys.ReleaseAndGetAddressOf());
        auto have = [](IDWriteFontCollection* c, const wchar_t* family) {
            UINT32 idx = 0; BOOL exists = FALSE;
            return c && SUCCEEDED(c->FindFamilyName(family, &idx, &exists)) && exists;
        };
        if (have(bundled.Get(), L"Barlow")) {
            sans = L"Barlow";
            sansCollection = bundled.Get();
        } else {
            sans = have(sys.Get(), L"Barlow")                 ? L"Barlow"
                 : have(sys.Get(), L"Segoe UI Variable Text") ? L"Segoe UI Variable Text"
                                                               : L"Segoe UI";
        }
        if (have(bundled.Get(), L"JetBrains Mono")) {
            mono = L"JetBrains Mono";
            monoCollection = bundled.Get();
        } else {
            mono = have(sys.Get(), L"Cascadia Mono") ? L"Cascadia Mono" : L"Consolas";
        }
        return true;
    }

    // Builds a font collection out of the embedded TrueType data. The data is
    // never copied: the loader hands DirectWrite pointers into the resource
    // section, which stays mapped for the life of the process.
    void LoadBundledFonts() {
        ComPtr<IDWriteFactory5> f5;
        if (FAILED(dw.As(&f5))) return;
        ComPtr<IDWriteInMemoryFontFileLoader> loader;
        if (FAILED(f5->CreateInMemoryFontFileLoader(loader.GetAddressOf()))) return;
        if (FAILED(f5->RegisterFontFileLoader(loader.Get()))) return;
        fontLoader = loader;  // from here on the destructor unregisters it

        ComPtr<IDWriteFontSetBuilder1> builder;
        if (FAILED(f5->CreateFontSetBuilder(builder.GetAddressOf()))) return;
        for (size_t i = 0; i < EmbeddedFontCount(); ++i) {
            const void* data = nullptr;
            DWORD size = 0;
            if (!LoadEmbeddedFont(hInst, EmbeddedFontAt(i), data, size)) continue;
            ComPtr<IDWriteFontFile> file;
            if (SUCCEEDED(loader->CreateInMemoryFontFileReference(f5.Get(), data, size, nullptr, file.GetAddressOf())))
                builder->AddFontFile(file.Get());
        }
        ComPtr<IDWriteFontSet> set;
        if (FAILED(builder->CreateFontSet(set.GetAddressOf()))) return;
        ComPtr<IDWriteFontCollection1> collection;
        if (FAILED(f5->CreateFontCollectionFromFontSet(set.Get(), collection.GetAddressOf()))) return;
        bundled = collection;
    }

    // The gamepad silhouette from the tray icons, as a path in a 256-unit box
    // (it spans x 20..236, y 40..202; the ring sits at 128,98). Vector rather
    // than the .ico's bitmap so it stays sharp at any DPI, and so it can be
    // tinted to the tile it is on. The same outline is inlined as an SVG in the
    // Customize window's title bar — change one, change both.
    void BuildPadGeometry() {
        if (FAILED(d2d->CreatePathGeometry(pad.ReleaseAndGetAddressOf()))) return;
        ComPtr<ID2D1GeometrySink> sink;
        if (FAILED(pad->Open(sink.GetAddressOf()))) { pad.Reset(); return; }
        auto P = [](float x, float y) { return D2D1::Point2F(x, y); };
        auto C = [&](float x1, float y1, float x2, float y2, float x3, float y3) {
            sink->AddBezier(D2D1::BezierSegment(P(x1, y1), P(x2, y2), P(x3, y3)));
        };
        sink->BeginFigure(P(78, 44), D2D1_FIGURE_BEGIN_FILLED);
        C(100, 40, 170, 40, 192, 44);     // top edge
        C(220, 50, 236, 80, 236, 110);    // right shoulder
        C(236, 140, 232, 165, 226, 185);  // right side
        C(222, 198, 212, 202, 204, 198);  // right grip
        C(196, 194, 192, 180, 186, 168);
        C(182, 160, 176, 158, 168, 158);  // notch
        sink->AddLine(P(88, 158));
        C(80, 158, 74, 160, 70, 168);
        C(64, 180, 60, 194, 52, 198);     // left grip
        C(44, 202, 34, 198, 30, 186);
        C(24, 165, 20, 140, 20, 110);     // left side
        C(20, 80, 36, 50, 78, 44);        // left shoulder
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        if (FAILED(sink->Close())) pad.Reset();
    }

    // `width` is the glyph's width in DIPs; it is centred in `box`.
    void IconGamepad(const D2D1_RECT_F& box, float width, UINT32 body, UINT32 ring) {
        if (!pad) return;
        const float s  = width / 216.f;
        const float cx = (box.left + box.right) * 0.5f;
        const float cy = (box.top + box.bottom) * 0.5f;
        rt->SetTransform(D2D1::Matrix3x2F::Scale(s, s) *
                         D2D1::Matrix3x2F::Translation(cx - 128.f * s, cy - 121.f * s));
        brush->SetColor(Col(body));
        rt->FillGeometry(pad.Get(), brush.Get());
        brush->SetColor(Col(ring));
        rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(128.f, 98.f), 25.f, 25.f), brush.Get(), 10.f);
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
    }

    // ---- text -------------------------------------------------------------

    IDWriteTextFormat* Format(float size, DWRITE_FONT_WEIGHT weight, bool useMono,
                              DWRITE_TEXT_ALIGNMENT align) {
        const unsigned key = (static_cast<unsigned>(size * 10.f) << 12) |
                             (static_cast<unsigned>(weight) / 100u << 4) |
                             (useMono ? 8u : 0u) | static_cast<unsigned>(align);
        auto it = formats.find(key);
        if (it != formats.end()) return it->second.Get();

        ComPtr<IDWriteTextFormat> f;
        if (FAILED(dw->CreateTextFormat(useMono ? mono.c_str() : sans.c_str(),
                                        useMono ? monoCollection : sansCollection, weight,
                                        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                        size, L"en-us", f.GetAddressOf())))
            return nullptr;
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        f->SetTextAlignment(align);
        ComPtr<IDWriteInlineObject> ellipsis;
        if (SUCCEEDED(dw->CreateEllipsisTrimmingSign(f.Get(), ellipsis.GetAddressOf()))) {
            DWRITE_TRIMMING t{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
            f->SetTrimming(&t, ellipsis.Get());
        }
        return formats.emplace(key, std::move(f)).first->second.Get();
    }

    void Text(const std::wstring& s, const D2D1_RECT_F& r, float size, DWRITE_FONT_WEIGHT weight,
              UINT32 rgb, float alpha = 1.f, bool useMono = false,
              DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
              float spacing = 0.f, bool wrap = false) {
        IDWriteTextFormat* f = Format(size, weight, useMono, align);
        if (!f || s.empty()) return;
        ComPtr<IDWriteTextLayout> tl;
        if (FAILED(dw->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), f,
                                        r.right - r.left, r.bottom - r.top, tl.GetAddressOf())))
            return;
        if (wrap) tl->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        if (spacing != 0.f) {
            ComPtr<IDWriteTextLayout1> tl1;
            if (SUCCEEDED(tl.As(&tl1)))
                tl1->SetCharacterSpacing(0.f, spacing, 0.f, DWRITE_TEXT_RANGE{ 0, static_cast<UINT32>(s.size()) });
        }
        brush->SetColor(Col(rgb, alpha));
        rt->DrawTextLayout(D2D1::Point2F(r.left, r.top), tl.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
    }

    float TextWidth(const std::wstring& s, float size, DWRITE_FONT_WEIGHT weight) {
        IDWriteTextFormat* f = Format(size, weight, false, DWRITE_TEXT_ALIGNMENT_LEADING);
        if (!f || s.empty()) return 0.f;
        ComPtr<IDWriteTextLayout> tl;
        if (FAILED(dw->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), f, 1000.f, 40.f, tl.GetAddressOf())))
            return 0.f;
        DWRITE_TEXT_METRICS tm{};
        tl->GetMetrics(&tm);
        return tm.widthIncludingTrailingWhitespace;
    }

    // ---- primitives -------------------------------------------------------

    void Fill(const D2D1_RECT_F& r, float radius, UINT32 rgb, float a = 1.f) {
        brush->SetColor(Col(rgb, a));
        if (radius <= 0.f) rt->FillRectangle(r, brush.Get());
        else rt->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush.Get());
    }

    void Outline(const D2D1_RECT_F& r, float radius, UINT32 rgb, float a, float w = 1.f) {
        brush->SetColor(Col(rgb, a));
        rt->DrawRoundedRectangle(D2D1::RoundedRect(Inset(r, w * 0.5f), radius, radius), brush.Get(), w);
    }

    void Dot(float x, float y, float radius, UINT32 rgb, float a = 1.f) {
        brush->SetColor(Col(rgb, a));
        rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), radius, radius), brush.Get());
    }

    void Line(float ox, float oy, float x1, float y1, float x2, float y2, UINT32 rgb, float w = 1.5f) {
        brush->SetColor(Col(rgb));
        rt->DrawLine(D2D1::Point2F(ox + x1, oy + y1), D2D1::Point2F(ox + x2, oy + y2),
                     brush.Get(), w, stroke.Get());
    }

    void Ring(float cx, float cy, float radius, UINT32 rgb, float w = 1.5f) {
        brush->SetColor(Col(rgb));
        rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), brush.Get(), w, stroke.Get());
    }

    // ---- icons (drawn from the same coordinates as the mock-ups) ----------

    void IconSliders(float x, float y, UINT32 rgb) {
        Line(x, y, 3, 6, 12, 6, rgb, 1.6f);   Line(x, y, 16, 6, 19, 6, rgb, 1.6f);
        Line(x, y, 3, 16, 6, 16, rgb, 1.6f);  Line(x, y, 10, 16, 19, 16, rgb, 1.6f);
        Ring(x + 14, y + 6, 2.2f, rgb, 1.6f); Ring(x + 8, y + 16, 2.2f, rgb, 1.6f);
    }
    void IconChevron(float x, float y, UINT32 rgb) {
        Line(x, y, 6, 3, 11, 8, rgb, 1.8f);   Line(x, y, 11, 8, 6, 13, rgb, 1.8f);
    }
    void IconList(float x, float y, UINT32 rgb) {
        Line(x, y, 3, 4, 13, 4, rgb, 1.4f);   Line(x, y, 3, 8, 13, 8, rgb, 1.4f);
        Line(x, y, 3, 12, 9, 12, rgb, 1.4f);
    }
    void IconExit(float x, float y, UINT32 rgb) {
        Line(x, y, 6, 3, 3.5f, 3, rgb, 1.4f);   Line(x, y, 3.5f, 3, 3.5f, 13, rgb, 1.4f);
        Line(x, y, 3.5f, 13, 6, 13, rgb, 1.4f); Line(x, y, 10, 5, 13, 8, rgb, 1.4f);
        Line(x, y, 13, 8, 10, 11, rgb, 1.4f);   Line(x, y, 13, 8, 6.5f, 8, rgb, 1.4f);
    }
    void IconWarn(float x, float y, UINT32 rgb) {
        Line(x, y, 9, 2.5f, 16, 15, rgb, 1.6f); Line(x, y, 16, 15, 2, 15, rgb, 1.6f);
        Line(x, y, 2, 15, 9, 2.5f, rgb, 1.6f);  Line(x, y, 9, 7.5f, 9, 10.5f, rgb, 1.6f);
        Line(x, y, 9, 12.6f, 9, 12.7f, rgb, 1.8f);
    }
    void IconBattery(float x, float y, int percent, UINT32 rgb) {
        Outline(D2D1::RectF(x + 1.5f, y + 4.5f, x + 12.5f, y + 11.5f), 1.6f, rgb, 1.f, 1.4f);
        Line(x, y, 14, 7, 14, 9, rgb, 1.4f);
        const float fill = std::max(1.f, 6.f * static_cast<float>(std::clamp(percent, 0, 100)) / 100.f);
        Fill(D2D1::RectF(x + 3.f, y + 6.f, x + 3.f + fill, y + 10.f), 0.6f, rgb);
    }

    // ---- layout -----------------------------------------------------------

    void AddHit(const D2D1_RECT_F& r, Action a, bool enabled = true, float radius = 9.f) {
        hits.push_back(Hit{ r, a, enabled, radius });
    }

    void BuildLayout() {
        const int  oldFocus = (focus >= 0 && focus < static_cast<int>(hits.size())) ? static_cast<int>(hits[focus].a) : -1;
        const int  oldHover = (hover >= 0 && hover < static_cast<int>(hits.size())) ? static_cast<int>(hits[hover].a) : -1;

        Layout lo;
        hits.clear();
        float y = 0.f;

        lo.banner = model.deviceDisabled;
        if (lo.banner) {
            lo.bannerR    = D2D1::RectF(0, 0, kWidth, 52);
            lo.bannerBtn  = D2D1::RectF(kWidth - 14 - 80, 12, kWidth - 14, 40);
            lo.bannerText = D2D1::RectF(14 + 18 + 10, 6, lo.bannerBtn.left - 10, 46);
            AddHit(lo.bannerBtn, Action::EnableDevice, true, 7.f);
            y = 52.f;
        }

        // Header: tile, title and status, and the master switch.
        lo.tile       = D2D1::RectF(kPad, y + 16, kPad + 38, y + 54);
        lo.mainSwitch = D2D1::RectF(kWidth - kPad - 46, y + 21, kWidth - kPad, y + 47);
        lo.title      = D2D1::RectF(lo.tile.right + 12, y + 16, lo.mainSwitch.left - 12, y + 36);
        lo.status     = D2D1::RectF(lo.title.left, y + 36, lo.mainSwitch.left - 12, y + 54);
        AddHit(lo.mainSwitch, Action::ToggleControl, model.connected && model.manual, 13.f);
        y += 68.f;

        // Control mode.
        lo.modeLabel = D2D1::RectF(kPad, y, kWidth - kPad, y + 12);
        lo.segTrack  = D2D1::RectF(kPad, y + 20, kWidth - kPad, y + 58);
        const float segW = ((lo.segTrack.right - lo.segTrack.left) - 6.f - 6.f) / 4.f;
        static const Action modeActs[4] = { Action::ModeManual, Action::ModeSteam, Action::ModeGame, Action::ModeProfile };
        for (int i = 0; i < 4; ++i) {
            const float x0 = lo.segTrack.left + 3.f + static_cast<float>(i) * (segW + 2.f);
            lo.seg[i] = D2D1::RectF(x0, lo.segTrack.top + 3.f, x0 + segW - (i < 3 ? 2.f : 0.f), lo.segTrack.bottom - 3.f);
            AddHit(lo.seg[i], modeActs[i], true, 8.f);
        }
        lo.caption = D2D1::RectF(kPad, y + 67, kWidth - kPad, y + 84);
        y += 100.f;

        lo.div1 = y;  y += 1.f;
        lo.customize = D2D1::RectF(8, y + 8, kWidth - 8, y + 64);
        AddHit(lo.customize, Action::Customize, true, 11.f);
        y += 72.f;

        lo.div2 = y;  y += 1.f;
        y += 6.f;
        lo.rowAct[lo.rowCount++] = Action::Startup;
        if (model.manual || launchT > 0.f) lo.rowAct[lo.rowCount++] = Action::EnableOnLaunch;
        lo.rowAct[lo.rowCount++] = Action::Notifications;
        for (int i = 0; i < lo.rowCount; ++i) {
            const bool launch = (lo.rowAct[i] == Action::EnableOnLaunch);
            const float h = launch ? 40.f * launchT : 40.f;
            lo.row[i] = D2D1::RectF(8, y, kWidth - 8, y + h);
            // A row on its way out is not something to click.
            if (!launch || model.manual) AddHit(lo.row[i], lo.rowAct[i]);
            y += h;
        }
        y += 6.f;

        lo.footer = D2D1::RectF(0, y, kWidth, y + 51);
        const float logW  = 12.f + 16.f + 8.f + TextWidth(L"Event log", 13.f, DWRITE_FONT_WEIGHT_MEDIUM) + 12.f;
        const float exitW = 12.f + TextWidth(L"Exit", 13.f, DWRITE_FONT_WEIGHT_MEDIUM) + 8.f + 16.f + 12.f;
        lo.logBtn  = D2D1::RectF(8, y + 9, 8 + logW, y + 43);
        lo.exitBtn = D2D1::RectF(kWidth - 8 - exitW, y + 9, kWidth - 8, y + 43);
        AddHit(lo.logBtn, Action::OpenLog, true, 8.f);
        AddHit(lo.exitBtn, Action::Exit, true, 8.f);
        lo.height = y + 51.f;

        lay = lo;

        // The indexes moved if a row appeared or went; carry hover and focus
        // across by what they were on rather than where they were.
        auto find = [&](int act) {
            if (act < 0) return -1;
            for (size_t i = 0; i < hits.size(); ++i)
                if (static_cast<int>(hits[i].a) == act) return static_cast<int>(i);
            return -1;
        };
        focus = find(oldFocus);
        hover = find(oldHover);
        PublishAccess();
    }

    static bool IsModeAction(Action a) {
        return a == Action::ModeManual || a == Action::ModeSteam ||
               a == Action::ModeGame   || a == Action::ModeProfile;
    }

    bool IsActive(const Hit& h) const {
        switch (h.a) {
        case Action::ModeManual:  return model.mode == 0;
        case Action::ModeSteam:   return model.mode == 1;
        case Action::ModeGame:    return model.mode == 2;
        case Action::ModeProfile: return model.mode == 3;
        default: return false;
        }
    }

    bool RowOn(Action a) const {
        switch (a) {
        case Action::Startup:        return model.startup;
        case Action::EnableOnLaunch: return model.enableOnLaunch;
        default:                     return model.notifications;
        }
    }

    int HitIndexFor(Action a) const {
        for (size_t i = 0; i < hits.size(); ++i)
            if (hits[i].a == a) return static_cast<int>(i);
        return -1;
    }

    // ---- painting ---------------------------------------------------------

    bool EnsureTarget() {
        if (rt) return true;
        RECT rc{};
        GetClientRect(hwnd, &rc);
        const D2D1_SIZE_U size = D2D1::SizeU(static_cast<UINT32>(std::max<LONG>(rc.right, 1)),
                                             static_cast<UINT32>(std::max<LONG>(rc.bottom, 1)));
        if (FAILED(d2d->CreateHwndRenderTarget(D2D1::RenderTargetProperties(),
                                               D2D1::HwndRenderTargetProperties(hwnd, size),
                                               rt.ReleaseAndGetAddressOf())))
            return false;
        const float dpi = scale * 96.f;
        rt->SetDpi(dpi, dpi);

        // Grayscale antialiasing, as the Customize window's browser engine uses.
        // ClearType's coloured subpixel fringes look heavy and crunchy on light
        // text over a dark ground at 12 or 13 pixels, and make Barlow, whose
        // letters are close-set, read as squashed.
        rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

        if (FAILED(rt->CreateSolidColorBrush(Col(kText), brush.ReleaseAndGetAddressOf()))) {
            rt.Reset();
            return false;
        }
        return true;
    }

    void DrawSwitch(const D2D1_RECT_F& r, bool on, bool enabled) {
        // A switch that is on but not the user's to flip (an auto mode owns it) goes
        // grey instead of fading: the green accent at partial opacity turns a muddy
        // dark green, where grey says "on, and out of your hands" cleanly. Its knob
        // still sits at the on end, so the state is not lost, and it is a little
        // translucent so it also reads as inactive. Off needs no colour change; it is
        // already neutral, and only dims, further than on does because it has
        // less to lose.
        const UINT32 track = !on ? kOff : enabled ? kOn : kIdle;
        const float  a     = enabled ? 1.f : on ? 0.7f : 0.45f;
        const float h = r.bottom - r.top;
        const float inset = h > 22.f ? 3.f : 2.f;
        const float d = h - inset * 2.f;
        Fill(r, h * 0.5f, track, a);
        const float kx = on ? r.right - inset - d * 0.5f : r.left + inset + d * 0.5f;
        Dot(kx, (r.top + r.bottom) * 0.5f, d * 0.5f, 0xffffff, a);
    }

    void DrawHoverFor(Action act, const D2D1_RECT_F& r, float radius, UINT32 rgb, float a) {
        const int i = HitIndexFor(act);
        if (i >= 0 && i == hover && hits[i].enabled) Fill(r, radius, rgb, a);
    }

    void DrawFocus() {
        if (!focusVisible || focus < 0 || focus >= static_cast<int>(hits.size())) return;
        const Hit& h = hits[focus];
        Outline(Inset(h.r, -1.f), h.radius + 1.f, kAccent, 0.9f, 1.5f);
    }

    void Paint() {
        if (!EnsureFactories() || !EnsureTarget()) return;
        const Layout& lo = lay;
        const Model&  m = model;

        rt->BeginDraw();
        rt->Clear(Col(kBg));

        // Banner: the way out of "the devnode is off" sits above everything.
        if (lo.banner) {
            Fill(lo.bannerR, 0.f, kWarn, 0.12f);
            Fill(D2D1::RectF(0, lo.bannerR.bottom - 1, kWidth, lo.bannerR.bottom), 0.f, kWarn, 0.35f);
            IconWarn(14.f, 17.f, kWarn);
            Text(L"Controller device is switched off in Windows.", lo.bannerText, 12.5f,
                 DWRITE_FONT_WEIGHT_REGULAR, kWarnText, 1.f, false, DWRITE_TEXT_ALIGNMENT_LEADING, 0.f, true);
            Fill(lo.bannerBtn, 7.f, kWarn);
            Text(L"Re-enable", lo.bannerBtn, 12.5f, DWRITE_FONT_WEIGHT_BOLD, 0x1d1604, 1.f, false,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
        }

        // Header tile: lit when the controller is being driven.
        const bool live = m.controlOn && m.connected;
        {
            if (live) {
                Fill(Inset(lo.tile, -4.f), 15.f, 0x1a9fff, 0.07f);
                Fill(Inset(lo.tile, -2.f), 13.f, 0x1a9fff, 0.12f);
            }
            ComPtr<ID2D1GradientStopCollection> stops;
            const D2D1_GRADIENT_STOP gs[2] = {
                { 0.f, Col(live ? 0x1a9fff : 0x4a5663) },
                { 1.f, Col(live ? 0x0a4a78 : 0x2a323c) },
            };
            ComPtr<ID2D1LinearGradientBrush> grad;
            if (SUCCEEDED(rt->CreateGradientStopCollection(gs, 2, stops.GetAddressOf())) &&
                SUCCEEDED(rt->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(D2D1::Point2F(lo.tile.left, lo.tile.top),
                                                        D2D1::Point2F(lo.tile.right, lo.tile.bottom)),
                    stops.Get(), grad.GetAddressOf()))) {
                rt->FillRoundedRectangle(D2D1::RoundedRect(lo.tile, 11.f, 11.f), grad.Get());
            } else {
                Fill(lo.tile, 11.f, live ? 0x1a9fff : 0x4a5663);
            }
            // The ring is cut from the glyph in the tile's own dark end, so it
            // reads as a hole in the pad rather than a second shape on top.
            IconGamepad(lo.tile, 24.f, live ? 0xbfe3ff : 0x9aa5b0, live ? 0x0a4a78 : 0x2a323c);
        }

        Text(L"Steamless Mode", lo.title, 16.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, kText);

        // Status line: dot, text, and the battery when there is a reading.
        {
            const std::wstring status = StatusText(m);
            const UINT32 dotCol = m.deviceDisabled ? kWarn : live ? kOn : kIdle;
            float x = lo.status.left;
            const float cy = (lo.status.top + lo.status.bottom) * 0.5f;
            Dot(x + 3.5f, cy, 3.5f, dotCol);
            x += 14.f;
            const float sw = TextWidth(status, 12.5f, DWRITE_FONT_WEIGHT_REGULAR);
            Text(status, D2D1::RectF(x, lo.status.top, std::min(x + sw + 2.f, lo.status.right), lo.status.bottom),
                 12.5f, DWRITE_FONT_WEIGHT_REGULAR, kText2);
            x += sw + 7.f;
            if (m.battery >= 0 && x + 52.f <= lo.status.right) {
                Text(L"·", D2D1::RectF(x, lo.status.top, x + 8.f, lo.status.bottom), 12.5f,
                     DWRITE_FONT_WEIGHT_REGULAR, kText3);
                x += 14.f;
                IconBattery(x, cy - 8.f, m.battery, m.battery < 20 ? kWarn : kText2);
                x += 22.f;
                Text(std::to_wstring(m.battery) + L"%", D2D1::RectF(x, lo.status.top, lo.status.right, lo.status.bottom),
                     12.5f, DWRITE_FONT_WEIGHT_REGULAR, m.battery < 20 ? kWarn : kText2);
            }
        }

        // The master switch dims when it is not the user's to flip.
        const int mainHit = HitIndexFor(Action::ToggleControl);
        DrawSwitch(lo.mainSwitch, m.controlOn, mainHit >= 0 && hits[mainHit].enabled);

        // Control mode: four options in one track, with a line saying what the
        // selected one means.
        Text(L"CONTROL MODE", lo.modeLabel, 10.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, kText3, 1.f, true,
             DWRITE_TEXT_ALIGNMENT_LEADING, 1.4f);
        Fill(lo.segTrack, 11.f, kInset);
        for (int i = 0; i < 4; ++i) {
            const bool active = (m.mode == i);
            if (active) {
                Fill(lo.seg[i], 8.f, kAccent, 0.16f);
                Outline(lo.seg[i], 8.f, kAccent, 0.55f);
            } else {
                DrawHoverFor(i == 0 ? Action::ModeManual : i == 1 ? Action::ModeSteam
                           : i == 2 ? Action::ModeGame : Action::ModeProfile, lo.seg[i], 8.f, 0xffffff, 0.06f);
            }
            Text(ModeLabel(i), lo.seg[i], 13.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, active ? 0xffffff : kText2, 1.f,
                 false, DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        Text(ModeCaption(m.mode), lo.caption, 12.5f, DWRITE_FONT_WEIGHT_REGULAR, kText2);

        Fill(D2D1::RectF(0, lo.div1, kWidth, lo.div1 + 1), 0.f, 0xffffff, 0.07f);

        // Customize Controls: the one accented row, because it is the one that
        // leads somewhere.
        {
            const int ci = HitIndexFor(Action::Customize);
            const bool hot = (ci >= 0 && (ci == hover || (focusVisible && ci == focus)));
            Fill(lo.customize, 11.f, kAccent, hot ? 0.17f : 0.10f);
            Outline(lo.customize, 11.f, kAccent, hot ? 0.5f : 0.28f);
            IconSliders(lo.customize.left + 12.f, (lo.customize.top + lo.customize.bottom) * 0.5f - 11.f, kAccent);
            Text(L"Customize Controls",
                 D2D1::RectF(lo.customize.left + 46.f, lo.customize.top + 8.f, lo.customize.right - 36.f, lo.customize.top + 28.f),
                 14.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, kText);
            Text(L"Bindings, trackpads, triggers, sticks",
                 D2D1::RectF(lo.customize.left + 46.f, lo.customize.top + 28.f, lo.customize.right - 36.f, lo.customize.top + 46.f),
                 12.f, DWRITE_FONT_WEIGHT_REGULAR, kText2);
            IconChevron(lo.customize.right - 28.f, (lo.customize.top + lo.customize.bottom) * 0.5f - 8.f, kAccent);
        }

        Fill(D2D1::RectF(0, lo.div2, kWidth, lo.div2 + 1), 0.f, 0xffffff, 0.07f);

        // Settings as switches. Enable on Launch only exists in Manual mode; the
        // auto modes never make an at-launch decision.
        for (int i = 0; i < lo.rowCount; ++i) {
            // Drawn at full height and clipped to the row's current one, so a
            // row that is shrinking is cut off rather than squashed.
            const D2D1_RECT_F full = D2D1::RectF(lo.row[i].left, lo.row[i].top, lo.row[i].right, lo.row[i].top + 40.f);
            ComPtr<ID2D1Layer> layer;
            const bool partial = (lo.rowAct[i] == Action::EnableOnLaunch && launchT < 1.f);
            if (partial) {
                if (launchT <= 0.f) continue;
                if (SUCCEEDED(rt->CreateLayer(nullptr, layer.GetAddressOf())))
                    rt->PushLayer(D2D1::LayerParameters(lo.row[i], nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                        D2D1::IdentityMatrix(), launchT), layer.Get());
            }
            DrawHoverFor(lo.rowAct[i], full, 9.f, 0xffffff, 0.05f);
            Text(RowLabel(lo.rowAct[i]),
                 D2D1::RectF(full.left + 12.f, full.top, full.right - 60.f, full.bottom),
                 14.f, DWRITE_FONT_WEIGHT_REGULAR, kText);
            const float sx = full.right - 8.f - 36.f;
            const float sy = (full.top + full.bottom) * 0.5f - 10.f;
            DrawSwitch(D2D1::RectF(sx, sy, sx + 36.f, sy + 20.f), RowOn(lo.rowAct[i]), true);
            if (layer) rt->PopLayer();
        }

        // Footer.
        Fill(lo.footer, 0.f, kFooter);
        Fill(D2D1::RectF(0, lo.footer.top, kWidth, lo.footer.top + 1), 0.f, 0xffffff, 0.07f);
        DrawHoverFor(Action::OpenLog, lo.logBtn, 8.f, 0xffffff, 0.07f);
        DrawHoverFor(Action::Exit, lo.exitBtn, 8.f, 0xffffff, 0.07f);
        IconList(lo.logBtn.left + 12.f, (lo.logBtn.top + lo.logBtn.bottom) * 0.5f - 8.f, kText2);
        Text(L"Event log", D2D1::RectF(lo.logBtn.left + 36.f, lo.logBtn.top, lo.logBtn.right, lo.logBtn.bottom),
             13.f, DWRITE_FONT_WEIGHT_MEDIUM, kText2);
        Text(L"Exit", D2D1::RectF(lo.exitBtn.left + 12.f, lo.exitBtn.top, lo.exitBtn.right, lo.exitBtn.bottom),
             13.f, DWRITE_FONT_WEIGHT_MEDIUM, kText2);
        IconExit(lo.exitBtn.right - 28.f, (lo.exitBtn.top + lo.exitBtn.bottom) * 0.5f - 8.f, kText2);

        // Windows 10 has no rounded corners or outline to lend us.
        if (!rounded) Outline(D2D1::RectF(0, 0, kWidth, lo.height), 0.f, kBorder, 1.f);

        DrawFocus();

        const HRESULT hr = rt->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET) {
            brush.Reset();
            rt.Reset();
        }
    }

    // ---- behaviour --------------------------------------------------------

    void Refresh(bool force) {
        if (!hwnd || !provider) return;
        const Model next = provider();
        if (!force && next == model) return;
        model = next;

        const float target = model.manual ? 1.f : 0.f;
        if (target != animTo) {
            animFrom  = launchT;
            animTo    = target;
            animStart = GetTickCount64();
            SetTimer(hwnd, kAnimTimer, kAnimFrameMs, nullptr);
        }

        const float oldH = lay.height;
        BuildLayout();
        if (lay.height != oldH) ResizeKeepingBottom();
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    // One frame of the row's ease-out. Redrawn on the spot rather than left to
    // the next WM_PAINT, so the window and what is in it change together.
    void AnimateStep() {
        const float t = std::min(1.f, static_cast<float>(GetTickCount64() - animStart) / static_cast<float>(kAnimMs));
        const float inv = 1.f - t;
        launchT = animFrom + (animTo - animFrom) * (1.f - inv * inv * inv);
        if (t >= 1.f) {
            launchT = animTo;
            KillTimer(hwnd, kAnimTimer);
        }
        BuildLayout();
        ResizeKeepingBottom();
        InvalidateRect(hwnd, nullptr, FALSE);
        UpdateWindow(hwnd);
    }

    void ResizeKeepingBottom() {
        RECT wr{};
        GetWindowRect(hwnd, &wr);
        const int h = static_cast<int>(std::lround(lay.height * scale));
        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
        const int top = std::max<int>(wr.bottom - h, mi.rcWork.top);
        SetWindowPos(hwnd, nullptr, wr.left, top, wr.right - wr.left, h, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void Activate(int i) {
        if (i < 0 || i >= static_cast<int>(hits.size()) || !hits[i].enabled) return;
        const Action a = hits[i].a;
        // Choosing what is already chosen would only re-run its side effects.
        if (IsModeAction(a) && IsActive(hits[i])) return;

        // Copied first: closing the panel is not allowed to take the handler
        // with it, and what the action opens should not be fighting a panel
        // that is still on screen for focus.
        ActionHandler h = handler;
        if (ClosesPanel(a)) {
            CloseWindow_();
            if (h) h(a);
        } else {
            if (h) h(a);
            Refresh(true);
        }
    }

    void Step(int dir) {
        if (hits.empty()) return;
        const int n = static_cast<int>(hits.size());
        int i = focus;
        for (int k = 0; k < n; ++k) {
            i = (i < 0) ? (dir > 0 ? 0 : n - 1) : (i + dir + n) % n;
            if (hits[i].enabled) { focus = i; focusVisible = true; break; }
        }
        PublishAccess();
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    void StepSegment(int dir) {
        if (focus < 0 || !IsModeAction(hits[focus].a)) return;
        const int j = focus + dir;
        if (j >= 0 && j < static_cast<int>(hits.size()) && IsModeAction(hits[j].a)) {
            focus = j;
            focusVisible = true;
            PublishAccess();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
    }

    int HitAt(int px, int py) const {
        const float x = static_cast<float>(px) / scale, y = static_cast<float>(py) / scale;
        for (size_t i = 0; i < hits.size(); ++i)
            if (Contains(hits[i].r, x, y)) return static_cast<int>(i);
        return -1;
    }

    void CloseWindow_() {
        if (!hwnd) return;
        closing = true;
        HWND h = hwnd;
        hwnd = nullptr;
        KillTimer(h, kRefreshTimer);
        KillTimer(h, kAnimTimer);
        // Before the window goes: a screen reader may be mid-question, and should be
        // told "gone" rather than find a provider pointing at nothing.
        if (a11y) { a11y->Detach(); a11y.reset(); }
        DestroyWindow(h);
        closing = false;
        rt.Reset();
        brush.Reset();
        hover = focus = -1;
        tracking = false;
    }

    LRESULT Handle(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_GETOBJECT:
            // A screen reader asking what is in this window.
            if (a11y && PanelAccessibility::WantsRoot(lp)) return a11y->HandleGetObject(wp, lp);
            break;

        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(h, &ps);
            Paint();
            EndPaint(h, &ps);
            return 0;
        }

        case WM_SIZE:
            if (rt) {
                rt->Resize(D2D1::SizeU(static_cast<UINT32>(std::max<int>(LOWORD(lp), 1)),
                                       static_cast<UINT32>(std::max<int>(HIWORD(lp), 1))));
            }
            return 0;

        case WM_MOUSEMOVE: {
            if (!tracking) {
                TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, h, 0 };
                TrackMouseEvent(&tme);
                tracking = true;
            }
            const int i = HitAt(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (i != hover) {
                hover = i;
                focus = -1;  // the pointer takes over from the keyboard
                PublishAccess();
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            tracking = false;
            if (hover != -1) { hover = -1; InvalidateRect(h, nullptr, FALSE); }
            return 0;

        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                const bool hand = hover >= 0 && hover < static_cast<int>(hits.size()) && hits[hover].enabled;
                SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(hand ? 32649 : 32512)));
                return TRUE;
            }
            break;

        case WM_LBUTTONUP: {
            const int i = HitAt(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            if (i >= 0) Activate(i);
            return 0;
        }

        case WM_KEYDOWN:
            switch (wp) {
            case VK_ESCAPE: CloseWindow_(); return 0;
            case VK_DOWN:   Step(+1); return 0;
            case VK_UP:     Step(-1); return 0;
            case VK_TAB:    Step((GetKeyState(VK_SHIFT) & 0x8000) ? -1 : +1); return 0;
            case VK_RIGHT:  StepSegment(+1); return 0;
            case VK_LEFT:   StepSegment(-1); return 0;
            case VK_RETURN:
            case VK_SPACE:  Activate(focus); return 0;
            }
            break;

        case WM_GETDLGCODE:
            return DLGC_WANTALLKEYS;

        case WM_TIMER:
            if (wp == kRefreshTimer)   Refresh(false);
            else if (wp == kAnimTimer) AnimateStep();
            return 0;

        case WM_ACTIVATE:
            // Clicking anywhere else, switching windows, a UAC prompt — all of
            // them deactivate the panel, and that is the whole of "click away".
            if (LOWORD(wp) == WA_INACTIVE && !closing) {
                dismissedAt = GetTickCount64();
                CloseWindow_();
            }
            return 0;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        }
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        // hwnd is cleared before DestroyWindow, so late messages from the
        // teardown itself reach DefWindowProc instead of half-dead state.
        if (!self || self->hwnd != h) return DefWindowProcW(h, msg, wp, lp);
        return self->Handle(h, msg, wp, lp);
    }
};

// ---------------------------------------------------------------------------

TrayPanel::TrayPanel() : m_d(std::make_unique<Impl>()) {}
TrayPanel::~TrayPanel() { Close(); }

bool TrayPanel::IsOpen() const { return m_d->hwnd != nullptr; }

std::wstring TrayPanel::FontFamilies() const {
    return m_d->sans + L" / " + m_d->mono + (m_d->bundled ? L" (bundled)" : L" (system)");
}

bool TrayPanel::RecentlyDismissed() const {
    return m_d->dismissedAt != 0 && GetTickCount64() - m_d->dismissedAt < kDismissGraceMs;
}

void TrayPanel::Close() { m_d->CloseWindow_(); }

void TrayPanel::Show(HINSTANCE hInst, POINT anchor, ModelProvider provider, ActionHandler handler) {
    Impl& d = *m_d;
    d.CloseWindow_();
    d.hInst = hInst;  // before the factories: the bundled fonts are read from it
    if (!d.EnsureFactories()) return;

    d.provider = std::move(provider);
    d.handler  = std::move(handler);
    d.model    = d.provider();
    d.dismissedAt = 0;

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_DROPSHADOW;
        wc.lpfnWndProc   = Impl::WndProc;
        wc.hInstance     = hInst;
        wc.hCursor       = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        wc.lpszClassName = kClassName;
        RegisterClassExW(&wc);
        registered = true;
    }

    // Created on the work area's corner of the monitor the click was on, so the
    // window reports that monitor's DPI before it is sized.
    const HMONITOR mon = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(mon, &mi);

    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kClassName, L"SteamlessController",
                                WS_POPUP, mi.rcWork.left, mi.rcWork.top, 8, 8,
                                nullptr, nullptr, hInst, &d);
    if (!hwnd) return;
    d.hwnd = hwnd;
    d.scale = static_cast<float>(GetDpiForWindow(hwnd)) / 96.f;
    d.formats.clear();
    d.hover = d.focus = -1;
    d.focusVisible = false;
    // Opens already at the right height; only a change while it is up animates.
    d.launchT = d.animFrom = d.animTo = d.model.manual ? 1.f : 0.f;
    d.a11y = std::make_unique<PanelAccessibility>(d);
    d.BuildLayout();
    // With a screen reader running, start with keyboard focus on the first control
    // so it has somewhere to begin reading from. Otherwise focus stays unset until
    // a key is pressed, so a mouse user is not shown a focus ring they did not ask
    // for.
    d.focusVisible = false;
    if (PanelAccessibility::ClientsListening()) {
        d.Step(+1);
        d.focusVisible = false;  // Step shows the ring; this opening is not a keypress
    }

    const int w   = static_cast<int>(std::lround(kWidth * d.scale));
    const int h   = static_cast<int>(std::lround(d.lay.height * d.scale));
    const int gap = static_cast<int>(std::lround(kEdge * d.scale));
    int x = anchor.x - w;
    int y = anchor.y - h;
    x = std::min<int>(x, mi.rcWork.right - w - gap);
    x = std::max<int>(x, mi.rcWork.left + gap);
    y = std::min<int>(y, mi.rcWork.bottom - h - gap);
    y = std::max<int>(y, mi.rcWork.top + gap);
    SetWindowPos(hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);

    // Windows 11 rounds and outlines the window itself; where that is not
    // available the call fails and the panel draws its own edge.
    const DWORD round = 2;  // DWMWCP_ROUND
    d.rounded = SUCCEEDED(DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &round, sizeof(round)));
    if (d.rounded) {
        const COLORREF border = RGB(0x2b, 0x35, 0x41);
        DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
    }

    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
    SetTimer(hwnd, kRefreshTimer, kRefreshMs, nullptr);
}
