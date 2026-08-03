#include "SteamWatcher.h"
#include "EventLog.h"
#include "SteamStrategy.h"
#include <Windows.h>
#include <TlHelp32.h>
#include <Wbemidl.h>
#include <shellapi.h>
#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

static constexpr int POLL_INTERVAL_MS      = 2000;
static constexpr int POLL_SLICE_MS         = 100;  // wake often for fast Stop()
static constexpr int LESS_STEAM_POLL_COUNT = 3;    // ~6s stable before we take over

struct SteamProcess {
    bool     running = false;
    DWORD    pid = 0;
    FILETIME started{};
};

static SteamProcess FindSteamProcess() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return {};

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"steam.exe") == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    if (pid == 0) return {};

    SteamProcess result{};
    result.running = true;
    result.pid = pid;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return result;

    FILETIME exited{}, kernel{}, user{};
    GetProcessTimes(process, &result.started, &exited, &kernel, &user);
    CloseHandle(process);
    return result;
}

static bool CommandLineHasNoJoy(const std::wstring& commandLine) {
    int count = 0;
    LPWSTR* argv = CommandLineToArgvW(commandLine.c_str(), &count);
    if (!argv) return false;
    bool found = false;
    for (int i = 1; i < count; ++i) {
        if (_wcsicmp(argv[i], L"-nojoy") == 0) {
            found = true;
            break;
        }
    }
    LocalFree(argv);
    return found;
}

// Win32_Process.CommandLine is a documented Windows source for another
// process's command line. It lets the watcher prove the running Steam session
// really has -nojoy instead of trusting a stale startup setting.
static bool SteamProcessHasNoJoy(DWORD pid) {
    IWbemLocator* locator = nullptr;
    IWbemServices* services = nullptr;
    IEnumWbemClassObject* results = nullptr;
    IWbemClassObject* object = nullptr;
    bool hasNoJoy = false;

    HRESULT hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_IWbemLocator,
                                  reinterpret_cast<void**>(&locator));
    if (FAILED(hr)) goto cleanup;

    {
        BSTR namespaceName = SysAllocString(L"ROOT\\CIMV2");
        hr = namespaceName
            ? locator->ConnectServer(namespaceName, nullptr, nullptr, nullptr, 0,
                                     nullptr, nullptr, &services)
            : E_OUTOFMEMORY;
        SysFreeString(namespaceName);
    }
    if (FAILED(hr)) goto cleanup;

    hr = CoSetProxyBlanket(services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                           RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                           nullptr, EOAC_NONE);
    if (FAILED(hr)) goto cleanup;

    {
        const std::wstring queryText = L"SELECT CommandLine FROM Win32_Process WHERE ProcessId="
                                     + std::to_wstring(pid);
        BSTR language = SysAllocString(L"WQL");
        BSTR query = SysAllocString(queryText.c_str());
        hr = language && query
            ? services->ExecQuery(language, query,
                                  WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                                  nullptr, &results)
            : E_OUTOFMEMORY;
        SysFreeString(language);
        SysFreeString(query);
    }
    if (FAILED(hr) || !results) goto cleanup;

    {
        ULONG returned = 0;
        hr = results->Next(2000, 1, &object, &returned);
        if (FAILED(hr) || returned != 1 || !object) goto cleanup;

        VARIANT value{};
        VariantInit(&value);
        hr = object->Get(L"CommandLine", 0, &value, nullptr, nullptr);
        if (SUCCEEDED(hr) && V_VT(&value) == VT_BSTR && V_BSTR(&value))
            hasNoJoy = CommandLineHasNoJoy(V_BSTR(&value));
        VariantClear(&value);
    }

cleanup:
    if (object) object->Release();
    if (results) results->Release();
    if (services) services->Release();
    if (locator) locator->Release();
    return hasNoJoy;
}

static bool ReadRegistryString(HKEY root, const wchar_t* subkey, const wchar_t* name,
                               std::wstring& value) {
    DWORD bytes = 0;
    if (RegGetValueW(root, subkey, name, RRF_RT_REG_SZ, nullptr, nullptr, &bytes)
            != ERROR_SUCCESS || bytes < sizeof(wchar_t))
        return false;

    value.resize(bytes / sizeof(wchar_t));
    if (RegGetValueW(root, subkey, name, RRF_RT_REG_SZ, nullptr, value.data(), &bytes)
            != ERROR_SUCCESS)
        return false;
    while (!value.empty() && value.back() == L'\0') value.pop_back();
    return !value.empty();
}

static bool ReadFileTail(const std::wstring& path, size_t maxBytes, std::string& text,
                         FILETIME* lastWrite = nullptr) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0) {
        CloseHandle(file);
        return false;
    }
    if (lastWrite && !GetFileTime(file, nullptr, nullptr, lastWrite)) {
        CloseHandle(file);
        return false;
    }

    const ULONGLONG fileBytes = static_cast<ULONGLONG>(size.QuadPart);
    const DWORD bytes = static_cast<DWORD>(std::min<ULONGLONG>(fileBytes, maxBytes));
    LARGE_INTEGER offset{};
    offset.QuadPart = size.QuadPart - bytes;
    if (!SetFilePointerEx(file, offset, nullptr, FILE_BEGIN)) {
        CloseHandle(file);
        return false;
    }

    text.resize(bytes);
    DWORD read = 0;
    const bool ok = bytes == 0 || ReadFile(file, text.data(), bytes, &read, nullptr);
    CloseHandle(file);
    if (!ok) return false;
    text.resize(read);
    return true;
}

static void LowerAscii(std::string& text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
}

static bool ReportControllerHidden(bool hidden, const char* reason) {
    static std::string previous;
    const std::string current = std::string(hidden ? "hidden: " : "not hidden: ") + reason;
    if (current != previous) {
        EventLog::Write("STEAM: physical controller %s", current.c_str());
        previous = current;
    }
    return hidden;
}

static std::string ControllerBlacklistValue(const std::string& config) {
    constexpr char key[] = "\"controller_blacklist\"";
    const size_t keyPos = config.rfind(key);
    if (keyPos == std::string::npos) return {};
    const size_t open = config.find('"', keyPos + sizeof(key) - 1);
    if (open == std::string::npos) return {};
    const size_t close = config.find('"', open + 1);
    if (close == std::string::npos) return {};
    return config.substr(open + 1, close - open - 1);
}

// Steam writes an explicit line for every device suppressed by
// controller_blacklist. Requiring that line from the current client session is
// important: editing config.vdf while Steam is already running does not revoke
// the handles Steam has open. Any missing or ambiguous evidence fails closed.
static bool IsControllerHiddenFromSteam(const SteamProcess& steam) {
    if (!steam.running || (steam.started.dwLowDateTime == 0 && steam.started.dwHighDateTime == 0))
        return ReportControllerHidden(false, "process start time unavailable");

    std::wstring steamPath;
    if (!ReadRegistryString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath",
                            steamPath))
        return ReportControllerHidden(false, "SteamPath unavailable");
    std::replace(steamPath.begin(), steamPath.end(), L'/', L'\\');

    std::string config;
    const std::wstring configPath = steamPath + L"\\config\\config.vdf";
    if (!ReadFileTail(configPath, 1024 * 1024, config))
        return ReportControllerHidden(false, "config.vdf unavailable");
    LowerAscii(config);
    const std::string blacklist = ControllerBlacklistValue(config);
    if (blacklist.empty())
        return ReportControllerHidden(false, "controller_blacklist missing");

    FILETIME logWrite{};
    std::string log;
    if (!ReadFileTail(steamPath + L"\\logs\\controller.txt", 4 * 1024 * 1024, log,
                      &logWrite)
            || CompareFileTime(&logWrite, &steam.started) < 0)
        return ReportControllerHidden(false, "current controller log unavailable");
    LowerAscii(log);

    const size_t sessionPos = log.rfind("client version:");
    if (sessionPos == std::string::npos)
        return ReportControllerHidden(false, "current controller session unavailable");
    const std::string session = log.substr(sessionPos);

    constexpr const char* ids[] = {"28de/1302", "28de/1303", "28de/1304"};
    bool hidden = false;
    for (const char* id : ids) {
        std::string type(id);
        std::replace(type.begin(), type.end(), '/', ' ');
        if (session.find("type: " + type) != std::string::npos)
            return ReportControllerHidden(false, "Steam opened a physical controller");
        if (blacklist.find(id) != std::string::npos
                && session.find(std::string("hiding blacklisted device ") + id)
                        != std::string::npos)
            hidden = true;
    }
    return ReportControllerHidden(hidden, hidden ? "confirmed by current Steam session"
                                                  : "no matching hide event");
}

// Steam publishes the app id of the running game here and resets it to 0 on
// exit. Covers Steam games and non-Steam shortcuts launched through Steam;
// games launched entirely outside Steam are invisible to it (acceptable — we
// keep the controller, and the manual toggle remains the escape hatch).
static bool IsGameRunning() {
    DWORD appId = 0;
    DWORD size  = sizeof(appId);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"RunningAppID",
                     RRF_RT_REG_DWORD, nullptr, &appId, &size) != ERROR_SUCCESS)
        return false;
    return appId != 0;
}

SteamState SteamWatcher::Detect() {
    const SteamProcess steam = FindSteamProcess();
    if (!steam.running) return SteamState::NoSteam;
    if (SteamStrategies::IsNoJoySelected()) {
        if (SteamProcessHasNoJoy(steam.pid)) {
            ReportControllerHidden(true, "confirmed Steam -nojoy command line");
            return SteamState::ControllerHidden;
        }
        ReportControllerHidden(false, "selected -nojoy is absent from running Steam");
        return IsGameRunning() ? SteamState::InGame : SteamState::SteamIdle;
    }
    if (IsControllerHiddenFromSteam(steam)) return SteamState::ControllerHidden;
    return IsGameRunning() ? SteamState::InGame : SteamState::SteamIdle;
}

void SteamWatcher::Start(SteamStateFn onChange) {
    Stop();
    m_onChange = std::move(onChange);
    m_running  = true;
    m_thread   = std::thread(&SteamWatcher::PollLoop, this);
}

void SteamWatcher::Stop() {
    if (m_running.exchange(false) && m_thread.joinable())
        m_thread.join();
}

void SteamWatcher::PollLoop() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SteamState reported = Detect();
    m_state = reported;
    if (m_onChange) m_onChange(reported);

    SteamState pending      = reported;
    int        pendingPolls = 0;

    while (m_running.load()) {
        for (int waited = 0; waited < POLL_INTERVAL_MS && m_running.load();
             waited += POLL_SLICE_MS)
            Sleep(POLL_SLICE_MS);
        if (!m_running.load()) break;

        const SteamState now = Detect();
        if (now == reported) {
            pendingPolls = 0;
            continue;
        }

        if (static_cast<int>(now) > static_cast<int>(reported)) {
            // More Steam activity (Steam appeared / game launched) — report
            // immediately so the controller is yielded before Steam needs it.
            reported     = now;
            pendingPolls = 0;
            m_state      = reported;
            if (m_onChange) m_onChange(reported);
        } else {
            // Less Steam activity (game quit / Steam gone) — must hold for
            // several consecutive polls so a Steam self-restart or game
            // relaunch doesn't cause ownership flapping.
            if (now != pending) {
                pending      = now;
                pendingPolls = 1;
            } else if (++pendingPolls >= LESS_STEAM_POLL_COUNT) {
                reported     = now;
                pendingPolls = 0;
                m_state      = reported;
                if (m_onChange) m_onChange(reported);
            }
        }
    }
    if (SUCCEEDED(com)) CoUninitialize();
}
