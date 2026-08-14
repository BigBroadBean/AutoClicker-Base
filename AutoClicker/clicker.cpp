#include "clicker.h"
#include "config.h"
#include "sound.h"
#include "overlay.h"
#include "canattack.h"
#include "ui.h"

#include <Windows.h>
#include <thread>
#include <chrono>
#include <string>
#include <cstring>
#include <atomic>
#include <cmath>

#pragma comment(lib, "winmm.lib")

using namespace std::chrono;

Theme g_theme = Theme::Light;
int cpsLeft10 = 100;
int cpsRight10 = 100;
int cpsMax = 50;
int leftms = 50;
int rightms = 50;
int vk_key = 4;
int vk_multi_key = VK_XBUTTON2;
int vk_scroll_key = 6;
int vk_scroll_lr_key = VK_XBUTTON1;
bool changedKey = false;
HWND mhwnd = nullptr;
bool isstart = false;
bool leftenabled = false;
bool rightenabled = false;
bool keepClicke = false;
bool flag = false;
POINT point = {};

bool isMultiActive = false;
int multiMul = 1;
int multiDelayMs = 20;
bool randomCpsEnabled = false;
int randomCpsRange = 2;
std::atomic<long long> g_debounceUntil{ 0 };
std::atomic<bool> cursorOnlyClick{ false };

// 系统光标当前是否可见 (MC 背包/聊天/菜单中可见, 游戏视角下隐藏)
static bool IsCursorShowing()
{
    CURSORINFO ci = {};
    ci.cbSize = sizeof(ci);
    return GetCursorInfo(&ci) != FALSE && (ci.flags & CURSOR_SHOWING) != 0;
}
bool isScrollClickActive = false;
int scrollClickButton = 0;

int humanizeMode = 0;      // 0=均匀 1=双击连招 2=呼吸波动 3=疲劳递减
int humanizeLevel = 3;     // 1..5
int vk_profile_key = 0;    // 方案切换热键 (默认无)
int g_accentIdx = 0;       // 强调色 (types.h)

bool autoStopEnabled = false;
int autoStopSeconds = 30;
bool topmost = false;
bool soundEnabled = true;   // 提示音总开关 (默认开, 兼容旧配置)
std::atomic<long long> g_clickCount{ 0 };

// ---- realtime CPS: ring buffer of click timestamps (ns) ----
static constexpr int kCpsWindow = 1024;   // enough for 500+ CPS over 1s
static std::atomic<long long> s_clickStamp[kCpsWindow];
static std::atomic<int> s_clickHead{ 0 };
static std::atomic<int> s_clickFilled{ 0 };

void RecordClick()
{
    long long ns = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
    int h = s_clickHead.load(std::memory_order_relaxed);
    s_clickStamp[h].store(ns, std::memory_order_relaxed);
    s_clickHead.store((h + 1) % kCpsWindow, std::memory_order_relaxed);
    int f = s_clickFilled.load(std::memory_order_relaxed);
    if (f < kCpsWindow)
        s_clickFilled.store(f + 1, std::memory_order_relaxed);
    g_clickCount++;
}

int GetRealtimeCps()
{
    long long nowNs = duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
    int f = s_clickFilled.load(std::memory_order_relaxed);
    if (f <= 0) return 0;
    int h = s_clickHead.load(std::memory_order_relaxed);
    int n = f < kCpsWindow ? f : kCpsWindow;
    int cnt = 0;
    for (int i = 0; i < n; i++) {
        int idx = h - 1 - i;
        if (idx < 0) idx += kCpsWindow;
        if (nowNs - s_clickStamp[idx].load(std::memory_order_relaxed) <= 1000000000LL) cnt++;
        else break;   // timestamps are monotonic, older ones are even older
    }
    return cnt;
}

std::wstring getKeyName(int vk)
{
    switch (vk) {
    case 0:             return L"\x65e0";
    case VK_LBUTTON:   return L"\x9f20\x6807\x5de6\x952e";
    case VK_RBUTTON:   return L"\x9f20\x6807\x53f3\x952e";
    case VK_MBUTTON:   return L"\x9f20\x6807\x4e2d\x952e";
    case VK_XBUTTON1:  return L"\x9f20\x6807\x4fa7\x952e\x0031";
    case VK_XBUTTON2:  return L"\x9f20\x6807\x4fa7\x952e\x0032";
    }

    wchar_t buffer[256] = { 0 };
    UINT scanCode = MapVirtualKey(vk, MAPVK_VK_TO_VSC);

    switch (vk) {
    case VK_LEFT: case VK_UP: case VK_RIGHT: case VK_DOWN:
    case VK_RCONTROL: case VK_RMENU:
    case VK_LWIN: case VK_RWIN: case VK_APPS:
    case VK_INSERT: case VK_HOME: case VK_PRIOR:
    case VK_DELETE: case VK_END: case VK_NEXT:
    case VK_NUMLOCK: case VK_SCROLL:
    case VK_OEM_NEC_EQUAL:
        scanCode |= 0xE000;
        break;
    }

    GetKeyNameTextW(scanCode << 16, buffer, 255);
    return std::wstring(buffer);
}

void udmWindow()
{
    mhwnd = GetForegroundWindow();
}

static HHOOK g_hMouseHook = nullptr;

// shared dynamically-resolved PostMessageA for the mouse-hook click paths
// (multi-click & scroll-to-click). Dynamic GetProcAddress resolution keeps
// the call out of the IAT, immune to IME inline/IAT hooks (see DEVELOPMENT.md
// §7.1). Resolved once when the hook thread starts; plain pointer is safe:
// every hook consumer is created after that write.
using PfnPostMessageA = int (WINAPI*)(HWND, UINT, WPARAM, LPARAM);
static PfnPostMessageA g_PostMsgA = nullptr;

// synthetic clicks need a realistic press duration: a 0-length DOWN+UP pair
// (posted back-to-back) is dropped by some games / anti-cheats. The main
// clicker naturally holds ~25ms per click (DOWN at t, UP at t+interval);
// the hook paths emulate that with this fixed hold.
static constexpr int kClickHoldMs = 10;

static LRESULT CALLBACK MultiClickHookProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode < 0)
        return CallNextHookEx(nullptr, nCode, wParam, lParam);

    auto* p = (MSLLHOOKSTRUCT*)lParam;

    if (p->flags & LLMHF_INJECTED)
        return CallNextHookEx(nullptr, nCode, wParam, lParam);

    bool hasMulti = isMultiActive && multiMul > 1;
    bool hasScroll = isScrollClickActive;

    if (!hasMulti && !hasScroll)
        return CallNextHookEx(nullptr, nCode, wParam, lParam);

    // scroll-to-click: convert scroll wheel to click
    if (hasScroll && wParam == WM_MOUSEWHEEL) {
        HWND target = GetForegroundWindow();
        if (target) {
            wchar_t cls[64];
            if (!(GetClassNameW(target, cls, 64) && wcscmp(cls, L"ACgdi") == 0)) {
                POINT pt = p->pt;
                ScreenToClient(target, &pt);
                LPARAM lp = MAKELPARAM(pt.x, pt.y);
                bool isLeft = (scrollClickButton == 0);
                UINT msgDown = isLeft ? WM_LBUTTONDOWN : WM_RBUTTONDOWN;
                UINT msgUp = isLeft ? WM_LBUTTONUP : WM_RBUTTONUP;
                WPARAM wpDown = isLeft ? MK_LBUTTON : MK_RBUTTON;
                std::thread([target, msgDown, msgUp, wpDown, lp]() {
                    if (!g_PostMsgA) return;
                    g_PostMsgA(target, msgDown, wpDown, lp);
                    std::this_thread::sleep_for(std::chrono::milliseconds(kClickHoldMs));
                    g_PostMsgA(target, msgUp, 0, lp);
                }).detach();
            }
        }
        return 1;
    }

    if (!hasMulti)
        return CallNextHookEx(nullptr, nCode, wParam, lParam);

    bool isLeftDown = (wParam == WM_LBUTTONDOWN);
    bool isRightDown = (wParam == WM_RBUTTONDOWN);

    if (!isLeftDown && !isRightDown)
        return CallNextHookEx(nullptr, nCode, wParam, lParam);

    HWND target = GetForegroundWindow();
    if (!target)
        return CallNextHookEx(nullptr, nCode, wParam, lParam);

    wchar_t cls[64];
    if (GetClassNameW(target, cls, 64) && wcscmp(cls, L"ACgdi") == 0)
        return CallNextHookEx(nullptr, nCode, wParam, lParam);

    POINT pt = p->pt;
    ScreenToClient(target, &pt);

    int count = multiMul - 1;
    int delay = multiDelayMs;
    UINT msgDown = isLeftDown ? WM_LBUTTONDOWN : WM_RBUTTONDOWN;
    UINT msgUp = isLeftDown ? WM_LBUTTONUP : WM_RBUTTONUP;
    WPARAM wpDown = isLeftDown ? MK_LBUTTON : MK_RBUTTON;
    LPARAM lp = MAKELPARAM(pt.x, pt.y);

    std::thread([target, count, delay, msgDown, msgUp, wpDown, lp]() {
        if (!g_PostMsgA) return;
        for (int i = 0; i < count; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            g_PostMsgA(target, msgDown, wpDown, lp);
            std::this_thread::sleep_for(std::chrono::milliseconds(kClickHoldMs));
            g_PostMsgA(target, msgUp, 0, lp);
        }
    }).detach();

    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

void StartMultiClickHook()
{
    std::thread([]() {
        // resolve once, before the hook can deliver any event
        HMODULE u32 = GetModuleHandleA("User32.dll");
        g_PostMsgA = u32 ? (PfnPostMessageA)GetProcAddress(u32, "PostMessageA") : nullptr;

        g_hMouseHook = SetWindowsHookExW(WH_MOUSE_LL, MultiClickHookProc,
                                         GetModuleHandleW(nullptr), 0);
        if (!g_hMouseHook) return;

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        UnhookWindowsHookEx(g_hMouseHook);
        g_hMouseHook = nullptr;
    }).detach();
}

// our own window class ("ACgdi", registered ANSI in main.cpp). The mouse
// hook already skips it; the main clicker must too, otherwise clicking the
// UI while the clicker is active feeds synthetic clicks back into our own
// controls (the window is foreground while the user configures it).
static bool IsOwnWindow(HWND h)
{
    char cls[64] = {};
    return h && GetClassNameA(h, cls, 64) && strcmp(cls, "ACgdi") == 0;
}

// high-precision wait: coarse Sleep + fine spin for sub-millisecond accuracy.
// when precise, the final <=1.5ms stretch is a low-power spin so click timing
// error stays well below 0.1ms (steady_clock resolution ~100ns).
static void PreciseSleepUntil(steady_clock::time_point target, bool precise)
{
    for (;;) {
        auto remain = target - steady_clock::now();
        if (remain <= steady_clock::duration::zero()) return;
        long long us = duration_cast<microseconds>(remain).count();
        if (us > 8000) {
            Sleep((DWORD)((us - 2000) / 1000));     // coarse, wake ~2ms early
        } else if (us > 1500) {
            Sleep(1);                                 // 1ms timer granularity
        } else if (precise) {
            while (steady_clock::now() < target) YieldProcessor();  // final spin
            return;
        } else {
            Sleep(1);                                 // idle: true sleep, never spin
        }
    }
}

void ClickerThreadProc()
{
    typedef int(WINAPI* pPostMessageA) (HWND, UINT, WPARAM, LPARAM);
    pPostMessageA MyPostMessageA = (pPostMessageA)GetProcAddress(LoadLibraryA("User32.dll"), "PostMessageA");

    // xorshift64* PRNG (fast & uniform, for random CPS jitter)
    unsigned long long rng = 0x9E3779B97F4A7C15ull ^ (unsigned long long)GetTickCount64();
    auto rnd = [&]() -> unsigned {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        return (unsigned)(rng >> 32);
    };

    // 拟人化节奏: 在基础 CPS 上叠加变速因子, 再叠加随机抖动。
    // humanFactor = 1 时与旧版行为完全一致 (均匀 + 随机波动)。
    // factor 语义: <1 加速, >1 减速 (delay = 500 / (cps10 / factor))
    auto humanDelay = [&](int baseMs, int baseCps10, double humanFactor) -> int {
        double cps = (double)baseCps10 / humanFactor;
        int jitter = 0;
        if (randomCpsEnabled)
            jitter = (int)(rnd() % (unsigned)(randomCpsRange * 20 + 1)) - randomCpsRange * 10;
        int cps10 = (int)cps + jitter;
        if (cps10 < CPS_MIN10) cps10 = CPS_MIN10;
        if (cps10 > cpsMax * 10) cps10 = cpsMax * 10;
        return cpsToMs(cps10);
    };

    static std::atomic<bool> busyMulti{ false };
    static std::atomic<bool> busyKey{ false };
    static std::atomic<bool> busyPM{ false };
    static std::atomic<bool> busyScroll{ false };

    bool prevMulti = false;
    bool prevStart = false;

    // 拟人化节奏会话状态: 每次按住鼠标 (或 keep 模式开启) 视为一个会话,
    // 双击/呼吸/疲劳模式按会话内时间与击次计算变速因子
    bool prevHeldL = false, prevHeldR = false;
    long long clickIdxL = 0, clickIdxR = 0;
    auto heldStartL = steady_clock::now();
    auto heldStartR = steady_clock::now();

    // non-blocking click state
    enum ClickState { CS_IDLE, CS_WAIT_UP, CS_WAIT_DOWN };
    ClickState leftSt  = CS_IDLE;
    ClickState rightSt = CS_IDLE;
    auto nextLeftTime  = steady_clock::now();
    auto nextRightTime = steady_clock::now();
    auto nextScan      = steady_clock::now();
    POINT lastPt = {};

    // release helpers: whenever the state machine drops out of CS_WAIT_UP
    // (stop / toggle / mode switch / gate flip), the target window MUST
    // receive the matching button-up, otherwise the game sees a stuck
    // button (worst in keep mode: no physical release ever comes).
    auto releaseLeft = [&]() {
        if (leftSt == CS_WAIT_UP && mhwnd && !IsOwnWindow(mhwnd)) {
            GetCursorPos(&lastPt);
            ScreenToClient(mhwnd, &lastPt);
            MyPostMessageA(mhwnd, WM_LBUTTONUP, 0, MAKELPARAM(lastPt.x, lastPt.y));
        }
        leftSt = CS_IDLE;
    };
    auto releaseRight = [&]() {
        if (rightSt == CS_WAIT_UP && mhwnd && !IsOwnWindow(mhwnd)) {
            GetCursorPos(&lastPt);
            ScreenToClient(mhwnd, &lastPt);
            MyPostMessageA(mhwnd, WM_RBUTTONUP, 0, MAKELPARAM(lastPt.x, lastPt.y));
        }
        rightSt = CS_IDLE;
    };

    for (;;) {
        auto now = steady_clock::now();

        if (!flag) {
            flag = true;
            Sleep(1);
            continue;
        }

        // ---- periodic scan: hotkeys + auto-stop (every ~4ms) ----
        if (now >= nextScan) {
            nextScan = now + milliseconds(4);

            if (GetTickCount64() < (unsigned long long)g_debounceUntil) {
                // still debouncing after a key rebind
            } else {
                // multi-click hotkey - edge detect + async wait-release
                bool curMulti = vk_multi_key && (GetAsyncKeyState(vk_multi_key) & 0x8000) != 0;
                if (curMulti && !prevMulti && !busyMulti.exchange(true)) {
                    std::thread([]() {
                        while (GetAsyncKeyState(vk_multi_key) & 0x8000) Sleep(1);
                        isMultiActive = !isMultiActive;
                        PlayMultiClickSound(isMultiActive);
                        ShowToggleToast(L"\x591a\x500d\x70b9", isMultiActive);
                        SaveConfig();
                        busyMulti = false;
                    }).detach();
                }
                prevMulti = curMulti;

                // auto-clicker hotkey - edge detect + async wait-release
                bool curStart = vk_key && (GetAsyncKeyState(vk_key) & 0x8000) != 0;
                if (curStart && !prevStart && !busyKey.exchange(true)) {
                    std::thread([]() {
                        while (GetAsyncKeyState(vk_key) & 0x8000) Sleep(1);
                        isstart = !isstart;
                        GetAsyncKeyState(VK_LBUTTON);
                        GetAsyncKeyState(VK_RBUTTON);
                        PlayClickerSound(isstart);
                        ShowToggleToast(L"\x8fde\x70b9\x5668", isstart);
                        SaveConfig();
                        busyKey = false;
                    }).detach();
                }
                prevStart = curStart;

                // can-attack gate hotkey - edge detect + async wait-release
                {
                    static std::atomic<bool> busyCanAtk{ false };
                    static bool prevCanAtk = false;
                    bool curCanAtk = vk_canattack_key && (GetAsyncKeyState(vk_canattack_key) & 0x8000) != 0;
                    if (curCanAtk && !prevCanAtk && !busyCanAtk.exchange(true)) {
                        std::thread([]() {
                            while (GetAsyncKeyState(vk_canattack_key) & 0x8000) Sleep(1);
                            canAttackOnlyClick = !canAttackOnlyClick;
                            NotifyGateToggled();   // wake shm poller / UDP monitor / injector
                            PlayCanAttackSound(canAttackOnlyClick);
                            ShowCanAttackToast(canAttackOnlyClick);
                            SaveConfig();
                            busyCanAtk = false;
                        }).detach();
                    }
                    prevCanAtk = curCanAtk;
                }

                // can-place gate hotkey (right-click only while holding a
                // placeable) - edge detect + async wait-release
                {
                    static std::atomic<bool> busyPlace{ false };
                    static bool prevPlace = false;
                    bool curPlace = vk_place_key && (GetAsyncKeyState(vk_place_key) & 0x8000) != 0;
                    if (curPlace && !prevPlace && !busyPlace.exchange(true)) {
                        std::thread([]() {
                            while (GetAsyncKeyState(vk_place_key) & 0x8000) Sleep(1);
                            placeOnlyRightClick = !placeOnlyRightClick;
                            NotifyGateToggled();   // wake shm poller / UDP monitor / injector
                            PlayCanPlaceSound(placeOnlyRightClick);
                            ShowCanPlaceToast(placeOnlyRightClick);
                            SaveConfig();
                            busyPlace = false;
                        }).detach();
                    }
                    prevPlace = curPlace;
                }

                // scroll-to-click hotkey - edge detect + async wait-release
                bool curScroll = vk_scroll_key && (GetAsyncKeyState(vk_scroll_key) & 0x8000) != 0;
                {
                    static bool prevScroll = false;
                    if (curScroll && !prevScroll && !busyScroll.exchange(true)) {
                        std::thread([]() {
                            while (GetAsyncKeyState(vk_scroll_key) & 0x8000) Sleep(1);
                            isScrollClickActive = !isScrollClickActive;
                            PlayScrollClickSound(isScrollClickActive);
                            ShowToggleToast(L"\x6eda\x8f6e\x70b9\x51fb", isScrollClickActive);
                            SaveConfig();
                            busyScroll = false;
                        }).detach();
                    }
                    prevScroll = curScroll;
                }

                // scroll L/R toggle hotkey - edge detect + async wait-release
                {
                    static std::atomic<bool> busyScrollLR{ false };
                    static bool prevScrollLR = false;
                    bool curScrollLR = vk_scroll_lr_key && (GetAsyncKeyState(vk_scroll_lr_key) & 0x8000) != 0;
                    if (curScrollLR && !prevScrollLR && !busyScrollLR.exchange(true)) {
                        std::thread([]() {
                            while (GetAsyncKeyState(vk_scroll_lr_key) & 0x8000) Sleep(1);
                            scrollClickButton = (scrollClickButton == 0) ? 1 : 0;
                            PlayScrollLRSound();
                            ShowScrollLRToast(scrollClickButton);
                            SaveConfig();
                            busyScrollLR = false;
                        }).detach();
                    }
                    prevScrollLR = curScrollLR;
                }

                // profile cycle hotkey - edge detect + async wait-release.
                // 切换只在连点线程做数据面 (SwitchProfile), 主题/置顶/Toast 等
                // UI 副作用通过 PostMessage 交给主窗口线程 (WM_APP_PROFILE)。
                {
                    static std::atomic<bool> busyProf{ false };
                    static bool prevProf = false;
                    bool curProf = vk_profile_key && (GetAsyncKeyState(vk_profile_key) & 0x8000) != 0;
                    if (curProf && !prevProf && !busyProf.exchange(true)) {
                        std::thread([]() {
                            while (GetAsyncKeyState(vk_profile_key) & 0x8000) Sleep(1);
                            int next = (g_activeProfile % PROFILE_COUNT) + 1;
                            if (SwitchProfile(next)) {
                                NotifyGateToggled();   // 方案可能携带门控开关变化
                                PlayScrollLRSound();
                                if (g_uiHwnd)
                                    PostMessageW(g_uiHwnd, WM_APP_PROFILE, (WPARAM)next, 0);
                            }
                            busyProf = false;
                        }).detach();
                    }
                    prevProf = curProf;
                }

                // +/- keys adjust multi-click multiplier
                {
                    bool plus = (GetAsyncKeyState(VK_OEM_PLUS) & 0x8000) != 0;
                    bool minus = (GetAsyncKeyState(VK_OEM_MINUS) & 0x8000) != 0;
                    bool numPlus = (GetAsyncKeyState(VK_ADD) & 0x8000) != 0;
                    bool numMinus = (GetAsyncKeyState(VK_SUBTRACT) & 0x8000) != 0;
                    if ((plus || minus || numPlus || numMinus) && !busyPM.exchange(true)) {
                        bool inc = (plus || numPlus);
                        std::thread([inc]() {
                            while ((GetAsyncKeyState(VK_OEM_PLUS) & 0x8000) ||
                                   (GetAsyncKeyState(VK_OEM_MINUS) & 0x8000) ||
                                   (GetAsyncKeyState(VK_ADD) & 0x8000) ||
                                   (GetAsyncKeyState(VK_SUBTRACT) & 0x8000)) Sleep(1);
                            if (inc) { if (multiMul < 5) multiMul++; }
                            else { if (multiMul > 1) multiMul--; }
                            SaveConfig();
                            busyPM = false;
                        }).detach();
                    }
                }

                // ---- auto-stop timer: stop the clicker after N seconds ----
                {
                    static steady_clock::time_point stopStart{};
                    static bool stopArmed = false;
                    if (isstart && autoStopEnabled && autoStopSeconds > 0) {
                        if (!stopArmed) {
                            stopStart = steady_clock::now();
                            stopArmed = true;
                        } else if (steady_clock::now() - stopStart >= milliseconds((long long)autoStopSeconds * 1000)) {
                            isstart = false;
                            PlayClickerSound(false);
                            ShowToggleToast(L"\x8fde\x70b9\x5668", false);
                            SaveConfig();
                            stopArmed = false;
                        }
                    } else if (!isstart) {
                        stopArmed = false;
                    }
                }
            }
        }

        // ---- click state machine (sub-millisecond timing) ----
        // multi-click mode takes over the physical mouse: release any
        // in-flight synthetic press so the game never gets a stuck button
        if (isMultiActive) { releaseLeft(); releaseRight(); }

        // can-attack gate: when enabled, only the LEFT button clicks while the
        // targeted creature is attackable (live 0/1 fed by the shm/UDP monitor).
        // can-place gate: when enabled, only the RIGHT button clicks while the
        // held item is a placeable (ItemBlock/BlockItem). Both are independent.
        bool canAtkGate = !canAttackOnlyClick ||
                          g_canAttack.load(std::memory_order_relaxed) == 1;
        bool canPlaceGate = !placeOnlyRightClick ||
                            g_canPlace.load(std::memory_order_relaxed) == 1;

        // cursor gate: 开启后仅当系统光标不可见 (游戏视角) 时连点,
        // 光标可见 (背包/聊天/菜单) 时暂停, 避免在 GUI 里误点。
        bool cursorGate = !cursorOnlyClick.load(std::memory_order_relaxed) ||
                          !IsCursorShowing();

        // gate flipped off mid-click: release the held mouse button immediately
        // so the game never gets stuck with a pressed mouse button
        if (!canAtkGate) releaseLeft();
        if (!canPlaceGate) releaseRight();
        if (!cursorGate) { releaseLeft(); releaseRight(); }

        // never target our own window (see IsOwnWindow above)
        bool targetOk = mhwnd && !IsOwnWindow(mhwnd);
        bool leftActive = isstart && leftenabled && targetOk && !isMultiActive && canAtkGate && cursorGate;
        bool rightActive = isstart && rightenabled && targetOk && !isMultiActive && canPlaceGate && cursorGate;

        bool leftHeld = leftActive && ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) || keepClicke);
        if (leftHeld) {
            if (now >= nextLeftTime) {
                GetCursorPos(&lastPt);
                ScreenToClient(mhwnd, &lastPt);
                LPARAM lp = MAKELPARAM(lastPt.x, lastPt.y);
                if (leftSt != CS_WAIT_UP) {
                    MyPostMessageA(mhwnd, WM_LBUTTONDOWN, MK_LBUTTON, lp);
                    leftSt = CS_WAIT_UP;
                    RecordClick();
                } else {
                    MyPostMessageA(mhwnd, WM_LBUTTONUP, 0, lp);
                    leftSt = CS_IDLE;
                }
                if (!prevHeldL) { heldStartL = now; clickIdxL = 0; }   // 新会话
                double t  = duration<double>(now - heldStartL).count();
                double amp = humanizeLevel / 5.0;
                double f = 1.0;
                switch (humanizeMode) {
                case 1:   // 双击连招: 短促双击 + 组间停顿
                    f = (clickIdxL % 2 == 0) ? 1.0 - 0.28 * amp : 1.0 + 0.38 * amp;
                    break;
                case 2:   // 呼吸波动: 正弦曲线起伏 (~3.5s 周期)
                    f = 1.0 + 0.24 * amp * std::sin(t * 6.28318530718 / 3.5);
                    break;
                case 3:   // 疲劳递减: 按住越久越慢, 渐近 -30%
                    f = 1.0 + 0.30 * amp * (1.0 - std::exp(-t / 8.0));
                    break;
                default:  // 均匀
                    f = 1.0;
                    break;
                }
                if (f < 0.5) f = 0.5;
                if (f > 1.5) f = 1.5;
                clickIdxL++;
                int del = humanDelay(leftms, cpsLeft10, f);
                nextLeftTime = now + milliseconds(del);
            }
            prevHeldL = true;
        } else {
            // clicker stopped / button disabled mid-press: send the pending UP
            releaseLeft();
            prevHeldL = false;
        }

        bool rightHeld = rightActive && ((GetAsyncKeyState(VK_RBUTTON) & 0x8000) || keepClicke);
        if (rightHeld) {
            if (now >= nextRightTime) {
                GetCursorPos(&lastPt);
                ScreenToClient(mhwnd, &lastPt);
                LPARAM lp = MAKELPARAM(lastPt.x, lastPt.y);
                if (rightSt != CS_WAIT_UP) {
                    MyPostMessageA(mhwnd, WM_RBUTTONDOWN, MK_RBUTTON, lp);
                    rightSt = CS_WAIT_UP;
                    RecordClick();
                } else {
                    MyPostMessageA(mhwnd, WM_RBUTTONUP, 0, lp);
                    rightSt = CS_IDLE;
                }
                if (!prevHeldR) { heldStartR = now; clickIdxR = 0; }   // 新会话
                double t  = duration<double>(now - heldStartR).count();
                double amp = humanizeLevel / 5.0;
                double f = 1.0;
                switch (humanizeMode) {
                case 1:
                    f = (clickIdxR % 2 == 0) ? 1.0 - 0.28 * amp : 1.0 + 0.38 * amp;
                    break;
                case 2:
                    f = 1.0 + 0.24 * amp * std::sin(t * 6.28318530718 / 3.5);
                    break;
                case 3:
                    f = 1.0 + 0.30 * amp * (1.0 - std::exp(-t / 8.0));
                    break;
                default:
                    f = 1.0;
                    break;
                }
                if (f < 0.5) f = 0.5;
                if (f > 1.5) f = 1.5;
                clickIdxR++;
                int del = humanDelay(rightms, cpsRight10, f);
                nextRightTime = now + milliseconds(del);
            }
            prevHeldR = true;
        } else {
            // clicker stopped / button disabled mid-press: send the pending UP
            releaseRight();
            prevHeldR = false;
        }

        // ---- precise wait until the next event ----
        auto next = nextScan;
        if (leftHeld  && nextLeftTime  < next) next = nextLeftTime;
        if (rightHeld && nextRightTime < next) next = nextRightTime;
        if (next > now) {
            PreciseSleepUntil(next, leftHeld || rightHeld);
        }
    }
}
