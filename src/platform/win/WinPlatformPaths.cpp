#include "WinPlatformPaths.h"
#include "core/Text.h"
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cwctype>

std::string WinPlatformPaths::StateDir() const {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::wstring dir(buf);
    dir += L"\\SteamlessController";
    return WideToUtf8(dir);
}

std::string WinPlatformPaths::ConfigDir() const {
    // Settings live in the registry (WinRegistryStore) — nothing reads this.
    return StateDir();
}

std::vector<std::string> WinPlatformPaths::SteamRoots() const {
    // Written by the Steam client for the current user; the machine-wide key
    // exists too but points at the installer's idea of the path, not the
    // user's.
    wchar_t buf[MAX_PATH] = {};
    DWORD   size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath",
                     RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return {};

    std::wstring path(buf);
    // Steam stores this with forward slashes and in lower case.
    for (wchar_t& c : path) if (c == L'/') c = L'\\';
    while (!path.empty() && path.back() == L'\\') path.pop_back();
    if (path.empty()) return {};
    return { WideToUtf8(path) };
}

std::string WinPlatformPaths::PathKey(const std::string& path) const {
    std::wstring wide = Utf8ToWide(path);
    for (wchar_t& c : wide) c = static_cast<wchar_t>(towlower(c));
    return WideToUtf8(wide);
}
