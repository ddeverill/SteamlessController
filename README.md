# SteamlessController

A lightweight Windows system tray app that lets you use a **Steam Controller** as a standard gamepad — without Steam running.

<img width="263" height="154" alt="image" src="https://github.com/user-attachments/assets/7f4a63f6-b700-47dd-aeac-1fa8f78fcd04" />

When **Steamless Mode** is active, the app disables the controller's built-in keyboard/mouse emulation (lizard mode) and exposes it as a virtual Xbox 360 controller (or PlayStation controller) via [ViGEmBus](https://github.com/nefarius/ViGEmBus), making it compatible with any game that supports the Xbox or PlayStation controller.

## Features

- Supports all connection modes! Puck, wired, and Bluetooth!
- Runs alongside Steam! No need to close Steam to toggle modes or force you to use your PC in a weird way.
- Brand new extensive remapping controls for back buttons and trackpads. Save custom mappings and load them per game!
- Enable/Disable manually, or use new auto modes to turn on / off when it makes sense.
- Start on launch, suppress notifications, make this utility your own.

<img width="614" height="430" alt="image" src="https://github.com/user-attachments/assets/a8077a20-fd6b-4e1a-a3ea-ccd288a176c6" />

<img width="757" height="663" alt="image" src="https://github.com/user-attachments/assets/9a53b632-82b6-4485-93b0-1238c2ec6a07" />

## Requirements

### To run
- Windows 10 or later (64-bit)
- [ViGEmBus](https://github.com/nefarius/ViGEmBus/releases/latest) driver installed
- Steam Controller — wired USB (PID `0x1302`), wireless dongle (PID `0x1304`), or Bluetooth LE (PID `0x1303`, pair via Windows Bluetooth settings)
- Steam **closed** (Steam claims the controller when running)

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

## Building the installer

The installer is compiled from `resources\InnoInstallerScript.iss` with [Inno Setup](https://jrsoftware.org/isinfo.php). It needs two things a fresh clone does not have.

**Release builds of all three executables it packages.** The command in the previous section builds only the tray app:

```bat
cmake --build build/release --config Release --target SteamlessController
cmake --build build/release --config Release --target SteamlessDeviceCycle
cmake --build build/release --config Release --target ViGEmBusProbe
```

`SteamlessDeviceCycle` is the elevated helper that restarts the controller's device node. `ViGEmBusProbe` is how Setup checks whether the driver actually installed, and it stays in the install folder afterwards as a diagnostic.

**The ViGEmBus driver installer**, which is not in this repository — it is a 6 MB vendored binary, and `*.exe` is gitignored. Download it from [ViGEmBus releases](https://github.com/nefarius/ViGEmBus/releases/latest) and place it here:

```
resources\ViGEmBus_1.22.0_x64_x86_arm64.exe
```

The filename must match `#define ViGEmSetup` at the top of the script. Inno resolves `[Files]` sources when it compiles, so a missing or differently named payload fails the build outright rather than producing an installer that silently ships no driver. If you update the ViGEmBus version, change the `#define` to match the new filename.

## CMake Targets

| Target | Description |
|---|---|
| `SteamlessController` | Main system tray application |
| `SteamlessDeviceCycle` | Elevated helper that disables and re-enables the controller's device node, run as an on-demand scheduled task. Shipped by the installer. |
| `ViGEmBusProbe` | Reports whether a ViGEm bus is present by enumerating its interface. Used by Setup to verify the driver installed, and shipped as a diagnostic. |
| `SteamProbe` | Console diagnostic tool — dumps raw HID report bytes as you interact with the controller. Useful for protocol research. |
| `RawControllerProbe` | Checks whether `Windows.Gaming.Input.RawGameController` can enumerate the Steam Controller (requires WinRT). |

## How it works

The Steam Controller exposes a vendor HID collection (usage page `0xFF00`) that carries all game input in a 54-byte report (ID `0x42`) at ~60 Hz. By default the firmware runs in **lizard mode**, emulating a keyboard and mouse so the controller works without drivers.

SteamlessController sends HID feature reports to disable lizard mode, then reads the raw input reports and translates them into a virtual Xbox 360 controller via ViGEmBus. A background heartbeat re-sends the disable command every 800 ms to keep lizard mode off.

The full input report layout is documented in [`src/steam/SteamController.h`](src/steam/SteamController.h).

## Third-party

- [ViGEmClient](https://github.com/nefarius/ViGEmClient) — MIT License, built from source as a static library
