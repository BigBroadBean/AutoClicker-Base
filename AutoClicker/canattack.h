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
// This module owns two background threads:
//   1. UDP monitor   : binds 127.0.0.1:35785, reads the live 2-byte status
//                      into g_canAttack / g_canPlace, async loop sleeping 5ms.
//   2. Injector      : periodically finds Minecraft Java processes that do NOT
//                      have the DLL loaded yet and injects them (LoadLibrary
//                      remote thread), with anti-double-injection handling.

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

// true while UDP packets are arriving (game injected & running)
bool CanAttackConnected();

// true when MCCombatStatusJni.dll can be located next to the exe / in CWD
bool CanAttackDllAvailable();

// start both background threads (called once from WinMain)
void StartCanAttackMonitor();
void StartInjectorThread();
