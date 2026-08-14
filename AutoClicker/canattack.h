#pragma once

#include <Windows.h>
#include <atomic>

// ============================================================
//  can-attack / can-place gating (auxiliary of the direct clicker)
// ============================================================
// MCCombatStatusJni.dll is injected into Minecraft Java processes (javaw/java).
// Once attached to the game's JVM it sends one UDP datagram every ~5ms to
// 127.0.0.1:35785 carrying 2 bytes:
//   byte0 = '1' (0x31) = the targeted entity IS attackable right now,
//           '0' (0x30) = it is NOT
//   byte1 = '1' (0x31) = the held item is a placeable (ItemBlock/BlockItem),
//           '0' (0x30) = not a placeable / empty hand
// (byte0 matches the legacy 1-byte protocol, so old receivers still work.)
//
// This module owns three background threads:
//   1. Shared-memory poller (PRIMARY channel): reads the live status struct
//      that the DLL publishes in "Local\MCCombatStatus_<pid>" every ~5ms.
//      No port conflicts, no socket overhead. Runs for the whole app lifetime
//      (5ms cadence, negligible CPU) so the dashboard/HUD can show live game
//      state (目标实体/命中类型/游戏内) even while both gates are off.
//   2. UDP monitor (fallback for old DLLs): binds 127.0.0.1:35785 and reads
//      the same 2-byte datagrams. Lifecycle-gated: the port is only bound
//      while a gate is on, so other apps can use 35785 meanwhile.
//   3. Injector      : periodically finds Minecraft Java processes that do NOT
//      have the DLL loaded yet and injects them via MANUAL MAPPING (V67:
//      no LoadLibrary, no module-list entry, payload decrypted in memory and
//      never written to disk - the NetEase client-side anti-cheat scans
//      desktop files and background-process executables), with
//      anti-double-injection handling (shared-memory health check),
//      failure backoff and log throttling.
//      Parks on an event while both gates are off.

// ---- feature state ----
// atomic so the injector thread can reliably observe UI/hotkey toggles
extern std::atomic<bool> canAttackOnlyClick;    // gate left clicks on canAttack
extern std::atomic<bool> placeOnlyRightClick;   // gate right clicks on canPlace
extern int  vk_canattack_key;     // can-attack toggle hotkey VK code (0 = none)
extern int  vk_place_key;         // can-place  toggle hotkey VK code (0 = none)

// ---- live status from the UDP stream ----
// 1 = attackable / holding a placeable, 0 = not. Fail-safe: no fresh packet for
// a while (game closed / DLL not injected / port busy) -> 0.
extern std::atomic<int> g_canAttack;
extern std::atomic<int> g_canPlace;
// GetTickCount64() of the last received packet; 0 = never received any.
extern std::atomic<long long> g_canAttackLastMs;

// ---- live status from the shared-memory channel (HUD 用, 只读快照) ----
// 与门控开关无关: 只要游戏里有我们的 DLL 在发布, 就持续更新。
int  GetShmInGame();                       // 1=已进入游戏 0=否/未连接
int  GetShmHitType();                      // 0=未命中 1=方块 2=实体
void GetShmTargetName(char* out, size_t cap);   // 准星目标类名 (可空)

// true while fresh status is arriving (game injected & running)
bool CanAttackConnected();

// true when a payload is available (embedded encrypted resource or a sidecar
// MCCombatStatusJni.dll next to the exe / in CWD)
bool CanAttackDllAvailable();

// start the background threads (called once from WinMain)
void StartCanAttackMonitor();
void StartCanAttackShmPoller();
void StartInjectorThread();

// wake the gate-driven threads after canAttackOnlyClick / placeOnlyRightClick
// changes (call from ANY thread: UI toggles, hotkey toggles, config load)
void NotifyGateToggled();
