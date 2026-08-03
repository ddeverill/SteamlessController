#include "SteamStrategy.h"
#include "EventLog.h"
#include <Windows.h>
#include <TlHelp32.h>
#include <shellapi.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <vector>

namespace {

constexpr wchar_t APP_REG_KEY[] = L"Software\\SteamlessController";
constexpr wchar_t STEAM_REG_KEY[] = L"Software\\Valve\\Steam";
constexpr wchar_t RUN_REG_KEY[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t STEAM_RUN_VALUE[] = L"Steam";
constexpr wchar_t STRATEGY_VALUE[] = L"SteamStrategy";
constexpr std::array<const char*, 3> STEAM_CONTROLLER_IDS = {
    "28de/1302", "28de/1303", "28de/1304"
};

bool ReadRegistryString(HKEY root, const wchar_t* keyName, const wchar_t* valueName,
                        std::wstring& value, DWORD* typeOut = nullptr) {
    DWORD type = 0;
    DWORD bytes = 0;
    if (RegGetValueW(root, keyName, valueName, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
                     &type, nullptr, &bytes) != ERROR_SUCCESS
            || bytes < sizeof(wchar_t))
        return false;

    value.resize(bytes / sizeof(wchar_t));
    if (RegGetValueW(root, keyName, valueName, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
                     &type, value.data(), &bytes) != ERROR_SUCCESS)
        return false;
    while (!value.empty() && value.back() == L'\0') value.pop_back();
    if (typeOut) *typeOut = type;
    return !value.empty();
}

bool ReadFileBytes(const std::wstring& path, std::string& bytes) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0
            || size.QuadPart > static_cast<LONGLONG>(64 * 1024 * 1024)) {
        CloseHandle(file);
        return false;
    }

    bytes.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const bool ok = bytes.empty()
        || (ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr)
            && read == static_cast<DWORD>(bytes.size()));
    CloseHandle(file);
    return ok;
}

bool WriteFileAtomically(const std::wstring& path, const std::string& bytes) {
    const std::wstring temporary = path + L".steamlesscontroller.tmp";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    const bool wrote = bytes.empty()
        || (WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)
            && written == static_cast<DWORD>(bytes.size()));
    const bool flushed = wrote && FlushFileBuffers(file);
    CloseHandle(file);
    if (!flushed) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

std::string LowerAscii(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return text;
}

std::string TrimAscii(std::string text) {
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const size_t last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

bool IsSteamControllerId(const std::string& value) {
    const std::string lower = LowerAscii(TrimAscii(value));
    return std::any_of(STEAM_CONTROLLER_IDS.begin(), STEAM_CONTROLLER_IDS.end(),
                       [&](const char* id) { return lower == id; });
}

struct VdfValue {
    bool   found = false;
    size_t start = 0;
    size_t end = 0;
    std::string value;
};

VdfValue FindControllerBlacklist(const std::string& config) {
    const std::string lower = LowerAscii(config);
    constexpr char key[] = "\"controller_blacklist\"";
    const size_t keyPos = lower.rfind(key);
    if (keyPos == std::string::npos) return {};
    const size_t open = config.find('"', keyPos + sizeof(key) - 1);
    if (open == std::string::npos) return {};
    const size_t close = config.find('"', open + 1);
    if (close == std::string::npos) return {};
    return {true, open + 1, close, config.substr(open + 1, close - open - 1)};
}

std::string UpdateControllerBlacklist(std::string config, bool addControllers) {
    const VdfValue existing = FindControllerBlacklist(config);
    std::vector<std::string> entries;
    if (existing.found) {
        size_t start = 0;
        while (start <= existing.value.size()) {
            const size_t comma = existing.value.find(',', start);
            std::string entry = TrimAscii(existing.value.substr(
                start, comma == std::string::npos ? std::string::npos : comma - start));
            if (!entry.empty() && !IsSteamControllerId(entry))
                entries.push_back(std::move(entry));
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }
    if (addControllers)
        for (const char* id : STEAM_CONTROLLER_IDS) entries.emplace_back(id);

    std::string value;
    for (const std::string& entry : entries) {
        if (!value.empty()) value += ',';
        value += entry;
    }

    if (existing.found) {
        config.replace(existing.start, existing.end - existing.start, value);
        return config;
    }
    if (!addControllers) return config;

    const size_t close = config.find_last_of('}');
    if (close == std::string::npos) return {};
    const char* newline = config.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    std::string property = std::string("\t\"controller_blacklist\"\t\t\"")
                         + value + "\"" + newline;
    config.insert(close, property);
    return config;
}

bool GetSteamPaths(std::wstring& steamPath, std::wstring& steamExe,
                   std::wstring& configPath) {
    if (!ReadRegistryString(HKEY_CURRENT_USER, STEAM_REG_KEY, L"SteamPath", steamPath))
        return false;
    std::replace(steamPath.begin(), steamPath.end(), L'/', L'\\');
    while (!steamPath.empty() && steamPath.back() == L'\\') steamPath.pop_back();
    steamExe = steamPath + L"\\steam.exe";
    configPath = steamPath + L"\\config\\config.vdf";
    return GetFileAttributesW(steamExe.c_str()) != INVALID_FILE_ATTRIBUTES
        && GetFileAttributesW(configPath.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool IsSteamProcessRunning() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool running = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"steam.exe") == 0) {
                running = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return running;
}

std::wstring QuoteArgument(const std::wstring& argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring::npos)
        return argument;

    std::wstring quoted = L"\"";
    size_t backslashes = 0;
    for (const wchar_t ch : argument) {
        if (ch == L'\\') {
            ++backslashes;
        } else if (ch == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted += L'"';
            backslashes = 0;
        } else {
            quoted.append(backslashes, L'\\');
            backslashes = 0;
            quoted += ch;
        }
    }
    quoted.append(backslashes * 2, L'\\');
    quoted += L'"';
    return quoted;
}

std::vector<std::wstring> ParseArguments(const std::wstring& command) {
    int count = 0;
    LPWSTR* argv = CommandLineToArgvW(command.c_str(), &count);
    if (!argv) return {};
    std::vector<std::wstring> args;
    for (int i = 0; i < count; ++i) args.emplace_back(argv[i]);
    LocalFree(argv);
    return args;
}

std::wstring SteamCommand(const std::wstring& steamExe, const std::wstring& existing,
                          bool noJoy) {
    std::vector<std::wstring> args = ParseArguments(existing);
    if (!args.empty()) args.erase(args.begin());
    args.erase(std::remove_if(args.begin(), args.end(), [](const std::wstring& arg) {
        return _wcsicmp(arg.c_str(), L"-nojoy") == 0;
    }), args.end());
    if (noJoy) args.emplace_back(L"-nojoy");
    if (args.empty()) args.emplace_back(L"-silent");

    std::wstring command = QuoteArgument(steamExe);
    for (const std::wstring& arg : args)
        command += L" " + QuoteArgument(arg);
    return command;
}

bool WriteSteamRunCommandIfPresent(const std::wstring& command, std::wstring& error) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_REG_KEY, 0, KEY_QUERY_VALUE | KEY_SET_VALUE,
                      &key) != ERROR_SUCCESS) {
        error = L"Could not open the Windows startup registry key.";
        return false;
    }

    const bool exists = RegQueryValueExW(key, STEAM_RUN_VALUE, nullptr, nullptr,
                                         nullptr, nullptr) == ERROR_SUCCESS;
    bool ok = true;
    if (exists) {
        ok = RegSetValueExW(key, STEAM_RUN_VALUE, 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(command.c_str()),
                            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)))
             == ERROR_SUCCESS;
        if (!ok) error = L"Could not update Steam's Windows startup command.";
    }
    RegCloseKey(key);
    return ok;
}

bool SaveStrategy(SteamStrategy strategy) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, APP_REG_KEY, 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key, nullptr)
            != ERROR_SUCCESS)
        return false;
    const DWORD value = static_cast<DWORD>(strategy);
    const bool ok = RegSetValueExW(key, STRATEGY_VALUE, 0, REG_DWORD,
                                   reinterpret_cast<const BYTE*>(&value), sizeof(value))
                    == ERROR_SUCCESS;
    RegCloseKey(key);
    return ok;
}

bool StopSteam(const std::wstring& steamExe, std::wstring& error) {
    if (!IsSteamProcessRunning()) return true;
    std::wstring command = QuoteArgument(steamExe) + L" -shutdown";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, nullptr, &si, &pi)) {
        error = L"Steam could not be asked to shut down.";
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    for (int waited = 0; waited < 30000; waited += 200) {
        if (!IsSteamProcessRunning()) return true;
        Sleep(200);
    }
    error = L"Steam did not finish shutting down, so no settings were changed.";
    return false;
}

bool StartSteam(std::wstring command, std::wstring& error) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, nullptr, &si, &pi)) {
        error = L"The strategy was saved, but Steam could not be restarted.";
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

bool ConfigContainsSteamController(const std::wstring& configPath) {
    std::string config;
    if (!ReadFileBytes(configPath, config)) return false;
    const VdfValue value = FindControllerBlacklist(config);
    if (!value.found) return false;
    size_t start = 0;
    while (start <= value.value.size()) {
        const size_t comma = value.value.find(',', start);
        if (IsSteamControllerId(value.value.substr(
                start, comma == std::string::npos ? std::string::npos : comma - start)))
            return true;
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return false;
}

bool CommandHasNoJoy(const std::wstring& command) {
    const std::vector<std::wstring> args = ParseArguments(command);
    return std::any_of(args.begin(), args.end(), [](const std::wstring& arg) {
        return _wcsicmp(arg.c_str(), L"-nojoy") == 0;
    });
}

} // namespace

namespace SteamStrategies {

SteamStrategy DetectConfigured() {
    DWORD saved = 0;
    DWORD bytes = sizeof(saved);
    if (RegGetValueW(HKEY_CURRENT_USER, APP_REG_KEY, STRATEGY_VALUE,
                     RRF_RT_REG_DWORD, nullptr, &saved, &bytes) == ERROR_SUCCESS
            && saved <= static_cast<DWORD>(SteamStrategy::NoJoy))
        return static_cast<SteamStrategy>(saved);

    std::wstring startup;
    if (ReadRegistryString(HKEY_CURRENT_USER, RUN_REG_KEY, STEAM_RUN_VALUE, startup)
            && CommandHasNoJoy(startup))
        return SteamStrategy::NoJoy;

    std::wstring steamPath, steamExe, configPath;
    if (GetSteamPaths(steamPath, steamExe, configPath)
            && ConfigContainsSteamController(configPath))
        return SteamStrategy::ControllerBlacklist;
    return SteamStrategy::YieldToSteam;
}

ApplyResult ApplyAndRestart(SteamStrategy strategy) {
    std::wstring steamPath, steamExe, configPath;
    if (!GetSteamPaths(steamPath, steamExe, configPath))
        return {false, L"Steam's installation or config.vdf could not be found."};

    std::wstring existingRun;
    const bool steamRunExists = ReadRegistryString(
        HKEY_CURRENT_USER, RUN_REG_KEY, STEAM_RUN_VALUE, existingRun);
    const std::wstring previousLaunchCommand = SteamCommand(
        steamExe, existingRun, CommandHasNoJoy(existingRun));
    const std::wstring launchCommand = SteamCommand(
        steamExe, existingRun, strategy == SteamStrategy::NoJoy);

    std::wstring error;
    if (!StopSteam(steamExe, error)) return {false, std::move(error)};
    auto failAfterStop = [&](std::wstring message) {
        std::wstring restartError;
        if (!StartSteam(previousLaunchCommand, restartError)) {
            message += L" Steam also could not be restarted with its previous settings.";
        }
        return ApplyResult{false, std::move(message)};
    };

    std::string original;
    if (!ReadFileBytes(configPath, original))
        return failAfterStop(L"Steam was stopped, but config.vdf could not be read.");
    const std::wstring backupPath = configPath + L".steamlesscontroller.bak";
    if (GetFileAttributesW(backupPath.c_str()) == INVALID_FILE_ATTRIBUTES
            && !CopyFileW(configPath.c_str(), backupPath.c_str(), TRUE))
        return failAfterStop(L"Steam was stopped, but config.vdf could not be backed up.");

    const std::string updated = UpdateControllerBlacklist(
        original, strategy == SteamStrategy::ControllerBlacklist);
    if (updated.empty())
        return failAfterStop(L"Steam was stopped, but config.vdf was not valid VDF.");
    if (updated != original && !WriteFileAtomically(configPath, updated))
        return failAfterStop(L"Steam was stopped, but config.vdf could not be updated.");

    if (!WriteSteamRunCommandIfPresent(launchCommand, error)) {
        if (updated != original) WriteFileAtomically(configPath, original);
        return failAfterStop(std::move(error));
    }
    if (!SaveStrategy(strategy)) {
        if (updated != original) WriteFileAtomically(configPath, original);
        if (steamRunExists) {
            std::wstring ignored;
            WriteSteamRunCommandIfPresent(existingRun, ignored);
        }
        return failAfterStop(L"The strategy could not be saved.");
    }

    EventLog::Write("STEAM STRATEGY: applied %d (0=yield 1=blacklist 2=nojoy)",
                    static_cast<int>(strategy));
    if (!StartSteam(launchCommand, error)) return {true, std::move(error)};
    return {true, {}};
}

bool IsGameRunning() {
    DWORD appId = 0;
    DWORD bytes = sizeof(appId);
    return RegGetValueW(HKEY_CURRENT_USER, STEAM_REG_KEY, L"RunningAppID",
                        RRF_RT_REG_DWORD, nullptr, &appId, &bytes) == ERROR_SUCCESS
        && appId != 0;
}

bool IsNoJoySelected() {
    return DetectConfigured() == SteamStrategy::NoJoy;
}

const wchar_t* Tooltip(SteamStrategy strategy) {
    switch (strategy) {
    case SteamStrategy::YieldToSteam:
        return L"Pros: least invasive; Steam Input, gyro, touch menus, and Big Picture "
               L"controller navigation keep working.\nCons: Steamless yields whenever Steam "
               L"can see the controller, so Game Pass needs Steam closed.";
    case SteamStrategy::ControllerBlacklist:
        return L"Pros: recommended for Game Pass; Steam stays open and other controller "
               L"types keep Steam Input.\nCons: Steam Controller layouts, gyro, and touch "
               L"menus in Steam are unavailable while it is hidden.";
    case SteamStrategy::NoJoy:
        return L"Pros: simple, driver-free, and Steam can stay open.\nCons: disables all "
               L"Steam client controller support, including Steam Input and Big Picture "
               L"controller navigation. Launching Steam without -nojoy bypasses it.";
    }
    return L"";
}

} // namespace SteamStrategies
