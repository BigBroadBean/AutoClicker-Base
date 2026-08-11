#include "clicker.h"
#include "config.h"
#include "sound.h"
#include "overlay.h"
#include "canattack.h"

#include <Windows.h>
#include <thread>
#include <chrono>
#include <string>
#include <atomic>

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
bool isScrollClickActive = false;
int scrollClickButton = 0;

bool autoStopEnabled = false;
int autoStopSeconds = 30;
bool topmost = false;
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
                    typedef int(WINAPI* pPostMsg)(HWND, UINT, WPARAM, LPARAM);
                    pPostMsg PostMsgA = (pPostMsg)GetProcAddress(LoadLibraryA("User32.dll"), "PostMessageA");
                    if (!PostMsgA) return;
                    PostMsgA(target, msgDown, wpDown, lp);
                    PostMsgA(target, msgUp, 0, lp);
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
        typedef int(WINAPI * pPostMsg)(HWND, UINT, WPARAM, LPARAM);
        pPostMsg PostMsgA = (pPostMsg)GetProcAddress(LoadLibraryA("User32.dll"), "PostMessageA");
        if (!PostMsgA) return;

        for (int i = 0; i < count; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            PostMsgA(target, msgDown, wpDown, lp);
            PostMsgA(target, msgUp, 0, lp);
        }
    }).detach();

    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

void StartMultiClickHook()
{
    std::thread([]() {
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

    auto randDelay = [&](int baseMs, int baseCps10) -> int {
        if (!randomCpsEnabled) return baseMs;
        int offset = (int)(rnd() % (unsigned)(randomCpsRange * 20 + 1)) - randomCpsRange * 10;
        int cps10 = baseCps10 + offset;
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

    // non-blocking click state
    enum ClickState { CS_IDLE, CS_WAIT_UP, CS_WAIT_DOWN };
    ClickState leftSt  = CS_IDLE;
    ClickState rightSt = CS_IDLE;
    auto nextLeftTime  = steady_clock::now();
    auto nextRightTime = steady_clock::now();
    auto nextScan      = steady_clock::now();
    POINT lastPt = {};

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
        if (isMultiActive) { leftSt = CS_IDLE; rightSt = CS_IDLE; }

        // can-attack gate: when enabled, only the LEFT button clicks while the
        // targeted creature is attackable (live 0/1 fed by the UDP monitor).
        // can-place gate: when enabled, only the RIGHT button clicks while the
        // held item is a placeable (ItemBlock/BlockItem). Both are independent.
        bool canAtkGate = !canAttackOnlyClick ||
                          g_canAttack.load(std::memory_order_relaxed) == 1;
        bool canPlaceGate = !placeOnlyRightClick ||
                            g_canPlace.load(std::memory_order_relaxed) == 1;

        // gate flipped off mid-click: release the held mouse button immediately
        // so the game never gets stuck with a pressed mouse button
        if (!canAtkGate && mhwnd && leftSt == CS_WAIT_UP) {
            GetCursorPos(&lastPt);
            ScreenToClient(mhwnd, &lastPt);
            MyPostMessageA(mhwnd, WM_LBUTTONUP, 0, MAKELPARAM(lastPt.x, lastPt.y));
            leftSt = CS_IDLE;
        }
        if (!canPlaceGate && mhwnd && rightSt == CS_WAIT_UP) {
            GetCursorPos(&lastPt);
            ScreenToClient(mhwnd, &lastPt);
            MyPostMessageA(mhwnd, WM_RBUTTONUP, 0, MAKELPARAM(lastPt.x, lastPt.y));
            rightSt = CS_IDLE;
        }

        bool leftActive = isstart && leftenabled && mhwnd != nullptr && !isMultiActive && canAtkGate;
        bool rightActive = isstart && rightenabled && mhwnd != nullptr && !isMultiActive && canPlaceGate;

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
                int del = randDelay(leftms, cpsLeft10);
                nextLeftTime = now + milliseconds(del);
            }
        } else {
            leftSt = CS_IDLE;
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
                int del = randDelay(rightms, cpsRight10);
                nextRightTime = now + milliseconds(del);
            }
        } else {
            rightSt = CS_IDLE;
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
