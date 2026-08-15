#include "WinRegistryStore.h"
#include "core/Text.h"
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <iterator>

namespace {

constexpr wchar_t kBaseKey[] = L"Software\\SteamlessController";

std::wstring SubkeyPath(std::string_view section) {
    std::wstring path = kBaseKey;
    if (!section.empty()) {
        path += L'\\';
        std::string s(section);
        for (char& c : s) if (c == '/') c = '\\';
        path += Utf8ToWide(s);
    }
    return path;
}

}  // namespace

std::optional<uint32_t> WinRegistryStore::GetUInt(std::string_view section, std::string_view key) const {
    HKEY hkey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, SubkeyPath(section).c_str(), 0, KEY_READ, &hkey) != ERROR_SUCCESS)
        return std::nullopt;
    DWORD val = 0, size = sizeof(val);
    const bool ok = RegQueryValueExW(hkey, Utf8ToWide(std::string(key)).c_str(), nullptr, nullptr,
                                     reinterpret_cast<LPBYTE>(&val), &size) == ERROR_SUCCESS;
    RegCloseKey(hkey);
    return ok ? std::optional<uint32_t>(val) : std::nullopt;
}

std::optional<std::string> WinRegistryStore::GetString(std::string_view section, std::string_view key) const {
    HKEY hkey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, SubkeyPath(section).c_str(), 0, KEY_READ, &hkey) != ERROR_SUCCESS)
        return std::nullopt;
    wchar_t buf[1024] = {};
    DWORD size = sizeof(buf);
    const bool ok = RegQueryValueExW(hkey, Utf8ToWide(std::string(key)).c_str(), nullptr, nullptr,
                                     reinterpret_cast<LPBYTE>(buf), &size) == ERROR_SUCCESS;
    RegCloseKey(hkey);
    return ok ? std::optional<std::string>(WideToUtf8(buf)) : std::nullopt;
}

void WinRegistryStore::SetUInt(std::string_view section, std::string_view key, uint32_t value) {
    HKEY hkey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, SubkeyPath(section).c_str(), 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hkey, nullptr) != ERROR_SUCCESS)
        return;
    RegSetValueExW(hkey, Utf8ToWide(std::string(key)).c_str(), 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(hkey);
}

void WinRegistryStore::SetString(std::string_view section, std::string_view key, const std::string& value) {
    HKEY hkey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, SubkeyPath(section).c_str(), 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hkey, nullptr) != ERROR_SUCCESS)
        return;
    const std::wstring wval = Utf8ToWide(value);
    RegSetValueExW(hkey, Utf8ToWide(std::string(key)).c_str(), 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(wval.c_str()),
                   static_cast<DWORD>((wval.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(hkey);
}

std::vector<std::string> WinRegistryStore::ListSubsections(std::string_view section) const {
    std::vector<std::string> out;
    HKEY hkey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, SubkeyPath(section).c_str(), 0, KEY_READ, &hkey) != ERROR_SUCCESS)
        return out;
    for (DWORD i = 0;; ++i) {
        wchar_t name[256];
        DWORD nameLen = static_cast<DWORD>(std::size(name));
        if (RegEnumKeyExW(hkey, i, name, &nameLen, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
            break;
        out.push_back(WideToUtf8(name));
    }
    RegCloseKey(hkey);
    return out;
}

void WinRegistryStore::RemoveSection(std::string_view section) {
    // Subsections here (GameProfiles\<n>) have no children of their own, so a
    // plain per-key delete is enough — no recursive-delete dependency needed.
    const std::wstring full = SubkeyPath(section);
    const size_t lastSlash = full.find_last_of(L'\\');
    if (lastSlash == std::wstring::npos) return;
    const std::wstring parent = full.substr(0, lastSlash);
    const std::wstring child  = full.substr(lastSlash + 1);

    HKEY hkey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, parent.c_str(), 0, KEY_ALL_ACCESS, &hkey) != ERROR_SUCCESS)
        return;
    RegDeleteKeyW(hkey, child.c_str());
    RegCloseKey(hkey);
}
