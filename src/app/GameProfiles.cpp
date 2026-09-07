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

DWORD Packed(BackButtonAction a) {
    return BackButtonBinding::FromAction(a).Pack();
}

// One pad's values. Named rather than spelled out twice because the two pads
// carry the same twelve settings and only the prefix differs, and a
// copy-pasted second copy is where a left-pad name ends up reading a
// right-pad value.
void ReadPad(HKEY key, const wchar_t* prefix, TrackpadSettings& pad) {
    auto name = [&](const wchar_t* suffix) { return std::wstring(prefix) + suffix; };
    auto binding = [&](const wchar_t* suffix, BackButtonAction def) {
        return BackButtonBinding::Unpack(ReadDw(key, name(suffix).c_str(), Packed(def)));
    };

    pad.mode      = TrackpadModeFromDword(ReadDw(key, name(L"Mode").c_str(), 0));
    pad.click     = binding(L"Click", BackButtonAction::None);
    pad.scrollDir = ScrollDirectionFromDword(ReadDw(key, name(L"ScrollDir").c_str(), 0));
    // Zero means absent, which ScrollSpeedFromDword reads as the default.
    pad.scrollSpeed = ScrollSpeedFromDword(ReadDw(key, name(L"ScrollSpeed").c_str(), 0));
    // Absent from every profile written before the directional modes existed.
    // The defaults match TrackpadSettings' own, so those profiles read back as
    // an unconfigured directional pad rather than a broken one.
    pad.touch     = binding(L"Touch", BackButtonAction::None);
    pad.up        = binding(L"Up",    BackButtonAction::DPadUp);
    pad.down      = binding(L"Down",  BackButtonAction::DPadDown);
    pad.left      = binding(L"Left",  BackButtonAction::DPadLeft);
    pad.right     = binding(L"Right", BackButtonAction::DPadRight);
    pad.diagonals = DiagonalModeFromDword(ReadDw(key, name(L"Diagonals").c_str(), 0));
}

void WritePad(HKEY key, const wchar_t* prefix, const TrackpadSettings& pad) {
    auto name = [&](const wchar_t* suffix) { return std::wstring(prefix) + suffix; };

    WriteDw(key, name(L"Mode").c_str(),      static_cast<DWORD>(pad.mode));
    WriteDw(key, name(L"Click").c_str(),     pad.click.Pack());
    WriteDw(key, name(L"ScrollDir").c_str(), static_cast<DWORD>(pad.scrollDir));
    WriteDw(key, name(L"ScrollSpeed").c_str(), pad.scrollSpeed);
    WriteDw(key, name(L"Touch").c_str(),     pad.touch.Pack());
    WriteDw(key, name(L"Up").c_str(),        pad.up.Pack());
    WriteDw(key, name(L"Down").c_str(),      pad.down.Pack());
    WriteDw(key, name(L"Left").c_str(),      pad.left.Pack());
    WriteDw(key, name(L"Right").c_str(),     pad.right.Pack());
    WriteDw(key, name(L"Diagonals").c_str(), static_cast<DWORD>(pad.diagonals));
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
            // Absent on every profile written before followers existed, and 0
            // is "has its own controls" precisely so those keep theirs.
            p.useDefaultMappings = ReadDw(child, L"UseDefaultMappings", 0) != 0;
            p.platform = ReadDw(child, L"Platform", 0) != 0 ? ControllerPlatform::PlayStation
                                                            : ControllerPlatform::Xbox;
            p.back.l4 = BackButtonBinding::Unpack(ReadDw(child, L"L4", unbound));
            p.back.l5 = BackButtonBinding::Unpack(ReadDw(child, L"L5", unbound));
            p.back.r4 = BackButtonBinding::Unpack(ReadDw(child, L"R4", unbound));
            p.back.r5 = BackButtonBinding::Unpack(ReadDw(child, L"R5", unbound));
            // Profiles written before per-pad settings existed have none of
            // these values; the defaults leave both pads unclaimed, which is
            // exactly how those profiles behaved.
            ReadPad(child, L"LeftPad",  p.leftPad);
            ReadPad(child, L"RightPad", p.rightPad);
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
            // The controls below are written either way, so a profile that
            // stops following the default still has whatever it had before.
            WriteDw(child, L"UseDefaultMappings", p.useDefaultMappings ? 1 : 0);
            WriteDw(child, L"Platform",
                    p.platform == ControllerPlatform::PlayStation ? 1 : 0);
            WriteDw(child, L"L4", p.back.l4.Pack());
            WriteDw(child, L"L5", p.back.l5.Pack());
            WriteDw(child, L"R4", p.back.r4.Pack());
            WriteDw(child, L"R5", p.back.r5.Pack());
            WritePad(child, L"LeftPad",  p.leftPad);
            WritePad(child, L"RightPad", p.rightPad);
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
