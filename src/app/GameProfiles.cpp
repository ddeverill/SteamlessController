#include "GameProfiles.h"
#include <Windows.h>
#include <cstdio>

namespace {

constexpr wchar_t kProfilesKey[] = L"Software\\SteamlessController\\GameProfiles";

DWORD ReadDw(HKEY key, const wchar_t* name, DWORD def) {
    DWORD val = 0, size = sizeof(val);
    if (RegQueryValueExW(key, name, nullptr, nullptr,
                         reinterpret_cast<LPBYTE>(&val), &size) == ERROR_SUCCESS)
        return val;
    return def;
}

std::wstring ReadSz(HKEY key, const wchar_t* name) {
    wchar_t buf[MAX_PATH] = {};
    DWORD size = sizeof(buf);
    if (RegQueryValueExW(key, name, nullptr, nullptr,
                         reinterpret_cast<LPBYTE>(buf), &size) == ERROR_SUCCESS)
        return buf;
    return {};
}

void WriteDw(HKEY key, const wchar_t* name, DWORD val) {
    RegSetValueExW(key, name, 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&val), sizeof(val));
}

void WriteSz(HKEY key, const wchar_t* name, const std::wstring& val) {
    RegSetValueExW(key, name, 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(val.c_str()),
                   static_cast<DWORD>((val.size() + 1) * sizeof(wchar_t)));
}

}  // namespace

namespace GameProfiles {

std::map<std::wstring, ControllerProfile> Load() {
    std::map<std::wstring, ControllerProfile> profiles;

    HKEY parent;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kProfilesKey, 0, KEY_READ, &parent) != ERROR_SUCCESS)
        return profiles;

    const DWORD unbound = BackButtonBinding::FromAction(BackButtonAction::None).Pack();

    // Numbered subkeys ("0", "1", ...) rather than the game id itself — an
    // exe-path id is not a legal single registry key name, since its
    // backslashes would be read as key-hierarchy separators.
    for (DWORD i = 0;; ++i) {
        wchar_t sub[16];
        swprintf_s(sub, L"%lu", i);
        HKEY child;
        if (RegOpenKeyExW(parent, sub, 0, KEY_READ, &child) != ERROR_SUCCESS)
            break;

        const std::wstring id = ReadSz(child, L"Id");
        if (!id.empty()) {
            ControllerProfile p;
            p.displayName = ReadSz(child, L"Name");
            p.platform = ReadDw(child, L"Platform", 0) != 0 ? ControllerPlatform::PlayStation
                                                            : ControllerPlatform::Xbox;
            p.back.l4 = BackButtonBinding::Unpack(ReadDw(child, L"L4", unbound));
            p.back.l5 = BackButtonBinding::Unpack(ReadDw(child, L"L5", unbound));
            p.back.r4 = BackButtonBinding::Unpack(ReadDw(child, L"R4", unbound));
            p.back.r5 = BackButtonBinding::Unpack(ReadDw(child, L"R5", unbound));
            // Profiles written before per-pad settings existed have none of
            // these values; the defaults leave both pads unclaimed, which is
            // exactly how those profiles behaved.
            p.leftPad.mode       = TrackpadModeFromDword(ReadDw(child, L"LeftPadMode",  0));
            p.leftPad.click      = BackButtonBinding::Unpack(ReadDw(child, L"LeftPadClick",  unbound));
            p.leftPad.scrollDir  = ScrollDirectionFromDword(ReadDw(child, L"LeftPadScrollDir",  0));
            p.rightPad.mode      = TrackpadModeFromDword(ReadDw(child, L"RightPadMode", 0));
            p.rightPad.click     = BackButtonBinding::Unpack(ReadDw(child, L"RightPadClick", unbound));
            p.rightPad.scrollDir = ScrollDirectionFromDword(ReadDw(child, L"RightPadScrollDir", 0));
            profiles[id] = p;
        }
        RegCloseKey(child);
    }

    RegCloseKey(parent);
    return profiles;
}

void Save(const std::map<std::wstring, ControllerProfile>& profiles) {
    HKEY parent;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kProfilesKey, 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_ALL_ACCESS, nullptr,
                        &parent, nullptr) != ERROR_SUCCESS)
        return;

    DWORD i = 0;
    for (const auto& [id, p] : profiles) {
        wchar_t sub[16];
        swprintf_s(sub, L"%lu", i);
        HKEY child;
        if (RegCreateKeyExW(parent, sub, 0, nullptr, REG_OPTION_NON_VOLATILE,
                            KEY_WRITE, nullptr, &child, nullptr) == ERROR_SUCCESS) {
            WriteSz(child, L"Id", id);
            WriteSz(child, L"Name", p.displayName);
            WriteDw(child, L"Platform",
                    p.platform == ControllerPlatform::PlayStation ? 1 : 0);
            WriteDw(child, L"L4", p.back.l4.Pack());
            WriteDw(child, L"L5", p.back.l5.Pack());
            WriteDw(child, L"R4", p.back.r4.Pack());
            WriteDw(child, L"R5", p.back.r5.Pack());
            WriteDw(child, L"LeftPadMode",       static_cast<DWORD>(p.leftPad.mode));
            WriteDw(child, L"LeftPadClick",      p.leftPad.click.Pack());
            WriteDw(child, L"LeftPadScrollDir",  static_cast<DWORD>(p.leftPad.scrollDir));
            WriteDw(child, L"RightPadMode",      static_cast<DWORD>(p.rightPad.mode));
            WriteDw(child, L"RightPadClick",     p.rightPad.click.Pack());
            WriteDw(child, L"RightPadScrollDir", static_cast<DWORD>(p.rightPad.scrollDir));
            RegCloseKey(child);
        }
        ++i;
    }

    // Numbered subkeys have no children of their own, so dropping whatever
    // range existed past the current count is a plain per-key delete — no
    // recursive-delete dependency needed when the map has shrunk.
    for (;; ++i) {
        wchar_t sub[16];
        swprintf_s(sub, L"%lu", i);
        if (RegDeleteKeyW(parent, sub) != ERROR_SUCCESS) break;
    }

    RegCloseKey(parent);
}

}  // namespace GameProfiles
