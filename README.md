# Auto Hack 8in1

A Windows C++/Win32 helper for the following GTA Online hacking minigames:

- Slider
- Signal repeater / flashing grid
- Choose fingerprint
- Sort fingerprint
- Fleeca circuit breaker
- Find the number / BruteForce
- Signal matching
- Doomsday unlock

The app detects the active supported minigame, shows a small HUD/overlay, and uses screen analysis plus simulated key input to complete the detected sequence.
On activation it locates the visible GTA V client area (`GTA5_Enhanced.exe` or `GTA5.exe`), captures only that area, and downsizes it proportionally only when its height exceeds 1080 pixels.

## Requirements

- Windows
- MinGW-w64 with `g++.exe`, or MSVC Build Tools / Visual Studio with `cl.exe`
- PowerShell

For MinGW-w64, make sure `g++.exe` is available on `PATH`, or use the common `C:\mingw64\bin\g++.exe` fallback shown below.
For MSVC, run the command from a "Developer PowerShell for VS" prompt, or call `VsDevCmd.bat` first.

## Build

### MinGW-w64

```powershell
$root = (Get-Location).Path
$gxx = "g++"
$windres = "windres"
if (-not (Get-Command $gxx -ErrorAction SilentlyContinue)) {
  $gxx = "C:\mingw64\bin\g++.exe"
  $windres = "C:\mingw64\bin\windres.exe"
}

$resourceObject = Join-Path $env:TEMP "auto_hack_8in1_app_$PID.o"
& $windres "$root\src\resources\app.rc" -I "$root\src\resources" `
  --codepage=65001 -O coff -o $resourceObject

& $gxx -std=c++17 -O2 -DUNICODE -D_UNICODE -DNOMINMAX -D_WIN32_WINNT=0x0A00 `
  -static -static-libgcc -static-libstdc++ -municode -mwindows `
  "$root\src\main.cpp" `
  "$root\src\app\app_ui.cpp" `
  "$root\src\app\localization.cpp" `
  "$root\src\app\app_runtime.cpp" `
  "$root\src\capture\game_window.cpp" `
  "$root\src\input\key_input.cpp" `
  "$root\src\input\held_key_input.cpp" `
  "$root\src\input\mouse_input.cpp" `
  "$root\src\games\slider_module.cpp" `
  "$root\src\games\flashing_module.cpp" `
  "$root\src\games\choose_fingerprint_module.cpp" `
  "$root\src\games\sort_fingerprint_module.cpp" `
  "$root\src\games\fleeca_module.cpp" `
  "$root\src\games\find_number_module.cpp" `
  "$root\src\games\match_module.cpp" `
  "$root\src\games\doomsday_unlock_module\doomsday_unlock_module.cpp" `
  "$root\src\games\doomsday_unlock_module\imgproc\imgproc.cpp" `
  "$root\src\games\doomsday_unlock_module\vision\vision.cpp" `
  "$root\src\games\doomsday_unlock_module\tracking\tracking.cpp" `
  "$root\src\games\doomsday_unlock_module\json\json.cpp" `
  "$root\src\games\doomsday_unlock_module\preset\level_matcher.cpp" `
  "$root\src\games\doomsday_unlock_module\preset\motion.cpp" `
  "$root\src\games\doomsday_unlock_module\preset\preset_host.cpp" `
  $resourceObject `
  -lgdi32 -luser32 -lshell32 -lgdiplus -lcomctl32 -ldwmapi `
  -o "$root\auto_hack_8in1.exe"

Remove-Item -LiteralPath $resourceObject -Force
```

### MSVC

```powershell
$root = (Get-Location).Path
$resourceFile = Join-Path $env:TEMP "auto_hack_8in1_app_$PID.res"

rc /nologo /c 65001 /I "$root\src\resources" `
  /fo "$resourceFile" "$root\src\resources\app.rc"

cl /nologo /std:c++17 /EHsc /O2 /MT /DUNICODE /D_UNICODE /DNOMINMAX `
  /D_WIN32_WINNT=0x0A00 `
  "$root\src\main.cpp" `
  "$root\src\app\app_ui.cpp" `
  "$root\src\app\localization.cpp" `
  "$root\src\app\app_runtime.cpp" `
  "$root\src\capture\game_window.cpp" `
  "$root\src\input\key_input.cpp" `
  "$root\src\input\held_key_input.cpp" `
  "$root\src\input\mouse_input.cpp" `
  "$root\src\games\slider_module.cpp" `
  "$root\src\games\flashing_module.cpp" `
  "$root\src\games\choose_fingerprint_module.cpp" `
  "$root\src\games\sort_fingerprint_module.cpp" `
  "$root\src\games\fleeca_module.cpp" `
  "$root\src\games\find_number_module.cpp" `
  "$root\src\games\match_module.cpp" `
  "$root\src\games\doomsday_unlock_module\doomsday_unlock_module.cpp" `
  "$root\src\games\doomsday_unlock_module\imgproc\imgproc.cpp" `
  "$root\src\games\doomsday_unlock_module\vision\vision.cpp" `
  "$root\src\games\doomsday_unlock_module\tracking\tracking.cpp" `
  "$root\src\games\doomsday_unlock_module\json\json.cpp" `
  "$root\src\games\doomsday_unlock_module\preset\level_matcher.cpp" `
  "$root\src\games\doomsday_unlock_module\preset\motion.cpp" `
  "$root\src\games\doomsday_unlock_module\preset\preset_host.cpp" `
  "$resourceFile" `
  /link /SUBSYSTEM:WINDOWS `
  /OUT:"$root\auto_hack_8in1.exe" `
  user32.lib gdi32.lib shell32.lib gdiplus.lib comctl32.lib dwmapi.lib

Remove-Item -LiteralPath $resourceFile -Force
```

The compiled executable is written to:

```text
auto_hack_8in1.exe
```

## Configuration

Runtime settings are stored next to the executable in `setting.ini`. The program creates and updates this file automatically, so you do not need to prepare it before running the app.

`setting.ini` is intentionally ignored by Git because it is local machine/user state. `setting.example.ini` documents the default values:

```ini
setup_complete=0
launch_mode=0
delay_preset=0
language=0
hotkey_vk=117
overlay_cursor=1
tap_hold_ms=20
tap_gap_ms=20
```

`setup_complete=0` opens the first-launch setup dialog. It changes to `1` after setup is accepted.

`hotkey_vk=117` is `F6`.

`launch_mode=0` is Resident mode, which keeps the compact status panel on top and enables optional in-game overlays. `launch_mode=1` is Silent mode: it creates no topmost or game-overlay windows, and shows a small non-topmost status notice for two seconds when the hotkey turns automation on or off.

`capture_backend=0` keeps the default GDI capture path. `capture_backend=1` enables the experimental DXGI Desktop Duplication path. DXGI capture failures are reported directly and never fall back to GDI, so test results remain unambiguous.

`language=0` selects Simplified Chinese, the default. `language=1` selects English. Translation text is maintained as native Windows `STRINGTABLE` resources in `src/resources/lang/zh-CN.rc` and `src/resources/lang/en-US.rc`; both files are compiled into the executable.

Delay presets are `0` for Fast (`20/20 ms`), `1` for Slow (`40/40 ms`), and `2` for Custom. Timing varies by system: use Slow at low or unstable frame rates and Fast at a stable 60 FPS or higher. If inputs are missed or incorrect, adjust both values with Custom.

## Usage

1. Build and run `auto_hack_8in1.exe`.
2. Start the target game and open a supported hacking minigame.
3. Press the configured hotkey to start or stop detection/automation.
4. Use the taskbar button or HUD to bring the overlay back if needed.

## Repository Notes

Generated binaries, local build scripts, logs, and local settings are excluded from version control. Commit the source files, README, example config, `.gitignore`, and `.gitattributes`.

