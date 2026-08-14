[English](README.md) | [中文](README_CN.md)

# AutoClicker

A lightweight Windows auto-clicker with a **sidebar navigation + Glassmorphism UI**, built with C++20 and GDI. Dark & light themes included.

- **Glassmorphism window**: the whole window is a layered translucent surface (UpdateLayeredWindow + 32bpp premultiplied-alpha 3-layer cache); **borderless** — custom-drawn title bar (minimize/maximize/close controls + sound + pin + theme + profile chips), rounded corners let the desktop show through, drag / edge-resize / double-click-to-maximize / snap all work; cards/sidebar/status bar are frosted panels with hairline borders, top sheen gradients and soft shadows — "deep-space glass" in dark mode, "mist-white glass" in light mode
- **Landscape window**: 640×480 classic 4:3 aspect ratio, two-column card layout; position/size remembered across launches
- **High-performance clicking**: 1ms system timer + sub-millisecond precise sleep (fine spin), click interval error < 0.1ms, exact CPS; ~0% idle CPU
- **Buttery motion**: page transitions slide in + fade (content layer pre-rendered, animation frames cost only 2 alpha-blends + present, steady 60fps); slide-in toasts; 4 accent colors applied instantly
- **Modern fonts**: auto-picks the nicest CJK font installed (Noto Sans SC > HarmonyOS Sans SC > MiSans > Source Han Sans > Microsoft YaHei), grayscale anti-aliased for translucent backgrounds
- **IME-hook resistant**: window title set via ANSI APIs; clicks sent through dynamically-resolved PostMessage to bypass IME IAT hooks

## Features

- **Sidebar navigation**: Click / Multi / Scroll / Dashboard / Advanced pages, all features configurable independently; pages slide in with a 160ms transition animation
- **Config profiles**: 4 profile chips in the title bar switch whole setting sets (CPS/hotkeys/gates/theme/rhythm…) in one click; rename them on the Dashboard page; bindable cycle hotkey; old config file migrates automatically
- **Left/Right click automation** with adjustable CPS and live delay readout
- **CPS presets**: one-click 6 / 10 / 15 / 20 clicks per second
- **Multi-click mode** with configurable multiplier (1-5x) and delay (1-200ms), plus 2x/3x/4x/5x and 10/25/50/100ms presets
- **Scroll-to-click**: converts wheel scrolls into left/right clicks
- **Random CPS** jitter to mimic human behavior
- **Humanized rhythm**: 4 click-rhythm modes (uniform / double-click bursts / breathing wave / fatigue decay) with adjustable strength (1-5); stacks with random CPS
- **Realtime CPS dashboard**: 12-second CPS curve + live rate + total clicks + session time; the status bar shows a mini curve and rate
- **Game status dashboard**: connection / in-game / targeted entity / hit type, read live from the DLL's shared-memory section (no gate needed once the DLL is injected)
- **Accent color**: blue / purple / green / orange, applies instantly
- **Attack-only clicking** (accessory of the direct clicker): auto-injects MCCombatStatusJni.dll into Minecraft Java processes and reads the live “can attack the targeted entity” state (0/1) primarily via the DLL's shared-memory section `Local\MCCombatStatus_<PID>` (5ms updates; UDP port 35785 remains a fallback for old DLL builds); when enabled, the clicker only clicks while an attack is possible, otherwise it behaves as before. Supported versions: **1.8.8 – 1.21.11, almost every Minecraft version (vanilla / Forge / Fabric / NeoForge)**
- **Place-only right-clicking** (accessory of the direct clicker, same DLL): the status channel also carries the 2nd state — “is the held item a placeable” (0/1, refreshed together with the attack state at 5ms); when enabled, only the RIGHT button clicks while the player holds a block placeable (ItemBlock/BlockItem). Independent of the attack-only gate; both can be on at once. Enabling this gate also triggers the auto-injection (shared injection channel)
- **CPS limit** to prevent clicking too fast (type a value directly)
- **Auto-stop timer**: stops the clicker after N seconds
- **HWID usage reporting**: at startup reports a stable per-machine hardware ID (MachineGuid-derived, `HW-xxxx`) to a hardcoded server — `http://<domain>:<port>/report?hwid=...` (domain + port constants in `servercfg.h`, no config file); fire-and-forget background request with a 5s timeout, an unreachable server never blocks the UI; results logged to `%APPDATA%\AutoClicker\report.log`
- **Startup update check**: at startup compares the local version against the server's latest (`GET /version/latest`); if a newer version exists a MessageBox pops up showing the latest version number + changelog (`GET /content/latest`); already-up-to-date or unreachable server → silent (details in `%APPDATA%\AutoClicker\update.log`)
- **Custom hotkeys**: press any key to bind (Esc clears to “none”), with guide toast
- **Keep-click mode**: auto-click without holding the hotkey
- **Always-on-top** pin button; window position/size are remembered across launches
- **Sound switch**: the speaker button in the title bar mutes/restores every notification sound (toggles, clicking, gates) in one click; the switch itself always plays a confirmation sound; saved per profile
- **Glassmorphism UI**: translucent frosted panels, hairline borders, soft shadows, "deep-space glass" dark & "mist-white glass" light themes, 4 accent colors, slide-in toasts, hand cursor feedback
- **Responsive window**: resizable, cards scale to fit small screens
- **Keyboard navigation**: arrow keys switch pages
- **Minimal footprint** - no external dependencies beyond Windows SDK

## Requirements

- Windows 10 or later
- Visual Studio 2022+ (v143+ toolset) with C++ Desktop Development workload

## Build

1. Open `AutoClicker.sln` in Visual Studio
2. Pick a configuration:
   - **`Release | x64`** — Network edition: HWID usage reporting + startup
     version check (`AUTOCLICKER_NET` defined)
   - **`Release-Base | x64`** — Base edition: no networking modules compiled
     in (no reporting, no update check)
3. Build → Build Solution (Ctrl+Shift+B)

Or build from command line:

```powershell
msbuild AutoClicker.sln /p:Configuration=Release /p:Platform=x64          # Net
msbuild AutoClicker.sln /p:Configuration=Release-Base /p:Platform=x64      # Base
```

Building requires `MCCombatStatusJni.dll` in the repository root (a PreBuildEvent copies it into the project dir and it is embedded into the exe as an RCDATA resource).

## Usage

- **Sidebar**: click the icons to switch pages (Click / Multi / Scroll / Dashboard / Advanced), or use arrow keys
- **Title-bar profile chips**: one click switches the whole settings profile (current profile is saved first); hover for the name, rename on the Dashboard page, bind a cycle hotkey on the Advanced page
- **Click page**: left/right toggles + CPS sliders + presets + clicker hotkey + keep mode + attack-only gate / place-only right-click gate (each with a live status chip and hotkey)
- **Multi page**: multiplier/delay sliders + presets + multi hotkey
- **Scroll page**: scroll-click toggle + left/right selector + two hotkeys
- **Dashboard page**: live CPS curve, total clicks, session time, game status (connection / in-game / targeted entity / hit type), profile renaming, accent color
- **Advanced page**: CPS limit, random CPS, auto-stop timer, humanized rhythm (uniform / double-click / breathing / fatigue + strength)
- **Hotkeys**: click a hotkey button, then press any key to bind (Esc clears to “none”)
- **Attack-only gate**: toggle on the Click page (row 3); when on, only the LEFT button pauses unless the targeted entity is attackable (right button is unaffected). Status bar has a 3rd indicator (green=attackable, red=not, dim=no game data; the 4th one shows the held-item state) and the Click page shows a live 可攻击/不可攻击/未连接 chip; bindable hotkey. Fail-safe: no status data (game closed / DLL missing) is treated as “cannot attack”
- **Place-only right-click gate**: toggle on the Click page (row 4); when on, only the RIGHT button pauses unless the held item is a placeable. Status bar has a 4th indicator (green=placeable, red=not, dim=no game data) and the Click page shows a live 手持放置物/非放置物/未连接 chip; bindable hotkey. Same status channel as the attack gate (shared memory first, UDP 2-byte datagram fallback) — the two gates are independent and can both be enabled
- **Auto-injection**: after either the attack-only gate or the place-only gate is enabled, a background scan finds javaw/java processes and injects MCCombatStatusJni.dll into every Minecraft Java client not yet injected (identified by GLFW30/LWJGL window class, x64 only, no double injection, re-injects after game restarts, exponential retry backoff 1s→30s on failure); **nothing is injected while both gates are off**. Since DLL V65 the injected module no longer creates a worker thread and never calls AttachCurrentThread (it hooks gdi32!SwapBuffers and reuses the render thread's existing JNIEnv via GetEnv) - verified on the NetEase China Edition 1.20.1 Forge client (game stays alive, status reads correctly; NetEase windows use the same GLFW30 class so auto-injection finds them; keep observing over longer sessions at your own discretion)
- **Single-file distribution**: MCCombatStatusJni.dll is embedded into the exe and auto-extracted to `%TEMP%\AutoClicker\` at startup; a sidecar DLL next to the exe takes priority when present (for DLL updates)
- **Stats**: the status bar shows live CPS (with a mini curve) and the Dashboard page shows the total click count of the session

## License

MIT
