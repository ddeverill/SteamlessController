# SteamlessController

A lightweight Windows system tray app that lets you use a **Steam Controller** as a standard gamepad — without Steam running.

<img width="263" height="154" alt="image" src="https://github.com/user-attachments/assets/7f4a63f6-b700-47dd-aeac-1fa8f78fcd04" />

When **Steamless Mode** is active, the app disables the controller's built-in keyboard/mouse emulation (lizard mode) and exposes it as a virtual Xbox 360 controller via [ViGEmBus](https://github.com/nefarius/ViGEmBus), making it compatible with any game that supports XInput or the Xbox controller.

## Features

- **NEW in 1.9** - Bluetooth! Use the controller wired, through the dongle, or paired over Bluetooth LE
- **NEW in 1.8** - Automatically enable/disable based on if Steam is running, or a Steam game is running!
- **NEW in 1.8** - Advanced debugging and local logging to help diagnose issues
- Rumble Support v2.0
- Support for multiple Steam Controllers! Long live split screen!
- New shiny UI for remapping L4/L5 R4/L5 back buttons!
- Support for Xbox OR PlayStation controllers - with PS Touchpad and Gyro plumbing.
- System tray icon shows connection and mode status
- **Steamless Mode** — disables lizard mode and exposes controller as Xbox 360 gamepad
- **Trackpad Mouse** — use the right (or left) trackpad as a mouse cursor
- **Back Buttons for Clicking** — map R4/R5 (or L4/L5) to left/right mouse click
- **Use Left Trackpad Instead** — mirror all trackpad/back-button functionality to the left side for left-handed users
- **Start with Windows** — launch automatically at login
- Settings persist across restarts

<img width="614" height="430" alt="image" src="https://github.com/user-attachments/assets/a8077a20-fd6b-4e1a-a3ea-ccd288a176c6" />

<img width="757" height="663" alt="image" src="https://github.com/user-attachments/assets/9a53b632-82b6-4485-93b0-1238c2ec6a07" />

## Requirements

### To run
- Windows 10 or later (64-bit)
- [ViGEmBus](https://github.com/nefarius/ViGEmBus/releases/latest) driver installed
- Steam Controller — wired USB (PID `0x1302`), wireless dongle (PID `0x1304`), or Bluetooth LE (PID `0x1303`, pair via Windows Bluetooth settings)
- Steam **closed**, or a coexistence strategy selected from the tray's debug menu

### To build
- [Visual Studio 2022](https://visualstudio.microsoft.com/) with the **Desktop development with C++** workload
- [CMake](https://cmake.org/download/) 3.20 or later (included with Visual Studio, or install separately)
- Windows SDK 10.0.22000 or later (installed via Visual Studio Installer)

## Building

```bat
git clone https://github.com/your-username/SteamlessController.git
cd SteamlessController
cmake -B build
cmake --build build --target SteamlessController
```

The executable will be at `build\Debug\SteamlessController.exe`.

For a release build:

```bat
cmake -B build/release -G "Visual Studio 18 2025"
cmake --build build/release --config Release --target SteamlessController
```

> If you have Visual Studio 2022, replace `"Visual Studio 18 2025"` with `"Visual Studio 17 2022"`.

## CMake Targets

| Target | Description |
|---|---|
| `SteamlessController` | Main system tray application |
| `SteamProbe` | Console diagnostic tool — dumps raw HID report bytes as you interact with the controller. Useful for protocol research. |
| `RawControllerProbe` | Checks whether `Windows.Gaming.Input.RawGameController` can enumerate the Steam Controller (requires WinRT). |

## How it works

The Steam Controller exposes a vendor HID collection (usage page `0xFF00`) that carries all game input in a 54-byte report (ID `0x42`) at ~60 Hz. By default the firmware runs in **lizard mode**, emulating a keyboard and mouse so the controller works without drivers.

### Keeping Steam running

Steam normally opens the physical Steam Controller even while the client is idle. That can produce duplicate keyboard/mouse and virtual-gamepad input if SteamlessController uses the controller at the same time.

The tray's **Debug: Steam coexistence** submenu can apply one of three strategies. Hover an option to see its tradeoffs; selecting one safely releases the controller, closes Steam, applies the setting, and relaunches Steam immediately. If a Steam game is running, the app asks before closing it.

- **Yield to Steam** makes no Steam-specific controller changes. SteamlessController stays off whenever Steam can see the physical controller. This preserves full Steam Input but Game Pass use requires closing Steam.
- **Hide Steam Controllers from Steam** is recommended for Game Pass. It adds only the three Steam Controller product IDs to Steam's per-device blacklist, leaving Steam Input available for other controller types. Steam Controller-specific layouts, gyro, and touch menus are unavailable in Steam.
- **Launch Steam with `-nojoy`** disables all Steam client controller support, including Steam Input and Big Picture controller navigation. The app adds the flag to Steam's existing Windows startup entry; launching Steam another way without the flag is detected and SteamlessController safely yields.

The blacklist option backs up `config/config.vdf` to `config/config.vdf.steamlesscontroller.bak`, preserves unrelated blacklist entries, and manages this property inside the outer `InstallConfigStore` object:

```vdf
"controller_blacklist" "28de/1302,28de/1303,28de/1304"
```

SteamlessController verifies both the setting and Steam's current `logs/controller.txt` session before treating the blacklist as safe. For `-nojoy`, it verifies the running `steam.exe` command line through Windows Management Instrumentation. Missing or stale evidence always makes SteamlessController yield rather than risk duplicate input.

SteamlessController sends HID feature reports to disable lizard mode, then reads the raw input reports and translates them into a virtual Xbox 360 controller via ViGEmBus. A background heartbeat re-sends the disable command every 800 ms to keep lizard mode off.

The full input report layout is documented in [`src/steam/SteamController.h`](src/steam/SteamController.h).

## Third-party

- [ViGEmClient](https://github.com/nefarius/ViGEmClient) — MIT License, built from source as a static library
