#include "WinInputInjector.h"
#include "core/EventLog.h"
#include "core/Text.h"
#include <atomic>
#include <cstdint>
#include <vector>

namespace {

// A wedged foreground window refuses every event at report rate, so the log
// gets one line per burst rather than one per event.
constexpr uint64_t kLogGapMs = 5000;
// The balloon is a warning, not a running commentary: alt-tabbing in and out
// of an elevated window must not produce one notification per trip.
constexpr uint64_t kAlertGapMs = 60000;
constexpr DWORD kIntegrityUnknown = 0xFFFFFFFFu;

std::atomic<uint64_t> g_refused{0};
std::atomic<uint64_t> g_refusedTotal{0};
std::atomic<uint64_t> g_lastRefusalLogMs{0};
std::atomic<uint64_t> g_lastStuckLogMs{0};
std::atomic<bool>     g_refusing{false};

bool DueForLog(std::atomic<uint64_t>& lastMs) {
    const uint64_t now  = GetTickCount64();
    uint64_t       last = lastMs.load(std::memory_order_relaxed);
    if (last != 0 && now - last < kLogGapMs) return false;
    return lastMs.compare_exchange_strong(last, now, std::memory_order_relaxed);
}

const wchar_t* IntegrityName(DWORD rid) {
    if (rid == kIntegrityUnknown)             return L"unreadable";
    if (rid >= SECURITY_MANDATORY_SYSTEM_RID) return L"System";
    if (rid >= SECURITY_MANDATORY_HIGH_RID)   return L"High/elevated";
    if (rid >= SECURITY_MANDATORY_MEDIUM_RID) return L"Medium";
    if (rid >= SECURITY_MANDATORY_LOW_RID)    return L"Low";
    return L"Untrusted";
}

DWORD ProcessIntegrity(HANDLE process) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return kIntegrityUnknown;

    DWORD size = 0;
    GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size);

    DWORD rid = kIntegrityUnknown;
    if (size > 0) {
        std::vector<uint8_t> buf(size);
        auto* label = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buf.data());
        if (GetTokenInformation(token, TokenIntegrityLevel, label, size, &size)) {
            const UCHAR* count = GetSidSubAuthorityCount(label->Label.Sid);
            if (count && *count > 0)
                rid = *GetSidSubAuthority(label->Label.Sid, static_cast<DWORD>(*count - 1));
        }
    }
    CloseHandle(token);
    return rid;
}

// This process's own level never changes, so it is worth asking once.
DWORD OwnIntegrity() {
    static const DWORD own = ProcessIntegrity(GetCurrentProcess());
    return own;
}

std::wstring DescribeForeground(DWORD& integrityOut) {
    integrityOut = kIntegrityUnknown;

    const HWND fg = GetForegroundWindow();
    if (!fg)
        return L"(none — no foreground window; a UAC prompt, the lock screen "
               L"or a desktop switch owns input)";

    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);

    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) {
        wchar_t out[128];
        swprintf_s(out, L"pid=%lu (cannot open, err=%lu — protected process?)", pid, GetLastError());
        return out;
    }

    wchar_t image[MAX_PATH] = L"";
    DWORD   imageLen        = MAX_PATH;
    if (!QueryFullProcessImageNameW(proc, 0, image, &imageLen))
        wcscpy_s(image, L"(unknown)");

    const wchar_t* leaf = wcsrchr(image, L'\\');
    leaf = leaf ? leaf + 1 : image;

    integrityOut = ProcessIntegrity(proc);
    CloseHandle(proc);

    wchar_t out[256];
    swprintf_s(out, L"%s pid=%lu integrity=%s", leaf, pid, IntegrityName(integrityOut));
    return out;
}

std::wstring DescribeClip() {
    RECT clip{};
    if (!GetClipCursor(&clip)) {
        wchar_t out[64];
        swprintf_s(out, L"unreadable (err=%lu)", GetLastError());
        return out;
    }
    RECT screen{};
    screen.left   = GetSystemMetrics(SM_XVIRTUALSCREEN);
    screen.top    = GetSystemMetrics(SM_YVIRTUALSCREEN);
    screen.right  = screen.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    screen.bottom = screen.top  + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    wchar_t out[160];
    if (EqualRect(&clip, &screen)) {
        swprintf_s(out, L"none (screen is (%ld,%ld)-(%ld,%ld))",
                   screen.left, screen.top, screen.right, screen.bottom);
    } else {
        swprintf_s(out, L"CONFINED to (%ld,%ld)-(%ld,%ld) of screen (%ld,%ld)-(%ld,%ld)",
                   clip.left, clip.top, clip.right, clip.bottom,
                   screen.left, screen.top, screen.right, screen.bottom);
    }
    return out;
}

std::wstring DescribeCursor() {
    POINT pt{};
    if (!GetCursorPos(&pt)) {
        wchar_t out[96];
        swprintf_s(out, L"unreadable (err=%lu — not on the input desktop)", GetLastError());
        return out;
    }
    wchar_t out[64];
    swprintf_s(out, L"(%ld,%ld)", pt.x, pt.y);
    return out;
}

// ---------------------------------------------------------------------------
// Scan-code translation (was KeyInput.cpp)
// ---------------------------------------------------------------------------

// The navigation cluster shares scan codes with the numpad and is told apart
// only by the 0xE0 extended prefix — drop it and an Up arrow arrives as
// numpad 8. MapVirtualKey is documented to report that prefix under
// MAPVK_VK_TO_VSC_EX but does not actually do so for these keys, so the set
// is spelled out here.
bool IsExtendedKey(uint16_t vk) {
    switch (vk) {
    case VK_RCONTROL: case VK_RMENU:
    case VK_INSERT:   case VK_DELETE:
    case VK_HOME:     case VK_END:
    case VK_PRIOR:    case VK_NEXT:
    case VK_UP:       case VK_DOWN:
    case VK_LEFT:     case VK_RIGHT:
    case VK_NUMLOCK:  case VK_DIVIDE:
    case VK_SNAPSHOT:
    case VK_LWIN:     case VK_RWIN:  case VK_APPS:
        return true;
    default:
        return false;
    }
}

struct ScanCode { WORD scan; bool extended; };

ScanCode ScanCodeFor(uint16_t vk) {
    // _EX is still the right lookup for the scan code itself: it distinguishes
    // left from right modifiers, which the plain mapping collapses.
    const UINT mapped = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC_EX);
    return { static_cast<WORD>(mapped & 0xFF), IsExtendedKey(vk) };
}

}  // namespace

void WinInputInjector::SetAlertCallback(AlertFn fn) {
    std::lock_guard<std::mutex> lk(m_stateMutex);
    m_alertFn = std::move(fn);
}

std::wstring WinInputInjector::ForegroundBlocker(DWORD& pidOut) {
    pidOut = 0;

    const HWND fg = GetForegroundWindow();
    if (!fg) return L"";  // secure desktop or lock screen; nothing to name

    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == 0 || pid == GetCurrentProcessId()) return L"";
    pidOut = pid;

    std::lock_guard<std::mutex> lk(m_stateMutex);
    if (fg == m_cachedHwnd && pid == m_cachedPid)
        return m_cachedBlocked ? m_cachedName : std::wstring();

    m_cachedHwnd    = fg;
    m_cachedPid     = pid;
    m_cachedBlocked = false;
    m_cachedName.clear();

    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return L"";

    const DWORD integrity = ProcessIntegrity(proc);

    wchar_t image[MAX_PATH] = L"";
    DWORD   imageLen        = MAX_PATH;
    if (!QueryFullProcessImageNameW(proc, 0, image, &imageLen))
        wcscpy_s(image, L"(unknown)");
    CloseHandle(proc);

    const DWORD own = OwnIntegrity();
    if (integrity == kIntegrityUnknown || own == kIntegrityUnknown || integrity <= own)
        return L"";

    const wchar_t* leaf = wcsrchr(image, L'\\');
    m_cachedName    = leaf ? leaf + 1 : image;
    m_cachedBlocked = true;
    return m_cachedName;
}

void WinInputInjector::NoteBlockState(const std::wstring& blocker, DWORD pid, const char* what) {
    std::wstring startedBlocking, stoppedBlocking;
    AlertFn      alert;

    {
        std::lock_guard<std::mutex> lk(m_stateMutex);
        if (!blocker.empty()) {
            if (blocker != m_blockerLogged) {
                startedBlocking = blocker;
                m_blockerLogged = blocker;

                const uint64_t now = GetTickCount64();
                if (blocker != m_alertBlocker || now - m_lastAlertMs >= kAlertGapMs) {
                    m_alertBlocker = blocker;
                    m_lastAlertMs  = now;
                    alert          = m_alertFn;
                }
            }
        } else if (!m_blockerLogged.empty()) {
            stoppedBlocking = m_blockerLogged;
            m_blockerLogged.clear();
        }
    }

    if (!startedBlocking.empty()) {
        EventLog::Write("INJECT: BLOCKED (%s) — the foreground window belongs to "
                        "%s (pid=%lu), which runs at a higher integrity level; "
                        "Windows accepts each event and then discards it",
                        what, WideToUtf8(startedBlocking).c_str(), pid);
        LogEnvironment("state while blocked");
    }
    if (!stoppedBlocking.empty())
        EventLog::Write("INJECT: unblocked — %s is no longer the foreground window",
                        WideToUtf8(stoppedBlocking).c_str());

    // Outside the lock: the callback belongs to whoever wired it up (the
    // tray app, the daemon), and it is not this class's business what it
    // does before returning.
    if (alert) {
        std::string text = WideToUtf8(startedBlocking);
        text += " is running as administrator, and Windows does not let "
                "SteamlessController send input while it is the active window. "
                "The trackpad and buttons work again as soon as you switch to "
                "another window or close it.";
        alert("Controller input is being blocked", text);
    }
}

bool WinInputInjector::Send(const INPUT& input, const char* what) {
    // Asked before sending, and sent either way: the check is what reports
    // the fault, not a gate on the event. If it is ever wrong about a
    // system, the input still goes out exactly as it did before.
    DWORD              blockerPid = 0;
    const std::wstring blocker    = ForegroundBlocker(blockerPid);
    NoteBlockState(blocker, blockerPid, what);

    INPUT copy = input;  // SendInput takes a non-const array
    if (SendInput(1, &copy, sizeof(INPUT)) == 1) {
        if (g_refusing.exchange(false)) {
            EventLog::Write("INJECT: accepted again after %llu refused event(s)",
                            static_cast<unsigned long long>(g_refusedTotal.exchange(0)));
            g_refused.store(0);
            g_lastRefusalLogMs.store(0);
        }
        return true;
    }

    const DWORD err = GetLastError();
    g_refusing.store(true);
    g_refusedTotal.fetch_add(1);
    const uint64_t burst = g_refused.fetch_add(1) + 1;

    if (DueForLog(g_lastRefusalLogMs)) {
        g_refused.store(0);
        EventLog::Write("INJECT: SendInput REFUSED (%s, err=%lu%s) — %llu event(s) "
                        "dropped since the last line",
                        what, err, err == ERROR_ACCESS_DENIED ? " ACCESS_DENIED" : "",
                        static_cast<unsigned long long>(burst));
        LogEnvironment("state at refusal");
    }
    return false;
}

bool WinInputInjector::MoveMouse(int dx, int dy) {
    INPUT input{};
    input.type       = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    input.mi.dx      = dx;
    input.mi.dy      = dy;
    return Send(input, "trackpad-move");
}

void WinInputInjector::Scroll(int vDelta, int hDelta) {
    if (vDelta != 0) {
        INPUT input{};
        input.type         = INPUT_MOUSE;
        input.mi.dwFlags   = MOUSEEVENTF_WHEEL;
        input.mi.mouseData = static_cast<DWORD>(vDelta);
        Send(input, "trackpad-scroll");
    }
    if (hDelta != 0) {
        INPUT input{};
        input.type         = INPUT_MOUSE;
        input.mi.dwFlags   = MOUSEEVENTF_HWHEEL;
        input.mi.mouseData = static_cast<DWORD>(hDelta);
        Send(input, "trackpad-hscroll");
    }
}

void WinInputInjector::Button(MouseButtonId button, bool down) {
    INPUT input{};
    input.type = INPUT_MOUSE;
    switch (button) {
    case MouseButtonId::Left:   input.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN   : MOUSEEVENTF_LEFTUP;   break;
    case MouseButtonId::Right:  input.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN  : MOUSEEVENTF_RIGHTUP;  break;
    case MouseButtonId::Middle: input.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP; break;
    case MouseButtonId::X1:
        input.mi.dwFlags   = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
        input.mi.mouseData = XBUTTON1;
        break;
    case MouseButtonId::X2:
        input.mi.dwFlags   = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
        input.mi.mouseData = XBUTTON2;
        break;
    }
    Send(input, "paddle-mouse");
}

void WinInputInjector::Key(uint16_t keyId, bool down) {
    if (keyId == 0) return;
    const ScanCode sc = ScanCodeFor(keyId);
    if (sc.scan == 0) return;

    INPUT input = {};
    input.type       = INPUT_KEYBOARD;
    input.ki.wVk     = 0;  // ignored, and must be zero, when sending a scan code
    input.ki.wScan   = sc.scan;
    input.ki.dwFlags = KEYEVENTF_SCANCODE;
    if (sc.extended) input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    if (!down)       input.ki.dwFlags |= KEYEVENTF_KEYUP;
    Send(input, "key");
}

std::string WinInputInjector::KeyDisplayName(uint16_t keyId) const {
    if (keyId == 0) return "Key";
    const ScanCode sc = ScanCodeFor(keyId);
    if (sc.scan == 0) return "Key";

    LONG lParam = static_cast<LONG>(sc.scan) << 16;
    if (sc.extended) lParam |= (1L << 24);

    wchar_t buf[64] = {};
    const int n = GetKeyNameTextW(lParam, buf, static_cast<int>(std::size(buf)));
    return n > 0 ? WideToUtf8(std::wstring(buf, static_cast<size_t>(n))) : std::string("Key");
}

std::chrono::milliseconds WinInputInjector::KeyRepeatDelay() const {
    // 0-3, meaning roughly 250ms to 1000ms in 250ms steps.
    DWORD setting = 1;
    if (!SystemParametersInfoW(SPI_GETKEYBOARDDELAY, 0, &setting, 0) || setting > 3)
        setting = 1;
    return std::chrono::milliseconds(250 * (static_cast<int>(setting) + 1));
}

std::chrono::milliseconds WinInputInjector::KeyRepeatInterval() const {
    // 0-31, meaning roughly 2.5 to 30 repeats per second. Windows documents
    // only the endpoints, so interpolate the rate between them.
    DWORD setting = 31;
    if (!SystemParametersInfoW(SPI_GETKEYBOARDSPEED, 0, &setting, 0) || setting > 31)
        setting = 31;
    const double perSecond = 2.5 + (30.0 - 2.5) * (static_cast<double>(setting) / 31.0);
    return std::chrono::milliseconds(static_cast<long long>(1000.0 / perSecond + 0.5));
}

bool WinInputInjector::IsPhysicallyHeld(uint16_t keyId) const {
    // The merged virtual key, not the left/right one: the user may be holding
    // the right-hand key while this sends the left, and either satisfies the
    // shortcut.
    uint16_t merged = keyId;
    switch (keyId) {
    case VK_LCONTROL: merged = VK_CONTROL; break;
    case VK_LMENU:    merged = VK_MENU;    break;
    case VK_LSHIFT:   merged = VK_SHIFT;   break;
    default: break;
    }
    return (GetAsyncKeyState(merged) & 0x8000) != 0;
}

CursorProbe WinInputInjector::ProbeCursor() const {
    POINT pt{};
    CursorProbe probe;
    probe.available = GetCursorPos(&pt) != FALSE;
    probe.x = pt.x;
    probe.y = pt.y;
    return probe;
}

void WinInputInjector::LogEnvironment(const char* reason) {
    DWORD fgIntegrity = kIntegrityUnknown;
    const std::wstring fg   = DescribeForeground(fgIntegrity);
    const DWORD        own  = OwnIntegrity();
    const std::wstring clip = DescribeClip();
    const std::wstring cur  = DescribeCursor();

    const bool outranked = fgIntegrity != kIntegrityUnknown
                        && own         != kIntegrityUnknown
                        && fgIntegrity > own;

    EventLog::Write("INJECT: %s — foreground=%s, us=integrity=%s%s, "
                    "cursorClip=%s, cursor=%s",
                    reason, WideToUtf8(fg).c_str(), WideToUtf8(IntegrityName(own)).c_str(),
                    outranked ? " — FOREGROUND OUTRANKS US, injected input is "
                                "blocked (UIPI) until focus moves or it closes"
                              : "",
                    WideToUtf8(clip).c_str(), WideToUtf8(cur).c_str());
}

void WinInputInjector::LogCursorNotMoving(long travelPx, long x, long y) {
    {
        std::lock_guard<std::mutex> lk(m_stateMutex);
        if (!m_blockerLogged.empty()) return;
    }
    if (!DueForLog(g_lastStuckLogMs)) return;
    EventLog::Write("INJECT: %ld px of cursor movement accepted but the cursor "
                    "has not moved from (%ld,%ld) — something is discarding "
                    "injected motion (cursor clip, or a low-level hook)",
                    travelPx, x, y);
    LogEnvironment("state while cursor is stuck");
}
