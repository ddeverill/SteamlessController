#pragma once
#include <Windows.h>
#include <string>

// Who owns a window.
//
// Both fields are filled where they apply: every process has an image path,
// and only a packaged (MSIX/UWP) one has an AUMID. A window we could not
// identify at all — a protected process, or one that closed while being
// asked — reports empty, which callers should read as "unknown", never as
// "the desktop".
struct ForegroundIdentity {
    std::wstring exePath;
    std::wstring aumid;

    bool Empty() const { return exePath.empty() && aumid.empty(); }
    bool operator==(const ForegroundIdentity& o) const {
        return _wcsicmp(exePath.c_str(), o.exePath.c_str()) == 0
            && _wcsicmp(aumid.c_str(),   o.aumid.c_str())   == 0;
    }
    bool operator!=(const ForegroundIdentity& o) const { return !(*this == o); }
};

// Resolving a window to the application behind it, shared by the two things
// that need to do it: ForegroundWatcher, which asks about whatever the user
// switched to, and the remap window's running-app picker, which asks the same
// question of every open window at once. Both have to handle packaged apps
// the same way, so the frame-host unwrapping below lives here rather than in
// either caller.
namespace ProcessIdentity {

// Image path and AUMID for a process id. Empty for a process we cannot open,
// which for an unelevated caller means a protected or elevated one.
ForegroundIdentity ForProcess(DWORD pid);

// The application a top-level window belongs to, with packaged apps resolved
// past their host — see the note on kFrameHostExe in the implementation.
ForegroundIdentity ForWindow(HWND hwnd);

}  // namespace ProcessIdentity
