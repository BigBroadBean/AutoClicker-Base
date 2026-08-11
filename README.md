[English](README.md) | [中文](README_CN.md)

# AutoClicker

A lightweight Windows auto-clicker with a **sidebar navigation + Neumorphism UI**, built with C++20 and GDI. Dark & light themes included.

- **Landscape window**: 640×480 classic 4:3 aspect ratio, two-column card layout
- **Light theme by default**: soft light neumorphism, dark theme one click away
- **High-performance clicking**: 1ms system timer + sub-millisecond precise sleep (fine spin), click interval error < 0.1ms, exact CPS; ~0% idle CPU
- **Modern fonts**: auto-picks the nicest CJK font installed (Noto Sans SC > HarmonyOS Sans SC > MiSans > Source Han Sans > Microsoft YaHei), Latin glyphs benefit too
- **Soft visuals**: buttons get a layered rounded glow on hover; all surfaces cast soft shadows (exact region geometry, corners perfectly aligned)
- **IME-hook resistant**: window title set via ANSI APIs; clicks sent through dynamically-resolved PostMessage to bypass IME IAT hooks

## Features

- **Sidebar navigation**: Click / Multi / Scroll / Advanced pages, all features configurable independently
- **Left/Right click automation** with adjustable CPS and live delay readout
- **CPS presets**: one-click 6 / 10 / 15 / 20 clicks per second
- **Multi-click mode** with configurable multiplier (1-5x) and delay (1-200ms), plus 2x/3x/4x/5x and 10/25/50/100ms presets
- **Scroll-to-click**: converts wheel scrolls into left/right clicks
- **Random CPS** jitter to mimic human behavior
- **Attack-only clicking** (accessory of the direct clicker): auto-injects MCCanAttackJni.dll into Minecraft Java processes and reads the live “can attack the targeted entity” state (0/1) via UDP port 35785 (5ms updates); when enabled, the clicker only clicks while an attack is possible, otherwise it behaves as before. Supported versions: **1.8.9 / 1.12.2 / 1.20.1 (incl. their Forge versions)**
- **CPS limit** to prevent clicking too fast (type a value directly)
- **Auto-stop timer**: stops the clicker after N seconds
- **HWID usage reporting**: at startup reports a stable per-machine hardware ID (MachineGuid-derived, `HW-xxxx`) to a hardcoded server — `http://<domain>:<port>/report?hwid=...` (domain + port constants in `servercfg.h`, no config file); fire-and-forget background request with a 5s timeout, an unreachable server never blocks the UI; results logged to `%APPDATA%\AutoClicker\report.log`
- **Startup update check**: at startup compares the local version against the server's latest (`GET /version/latest`); if a newer version exists a MessageBox pops up showing the latest version number + changelog (`GET /content/latest`); already-up-to-date or unreachable server → silent (details in `%APPDATA%\AutoClicker\update.log`)
- **Realtime CPS readout**: bottom-right chip shows the live click rate (1s sliding window)
- **Custom hotkeys**: press any key to bind (Esc clears to “none”), with guide toast
- **Keep-click mode**: auto-click without holding the hotkey
- **Always-on-top** pin button
- **Neumorphism UI**: soft extruded/inset surfaces, dark & light themes, theme-aware toasts
- **Responsive window**: resizable, cards scale to fit small screens
- **Keyboard navigation**: arrow keys switch pages
- **Minimal footprint** - no external dependencies beyond Windows SDK

## Requirements

- Windows 10 or later
- Visual Studio 2022+ (v143+ toolset) with C++ Desktop Development workload

## Build

1. Open `AutoClicker.sln` in Visual Studio
2. Select `Release | x64` configuration
3. Build → Build Solution (Ctrl+Shift+B)

Or build from command line:

```powershell
msbuild AutoClicker.sln /p:Configuration=Release /p:Platform=x64
```

Building requires `MCCanAttackJni.dll` in the repository root (a PreBuildEvent copies it into the project dir and it is embedded into the exe as an RCDATA resource).

## Usage

- **Sidebar**: click the icons to switch pages (Click / Multi / Scroll / Advanced), or use arrow keys
- **Click page**: left/right toggles + CPS sliders + presets + clicker hotkey + keep mode
- **Multi page**: multiplier/delay sliders + presets + multi hotkey
- **Scroll page**: scroll-click toggle + left/right selector + two hotkeys
- **Advanced page**: CPS limit, random CPS, auto-stop timer
- **Hotkeys**: click a hotkey button, then press any key to bind (Esc clears to “none”)
- **Attack-only gate**: toggle on the Click page (row 3); when on, only the LEFT button pauses unless the targeted entity is attackable (right button is unaffected). Status bar has a 4th indicator (green=attackable, red=not, dim=no game data) and the Click page shows a live 可攻击/不可攻击/未连接 chip; bindable hotkey. Fail-safe: no UDP data (game closed / DLL missing) is treated as “cannot attack”
- **Auto-injection**: after the attack-only gate is enabled, a background loop (1s period) scans javaw/java processes and injects MCCanAttackJni.dll into every Minecraft Java client not yet injected (identified by GLFW30/LWJGL window class, x64 only, no double injection, re-injects after game restarts); **nothing is injected while the feature is off**. Note: NetEase China Edition (game box) has anti-cheat protection - injection is detected and the game gets terminated, so the feature does not work there
- **Single-file distribution**: MCCanAttackJni.dll is embedded into the exe and auto-extracted to `%TEMP%\AutoClicker\` at startup; a sidecar DLL next to the exe takes priority when present (for DLL updates)
- **Counter**: shows total clicks of the session; click it to reset

## License

MIT
