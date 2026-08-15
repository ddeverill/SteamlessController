#include "WinOnScreenKeyboard.h"
#include "core/EventLog.h"

#include <Windows.h>
#include <ShlObj.h>
#include <objbase.h>

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace {

// Neither of these is in the Windows SDK: showing the touch keyboard has never
// been public API, so the class and its one interface are declared here from
// the IIDs Windows registers them under. CLSID_UIHostNoLaunch is registered on
// Windows 11 with a Programmable subkey and no LocalServer32 — the name is
// literal, COM will not start the server, which is what ToggleNow's cold path
// below is for.
const CLSID kUIHostNoLaunch = { 0x4CE576FA, 0x83DC, 0x4F88,
                                { 0x95, 0x1C, 0x9D, 0x07, 0x82, 0xB4, 0xE3, 0x76 } };
const IID kIID_ITipInvocation = { 0x37C994E7, 0x432B, 0x4834,
                                  { 0xA2, 0xF7, 0xDC, 0xE1, 0xF1, 0x3B, 0x83, 0x4B } };

struct ITipInvocation : IUnknown {
    // Toggle takes the window the keyboard should position itself against.
    // The desktop window is what every caller passes and what gives the
    // keyboard its normal docked placement.
    virtual HRESULT STDMETHODCALLTYPE Toggle(HWND wnd) = 0;
};

// %CommonProgramFiles%\microsoft shared\ink\TabTip.exe. Asked of the shell
// rather than composed from an environment variable so it is right on a
// system installed somewhere other than C:, and on a 32-bit build of this app
// running on 64-bit Windows, where the variable is redirected and the folder
// id is not.
std::wstring TabTipPath() {
    PWSTR common = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFilesCommon, 0, nullptr, &common)))
        return L"";
    std::wstring path = common;
    CoTaskMemFree(common);
    path += L"\\microsoft shared\\ink\\TabTip.exe";
    return path;
}

// Starts TabTip so it can register the class object. TabTip is a uiAccess
// binary, which Windows auto-elevates without a UAC prompt because it is
// signed by Microsoft and lives in a protected directory — so starting it
// from this Medium-integrity process is allowed and silent.
bool StartTabTip() {
    const std::wstring path = TabTipPath();
    if (path.empty()) {
        EventLog::Write("TOUCHKB: cannot locate TabTip.exe (no common files folder)");
        return false;
    }
    const auto result = ShellExecuteW(nullptr, L"open", path.c_str(),
                                      nullptr, nullptr, SW_HIDE);
    // ShellExecuteW reports failure as a value of 32 or less, cast from an
    // error code rather than returned as one.
    const auto code = static_cast<int>(reinterpret_cast<INT_PTR>(result));
    if (code <= 32) {
        EventLog::Write("TOUCHKB: could not start %ls (code=%d)", path.c_str(), code);
        return false;
    }
    return true;
}

HRESULT CreateTip(ITipInvocation** out) {
    return CoCreateInstance(kUIHostNoLaunch, nullptr,
                            CLSCTX_LOCAL_SERVER | CLSCTX_INPROC_HANDLER,
                            kIID_ITipInvocation, reinterpret_cast<void**>(out));
}

// One toggle, on the worker thread, which owns the COM apartment.
void ToggleNow() {
    ITipInvocation* tip = nullptr;
    HRESULT hr = CreateTip(&tip);

    if (FAILED(hr)) {
        // TabTip is normally started by the shell at logon, so this is the
        // cold path: no server, therefore no class object to bind to. Start it
        // and wait for it to register, polling rather than sleeping a fixed
        // time because the wait is the process starting, not a known interval.
        if (!StartTabTip()) return;
        for (int i = 0; i < 20 && FAILED(hr); ++i) {
            Sleep(100);
            hr = CreateTip(&tip);
        }
        if (FAILED(hr)) {
            EventLog::Write("TOUCHKB: TabTip started but never registered its "
                            "toggle interface (hr=0x%08lX) — the touch keyboard "
                            "is unavailable on this system", hr);
            return;
        }
    }

    hr = tip->Toggle(GetDesktopWindow());
    tip->Release();
    if (FAILED(hr))
        EventLog::Write("TOUCHKB: toggle refused (hr=0x%08lX)", hr);
}

// Presses are counted rather than collapsed into a pending flag: two presses
// mean show and then hide, and coalescing them would leave the keyboard in the
// state the user just asked it to leave. Capped so that a Toggle call wedged
// against an unresponsive TabTip cannot turn a mashed paddle into an unbounded
// backlog that plays back later.
constexpr unsigned kMaxPending = 4;

std::mutex              g_mutex;
std::condition_variable g_wake;
unsigned                g_pending = 0;      // guarded by g_mutex
bool                    g_workerStarted = false;

void Worker() {
    // Apartment-threaded because this thread does nothing but make one
    // outbound call at a time and has no windows of its own.
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        EventLog::Write("TOUCHKB: COM would not initialise; the touch keyboard "
                        "binding will do nothing this session");
        std::lock_guard<std::mutex> lk(g_mutex);
        g_workerStarted = false;   // let a later press try again
        return;
    }

    // Runs until the process exits. There is no shutdown handshake and no
    // CoUninitialize: nothing here outlives the process that would need
    // cleaning up, and a thread parked in a cross-process COM call cannot be
    // joined promptly anyway — waiting on it at exit would hang the tray.
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(g_mutex);
            g_wake.wait(lk, [] { return g_pending > 0; });
            --g_pending;
        }
        ToggleNow();
    }
}

}  // namespace

namespace TouchKeyboard {

void Toggle() {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (!g_workerStarted) {
        g_workerStarted = true;
        // Detached: see Worker. Started on first use so a session that never
        // binds this action never pays for the thread or the COM apartment.
        std::thread(Worker).detach();
    }
    if (g_pending < kMaxPending) ++g_pending;
    g_wake.notify_one();
}

}  // namespace TouchKeyboard
