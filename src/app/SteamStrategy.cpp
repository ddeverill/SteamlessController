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
constexpr wchar_t APP_RUN_VALUE[] = L"SteamlessController";
constexpr wchar_t STRATEGY_VALUE[] = L"SteamStrategy";
constexpr wchar_t BASELINE_CAPTURED_VALUE[] = L"SteamBaselineCaptured";
constexpr wchar_t BASELINE_BLACKLIST_VALUE[] = L"SteamBaselineBlacklistMask";
constexpr wchar_t BASELINE_NOJOY_VALUE[] = L"SteamBaselineNoJoy";
constexpr std::array<const char*, 3> STEAM_CONTROLLER_IDS = {
    "28de/1302", "28de/1303", "28de/1304"
};
constexpr DWORD ALL_STEAM_CONTROLLER_IDS =
    (1u << STEAM_CONTROLLER_IDS.size()) - 1u;

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

DWORD SteamControllerIdBit(const std::string& value) {
    const std::string lower = LowerAscii(TrimAscii(value));
    for (size_t i = 0; i < STEAM_CONTROLLER_IDS.size(); ++i)
        if (lower == STEAM_CONTROLLER_IDS[i]) return 1u << i;
    return 0;
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

DWORD ControllerBlacklistValueMask(const std::string& value) {
    DWORD mask = 0;
    size_t start = 0;
    while (start <= value.size()) {
        const size_t comma = value.find(',', start);
        mask |= SteamControllerIdBit(value.substr(
            start, comma == std::string::npos ? std::string::npos : comma - start));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return mask;
}

DWORD ControllerBlacklistConfigMask(const std::string& config) {
    const VdfValue value = FindControllerBlacklist(config);
    return value.found ? ControllerBlacklistValueMask(value.value) : 0;
}

std::string UpdateControllerBlacklist(std::string config, DWORD desiredControllerMask) {
    const VdfValue existing = FindControllerBlacklist(config);
    std::vector<std::string> entries;
    if (existing.found) {
        size_t start = 0;
        while (start <= existing.value.size()) {
            const size_t comma = existing.value.find(',', start);
            std::string entry = TrimAscii(existing.value.substr(
                start, comma == std::string::npos ? std::string::npos : comma - start));
            if (!entry.empty() && SteamControllerIdBit(entry) == 0)
                entries.push_back(std::move(entry));
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }
    for (size_t i = 0; i < STEAM_CONTROLLER_IDS.size(); ++i)
        if ((desiredControllerMask & (1u << i)) != 0)
            entries.emplace_back(STEAM_CONTROLLER_IDS[i]);

    std::string value;
    for (const std::string& entry : entries) {
        if (!value.empty()) value += ',';
        value += entry;
    }

    if (existing.found) {
        config.replace(existing.start, existing.end - existing.start, value);
        return config;
    }
    if (desiredControllerMask == 0) return config;

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

bool CommandHasNoJoy(const std::wstring& command) {
    const std::vector<std::wstring> args = ParseArguments(command);
    return std::any_of(args.begin(), args.end(), [](const std::wstring& arg) {
        return _wcsicmp(arg.c_str(), L"-nojoy") == 0;
    });
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

bool ReadAppDword(const wchar_t* name, DWORD& value) {
    DWORD bytes = sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER, APP_REG_KEY, name, RRF_RT_REG_DWORD,
                        nullptr, &value, &bytes) == ERROR_SUCCESS;
}

bool WriteAppDword(HKEY key, const wchar_t* name, DWORD value) {
    return RegSetValueExW(key, name, 0, REG_DWORD,
                          reinterpret_cast<const BYTE*>(&value), sizeof(value))
        == ERROR_SUCCESS;
}

bool HasSavedStrategy() {
    DWORD strategy = 0;
    return ReadAppDword(STRATEGY_VALUE, strategy)
        && strategy <= static_cast<DWORD>(SteamStrategy::NoJoy);
}

struct SteamBaseline {
    DWORD blacklistMask = 0;
    bool  noJoy = false;
};

bool LoadBaseline(SteamBaseline& baseline) {
    DWORD captured = 0;
    DWORD noJoy = 0;
    if (!ReadAppDword(BASELINE_CAPTURED_VALUE, captured) || captured == 0
            || !ReadAppDword(BASELINE_BLACKLIST_VALUE, baseline.blacklistMask)
            || !ReadAppDword(BASELINE_NOJOY_VALUE, noJoy))
        return false;
    baseline.noJoy = noJoy != 0;
    return true;
}

bool SaveBaseline(const SteamBaseline& baseline) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, APP_REG_KEY, 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key, nullptr)
            != ERROR_SUCCESS)
        return false;

    // Captured is written last so a partial write is never treated as a
    // complete restore point.
    const bool ok = WriteAppDword(key, BASELINE_BLACKLIST_VALUE, baseline.blacklistMask)
        && WriteAppDword(key, BASELINE_NOJOY_VALUE, baseline.noJoy ? 1 : 0)
        && WriteAppDword(key, BASELINE_CAPTURED_VALUE, 1);
    RegCloseKey(key);
    return ok;
}

bool EnsureBaseline(const std::string& currentConfig, const std::wstring& startupCommand,
                    SteamBaseline& baseline) {
    if (LoadBaseline(baseline)) return true;

    // a1f9b80 could already have applied a strategy before baseline tracking
    // existed. That version explicitly managed all three Valve IDs and
    // -nojoy, so migrate them as Steamless-owned. Fresh installs capture the
    // actual pre-management state and restore it on uninstall.
    const bool migrating = HasSavedStrategy();
    baseline.blacklistMask = migrating ? 0 : ControllerBlacklistConfigMask(currentConfig);
    baseline.noJoy = !migrating && CommandHasNoJoy(startupCommand);
    return SaveBaseline(baseline);
}

void DeleteSteamlessStartupAndSettings() {
    HKEY runKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_REG_KEY, 0, KEY_SET_VALUE, &runKey)
            == ERROR_SUCCESS) {
        RegDeleteValueW(runKey, APP_RUN_VALUE);
        RegCloseKey(runKey);
    }
    RegDeleteTreeW(HKEY_CURRENT_USER, APP_REG_KEY);
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
    return ControllerBlacklistConfigMask(config) != 0;
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
    const bool previousNoJoy = CommandHasNoJoy(existingRun);
    const std::wstring previousLaunchCommand = SteamCommand(
        steamExe, existingRun, previousNoJoy);
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

    SteamBaseline baseline;
    if (!EnsureBaseline(original, existingRun, baseline))
        return failAfterStop(L"Steam was stopped, but its original settings could not be saved.");

    const DWORD desiredBlacklist = strategy == SteamStrategy::ControllerBlacklist
        ? ALL_STEAM_CONTROLLER_IDS
        : strategy == SteamStrategy::YieldToSteam ? baseline.blacklistMask : 0;
    const bool desiredNoJoy = strategy == SteamStrategy::NoJoy
        || (strategy == SteamStrategy::YieldToSteam && baseline.noJoy);
    const std::wstring launchCommand = SteamCommand(steamExe, existingRun, desiredNoJoy);
    const std::string updated = UpdateControllerBlacklist(original, desiredBlacklist);
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

ApplyResult CleanupForUninstall() {
    SteamBaseline baseline;
    const bool hasBaseline = LoadBaseline(baseline);
    const bool hasStrategy = HasSavedStrategy();
    std::wstring steamPath, steamExe, configPath;
    if (!GetSteamPaths(steamPath, steamExe, configPath)) {
        // Steam itself is gone, so there is no persistent Steam config left to
        // restore. Still remove SteamlessController's per-user state.
        DeleteSteamlessStartupAndSettings();
        return {true, {}};
    }
    const std::wstring backupPath = configPath + L".steamlesscontroller.bak";
    if (!hasBaseline && !hasStrategy) {
        DeleteFileW(backupPath.c_str());
        DeleteSteamlessStartupAndSettings();
        return {true, {}};
    }

    std::wstring existingRun;
    ReadRegistryString(HKEY_CURRENT_USER, RUN_REG_KEY, STEAM_RUN_VALUE, existingRun);
    const bool steamWasRunning = IsSteamProcessRunning();
    const std::wstring previousLaunchCommand = SteamCommand(
        steamExe, existingRun, CommandHasNoJoy(existingRun));
    std::wstring error;
    if (!StopSteam(steamExe, error)) return {false, std::move(error)};
    auto failAfterStop = [&](std::wstring message) {
        if (steamWasRunning) {
            std::wstring restartError;
            if (!StartSteam(previousLaunchCommand, restartError))
                message += L" Steam also could not be restarted with its previous settings.";
        }
        return ApplyResult{false, std::move(message)};
    };

    std::string original;
    if (!ReadFileBytes(configPath, original))
        return failAfterStop(L"Steam was stopped, but config.vdf could not be read for cleanup.");
    if (!EnsureBaseline(original, existingRun, baseline))
        return failAfterStop(L"Steam was stopped, but its original settings could not be recovered.");

    const std::string restored = UpdateControllerBlacklist(original, baseline.blacklistMask);
    if (restored.empty() || (restored != original && !WriteFileAtomically(configPath, restored)))
        return failAfterStop(L"Steam was stopped, but its controller blacklist could not be restored.");

    const std::wstring restoredRun = SteamCommand(steamExe, existingRun, baseline.noJoy);
    if (!WriteSteamRunCommandIfPresent(restoredRun, error)) {
        if (restored != original) WriteFileAtomically(configPath, original);
        return failAfterStop(std::move(error));
    }

    EventLog::Write("STEAM STRATEGY: restored pre-Steamless settings for uninstall");
    DeleteFileW(backupPath.c_str());
    DeleteSteamlessStartupAndSettings();
    if (steamWasRunning && !StartSteam(restoredRun, error))
        return {true, std::move(error)};
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
        return L"Restores the Steam Controller blacklist and -nojoy state recorded before "
               L"SteamlessController first changed them. This is the explicit off switch; "
               L"Steamless then yields whenever Steam can see the physical controller.";
    case SteamStrategy::ControllerBlacklist:
        return L"Persists across Steam restarts. Steam stays open and other controller "
               L"types keep Steam Input, but Steam Controller layouts, gyro, and touch "
               L"menus in Steam are unavailable. Choose 'Restore Steam's original "
               L"controller settings' to undo it; uninstall also restores the baseline.";
    case SteamStrategy::NoJoy:
        return L"Adds -nojoy to Steam's existing Windows startup command. Steam can stay "
               L"open, but all Steam Input and Big Picture controller navigation are "
               L"disabled. Launching Steam another way without the flag bypasses it. "
               L"Choose 'Restore Steam's original controller settings' to undo it; "
               L"uninstall also restores the baseline startup command.";
    }
    return L"";
}

const wchar_t* StatusLabel(SteamStrategy strategy) {
    switch (strategy) {
    case SteamStrategy::YieldToSteam: return L"Original Steam settings restored (Steamless off)";
    case SteamStrategy::ControllerBlacklist: return L"Steam Controller reserved for Steamless";
    case SteamStrategy::NoJoy: return L"Steam controller support disabled (-nojoy)";
    }
    return L"Unknown";
}

const wchar_t* AppliedMessage(SteamStrategy strategy) {
    switch (strategy) {
    case SteamStrategy::YieldToSteam:
        return L"Steam's original controller settings are restored. Steamless will yield "
               L"whenever Steam can see the physical controller.";
    case SteamStrategy::ControllerBlacklist:
        return L"The Steam Controller is now reserved for Steamless. Choose 'Restore Steam's "
               L"original controller settings' at any time to undo this.";
    case SteamStrategy::NoJoy:
        return L"Steam now starts with -nojoy. Choose 'Restore Steam's original controller "
               L"settings' at any time to undo this.";
    }
    return L"";
}

} // namespace SteamStrategies
