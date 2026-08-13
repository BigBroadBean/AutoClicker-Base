#include "overlay.h"
#include "glass.h"
#include <Windows.h>
#include <string>
#include <thread>
#include <chrono>
#include <atomic>

constexpr int TOAST_W = 240;
constexpr int TOAST_H = 64;
constexpr int TOAST_MARGIN = 8;      // room for the soft shadow
constexpr int TOAST_STAY_MS = 1100;

static std::atomic<int> g_toastGen{ 0 };

// 玻璃态 Toast 体: 阴影 + 玻璃底 + 顶部高光 + 发丝描边 + 强调色条
static void DrawToast(GLayer& l, const wchar_t* title, const wchar_t* status, COLORREF statusColor)
{
    RECT body = { TOAST_MARGIN, TOAST_MARGIN, TOAST_W + TOAST_MARGIN, TOAST_H + TOAST_MARGIN };
    GLShadow(l, body, 12, 6, RGB(0, 0, 0), 66);
    GLFillRound(l, body, 12, PANEL(), (BYTE)(g_theme == Theme::Dark ? 208 : 218));
    GLFillV(l, body, 12, SHEEN(), (BYTE)(g_theme == Theme::Dark ? 10 : 28), SHEEN(), 0);
    GLRing(l, body, 12, 1, HAIRLINE(), (BYTE)(g_theme == Theme::Dark ? 42 : 62));
    // 左侧强调色条
    RECT bar = { body.left + 8, body.top + 12, body.left + 12, body.bottom - 12 };
    GLFillRound(l, bar, 2, statusColor, 255);

    HDC dc = l.dc;
    SetBkMode(dc, TRANSPARENT);
    HFONT hTitle = CreateFontW(20, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                               0, 0, ANTIALIASED_QUALITY, 0, g_uiFontName);
    if (!hTitle) hTitle = CreateFontW(20, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                                      0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
    SelectObject(dc, hTitle);
    SetTextColor(dc, TXT());
    RECT rt = { body.left + 22, body.top + 6, body.right - 10, body.top + 30 };
    DrawTextW(dc, title, -1, &rt, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    GLLiftAlphaRect(l, rt);
    DeleteObject(hTitle);

    HFONT hStatus = CreateFontW(22, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                                0, 0, ANTIALIASED_QUALITY, 0, g_uiFontName);
    if (!hStatus) hStatus = CreateFontW(22, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                                        0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
    SelectObject(dc, hStatus);
    SetTextColor(dc, statusColor);
    RECT rs = { body.left + 22, body.top + 28, body.right - 10, body.bottom - 6 };
    DrawTextW(dc, status, -1, &rs, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    GLLiftAlphaRect(l, rs);
    DeleteObject(hStatus);
}

// 线程入口按值接收 wstring: std::thread 构造时同步深拷贝,
// 调用方的临时字符串可以先安全销毁 (避免悬垂指针)
static void ToastThreadProc(std::wstring title, std::wstring status, COLORREF statusColor,
                            int gen)
{
    const int W = TOAST_W + TOAST_MARGIN * 2;
    const int H = TOAST_H + TOAST_MARGIN * 2;
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int x = screenW - W - 24;
    int y = screenH - H - 80;

    HINSTANCE hInst = GetModuleHandle(nullptr);

    HWND hToast = CreateWindowExA(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT,
        "Static", "", WS_POPUP,
        x, y, W, H,
        nullptr, nullptr, hInst, nullptr);
    if (!hToast) return;

    GLayer surf = GLCreate(W, H);
    if (!surf.dc) {
        DestroyWindow(hToast);
        return;
    }

    auto Render = [&](BYTE alpha, int yy) {
        GLClear(surf);
        DrawToast(surf, title.c_str(), status.c_str(), statusColor);
        HDC sd = GetDC(hToast);
        if (!sd) return;
        POINT pt = { x, yy };
        SIZE sz = { W, H };
        POINT sp = { 0, 0 };
        BLENDFUNCTION blend = { AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA };
        UpdateLayeredWindow(hToast, sd, &pt, &sz, surf.dc, &sp, 0, &blend, ULW_ALPHA);
        ReleaseDC(hToast, sd);
    };

    ShowWindow(hToast, SW_SHOW);
    // 淡入 + 从下方 20px 滑入
    for (int step = 0; step <= 5; ++step) {
        if (g_toastGen != gen) break;
        int slide = (int)(20.0f * (1.0f - step / 5.0f) + 0.5f);
        Render((BYTE)(51 * step), y + slide);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    if (g_toastGen == gen)
        std::this_thread::sleep_for(std::chrono::milliseconds(TOAST_STAY_MS));
    // 淡出
    for (int step = 20; step >= 0; --step) {
        if (g_toastGen != gen) break;
        Render((BYTE)(255 * step / 20), y);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    GLFree(surf);
    DestroyWindow(hToast);
}

void ShowToast(const wchar_t* title, const wchar_t* status, COLORREF statusColor)
{
    int gen = ++g_toastGen;
    std::thread(ToastThreadProc, std::wstring(title), std::wstring(status),
                statusColor, gen).detach();
}
