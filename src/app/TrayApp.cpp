#include "TrayApp.h"
#include "ControllerManager.h"
#include "ControllerPlatform.h"
#include "resource.h"
#include <shellapi.h>
#include <dbt.h>
#include <winreg.h>

static TrayApp* g_app = nullptr;

static constexpr wchar_t WNDCLASS_NAME[] = L"SteamlessControllerTray";

TrayApp::TrayApp() {
    g_app = this;
}

TrayApp::~TrayApp() {
    RemoveTrayIcon();
    g_app = nullptr;
}

bool TrayApp::Init(HINSTANCE hInstance) {
    m_hInstance = hInstance;
    m_iconOff   = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_ICON_OFF));
    m_iconOn    = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_ICON_ON));
    m_wmTaskbar = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = WNDCLASS_NAME;
    if (!RegisterClassExW(&wc)) return false;

    // Message-only window — invisible, never shown.
    m_hwnd = CreateWindowExW(0, WNDCLASS_NAME, L"SteamlessController",
                             0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInstance, nullptr);
    if (!m_hwnd) return false;

    // Register for HID device arrival/removal notifications.
    DEV_BROADCAST_DEVICEINTERFACE_W filter{};
    filter.dbcc_size       = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid  = {0x4D1E55B2, 0xF16F, 0x11CF,
                              {0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30}};
    RegisterDeviceNotificationW(m_hwnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);

    m_controller = std::make_unique<ControllerManager>(
        [this](bool connected, bool gameModeActive, bool vigemMissing) {
            UpdateTrayIcon(connected, gameModeActive, vigemMissing);
        });

    LoadSettings();
    AddTrayIcon();
    UpdateTrayIcon(m_controller->IsConnected(), m_controller->IsGameModeActive(), false);
    return true;
}

int TrayApp::Run() {
    MSG msg;
    BOOL ret;
    while ((ret = GetMessageW(&msg, nullptr, 0, 0)) != 0) {
        if (ret == -1) return -1;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK TrayApp::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_app) return g_app->HandleMessage(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT TrayApp::HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == m_wmTaskbar) {
        AddTrayIcon();
        return 0;
    }

    switch (msg) {
    case WM_TRAY:
        if (LOWORD(lp) == NIN_BALLOONUSERCLICK)
            ShellExecuteW(nullptr, L"open", L"https://github.com/nefarius/ViGEmBus/releases/latest",
                          nullptr, nullptr, SW_SHOWNORMAL);
        else if (LOWORD(lp) == WM_LBUTTONDBLCLK)
            OpenRemapWindow();
        else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONUP)
            ShowContextMenu();
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_TOGGLE:
            if (m_controller->IsGameModeActive())
                m_controller->DisableGameMode();
            else
                m_controller->EnableGameMode();
            break;
        case IDM_TRACKPAD:
            m_controller->SetTrackpadMouseEnabled(!m_controller->IsTrackpadMouseEnabled());
            SaveSettings();
            break;
        case IDM_REMAP_BACK:
            OpenRemapWindow();
            break;
        case IDM_BACKBUTTONS:
            m_controller->SetBackButtonsEnabled(!m_controller->IsBackButtonsEnabled());
            SaveSettings();
            break;
        case IDM_LEFT_TRACKPAD:
            m_controller->SetUseLeftTrackpad(!m_controller->IsUseLeftTrackpad());
            SaveSettings();
            break;
        case IDM_PLATFORM_XBOX:
            m_controller->SetControllerPlatform(ControllerPlatform::Xbox);
            SaveSettings();
            break;
        case IDM_PLATFORM_PS:
            m_controller->SetControllerPlatform(ControllerPlatform::PlayStation);
            SaveSettings();
            break;
        case IDM_STARTUP:
            SetStartupEnabled(!IsStartupEnabled());
            break;
        case IDM_EXIT:
            m_controller->DisableGameMode();
            PostQuitMessage(0);
            break;
        }
        return 0;

    case WM_DEVICECHANGE:
        if (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE)
            m_controller->OnDeviceChange();
        return TRUE;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void TrayApp::OpenRemapWindow() {
    m_remapWindow.Open(
        m_hInstance,
        m_controller.get(),
        m_controller->GetBackButtonConfig(),
        [this](const BackButtonConfig& cfg) {
            m_controller->SetBackButtonConfig(cfg);
            SaveSettings();
        });
}

void TrayApp::AddTrayIcon() {
    NOTIFYICONDATAW nid{};
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = m_hwnd;
    nid.uID              = TRAY_UID;
    nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon            = m_iconOff;
    wcscpy_s(nid.szTip, L"Steamless Controller");
    Shell_NotifyIconW(NIM_ADD, &nid);
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

void TrayApp::RemoveTrayIcon() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd   = m_hwnd;
    nid.uID    = TRAY_UID;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void TrayApp::UpdateTrayIcon(bool connected, bool gameModeActive, bool vigemMissing) {
    if (vigemMissing) { ShowViGEmBalloon(); return; }

    const wchar_t* tip = gameModeActive ? L"Steamless Controller — Steamless Mode ON"
                       : connected      ? L"Steamless Controller — Connected (Steamless Mode OFF)"
                                        : L"Steamless Controller — No controller found";

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd   = m_hwnd;
    nid.uID    = TRAY_UID;
    nid.uFlags = NIF_TIP | NIF_ICON;
    nid.hIcon  = gameModeActive ? m_iconOn : m_iconOff;
    wcscpy_s(nid.szTip, tip);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayApp::ShowViGEmBalloon() {
    NOTIFYICONDATAW nid{};
    nid.cbSize           = sizeof(nid);
    nid.hWnd             = m_hwnd;
    nid.uID              = TRAY_UID;
    nid.uFlags           = NIF_INFO;
    nid.dwInfoFlags      = NIIF_WARNING;
    wcscpy_s(nid.szInfoTitle, L"Driver required");
    wcscpy_s(nid.szInfo,      L"ViGEmBus is not installed. Click here to download it.");
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static constexpr wchar_t REG_KEY[]     = L"Software\\SteamlessController";
static constexpr wchar_t REG_RUN_KEY[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static constexpr wchar_t APP_NAME[]    = L"SteamlessController";

bool TrayApp::IsStartupEnabled() const {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;
    bool exists = RegQueryValueExW(key, APP_NAME, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
    RegCloseKey(key);
    return exists;
}

void TrayApp::SetStartupEnabled(bool enabled) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN_KEY, 0, KEY_WRITE, &key) != ERROR_SUCCESS)
        return;

    if (enabled) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        RegSetValueExW(key, APP_NAME, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(path),
                       static_cast<DWORD>((wcslen(path) + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, APP_NAME);
    }

    RegCloseKey(key);
}

void TrayApp::LoadSettings() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return;

    auto readDw = [&](const wchar_t* name, DWORD def) -> DWORD {
        DWORD val = 0, size = sizeof(val);
        if (RegQueryValueExW(key, name, nullptr, nullptr,
                             reinterpret_cast<LPBYTE>(&val), &size) == ERROR_SUCCESS)
            return val;
        return def;
    };

    m_controller->SetTrackpadMouseEnabled(readDw(L"TrackpadMouse",   0) != 0);
    m_controller->SetBackButtonsEnabled  (readDw(L"BackButtons",     0) != 0);
    m_controller->SetUseLeftTrackpad     (readDw(L"UseLeftTrackpad", 0) != 0);
    m_controller->SetControllerPlatform  (readDw(L"ControllerPlatform", 0) != 0
                                          ? ControllerPlatform::PlayStation
                                          : ControllerPlatform::Xbox);

    BackButtonConfig cfg;
    cfg.l4 = static_cast<BackButtonAction>(readDw(L"BackBtnL4", static_cast<DWORD>(BackButtonAction::None)));
    cfg.l5 = static_cast<BackButtonAction>(readDw(L"BackBtnL5", static_cast<DWORD>(BackButtonAction::None)));
    cfg.r4 = static_cast<BackButtonAction>(readDw(L"BackBtnR4", static_cast<DWORD>(BackButtonAction::None)));
    cfg.r5 = static_cast<BackButtonAction>(readDw(L"BackBtnR5", static_cast<DWORD>(BackButtonAction::None)));
    m_controller->SetBackButtonConfig(cfg);

    RegCloseKey(key);
}

void TrayApp::SaveSettings() {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_KEY, 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr,
                        &key, nullptr) != ERROR_SUCCESS)
        return;

    auto writeDw = [&](const wchar_t* name, DWORD val) {
        RegSetValueExW(key, name, 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&val), sizeof(val));
    };

    writeDw(L"TrackpadMouse",       m_controller->IsTrackpadMouseEnabled()  ? 1 : 0);
    writeDw(L"BackButtons",         m_controller->IsBackButtonsEnabled()    ? 1 : 0);
    writeDw(L"UseLeftTrackpad",     m_controller->IsUseLeftTrackpad()       ? 1 : 0);
    writeDw(L"ControllerPlatform",  m_controller->GetControllerPlatform() == ControllerPlatform::PlayStation ? 1 : 0);

    const auto& cfg = m_controller->GetBackButtonConfig();
    writeDw(L"BackBtnL4", static_cast<DWORD>(cfg.l4));
    writeDw(L"BackBtnL5", static_cast<DWORD>(cfg.l5));
    writeDw(L"BackBtnR4", static_cast<DWORD>(cfg.r4));
    writeDw(L"BackBtnR5", static_cast<DWORD>(cfg.r5));

    RegCloseKey(key);
}

void TrayApp::ShowContextMenu() {
    bool connected    = m_controller->IsConnected();
    bool gameModeOn   = m_controller->IsGameModeActive();
    bool trackpadOn   = m_controller->IsTrackpadMouseEnabled();
    bool backMouseOn  = m_controller->IsBackButtonsEnabled();
    bool leftPad      = m_controller->IsUseLeftTrackpad();
    bool startupOn    = IsStartupEnabled();

    HMENU menu = CreatePopupMenu();

    UINT toggleFlags = MF_STRING | (connected ? MF_ENABLED : MF_GRAYED);
    AppendMenuW(menu, toggleFlags, IDM_TOGGLE,
                gameModeOn ? L"Disable Steamless Mode" : L"Enable Steamless Mode");

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(menu, MF_STRING | (trackpadOn ? MF_CHECKED : MF_UNCHECKED),
                IDM_TRACKPAD, L"Enable Trackpad Mouse");

    AppendMenuW(menu, MF_STRING | (backMouseOn ? MF_CHECKED : MF_UNCHECKED),
                IDM_BACKBUTTONS, L"Back Buttons as Mouse Click");

    AppendMenuW(menu, MF_STRING | (leftPad ? MF_CHECKED : MF_UNCHECKED),
                IDM_LEFT_TRACKPAD, L"Use Left Trackpad Instead");

    AppendMenuW(menu, MF_STRING, IDM_REMAP_BACK, L"Remap Back Buttons...");

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    bool isPS = (m_controller->GetControllerPlatform() == ControllerPlatform::PlayStation);
    HMENU platformMenu = CreatePopupMenu();
    AppendMenuW(platformMenu, MF_STRING | (!isPS ? MF_CHECKED : MF_UNCHECKED),
                IDM_PLATFORM_XBOX, L"Xbox");
    AppendMenuW(platformMenu, MF_STRING | (isPS ? MF_CHECKED : MF_UNCHECKED),
                IDM_PLATFORM_PS, L"PlayStation");
    AppendMenuW(menu, MF_STRING | MF_POPUP,
                reinterpret_cast<UINT_PTR>(platformMenu), L"Controller Platform");

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(menu, MF_STRING | (startupOn ? MF_CHECKED : MF_UNCHECKED),
                IDM_STARTUP, L"Start with Windows");

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");

    // SetForegroundWindow is required for the menu to dismiss on click-away.
    SetForegroundWindow(m_hwnd);

    POINT pt;
    GetCursorPos(&pt);
    TrackPopupMenu(menu, TPM_RIGHTALIGN | TPM_BOTTOMALIGN | TPM_RIGHTBUTTON,
                   pt.x, pt.y, 0, m_hwnd, nullptr);
    DestroyMenu(menu);
}
