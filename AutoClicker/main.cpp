#include "types.h"
#include "ui.h"
#include "clicker.h"
#include "config.h"
#include "overlay.h"
#include "sound.h"
#include "canattack.h"
#include "report.h"

#include <Windows.h>
#include <dwmapi.h>
#include <mmsystem.h>
#include <thread>
#include <cmath>
#include <cstdio>
#include <string>
#include <atomic>

#pragma comment(lib, "Dwmapi.lib")
#pragma comment(lib, "winmm.lib")

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

// ---- GDI resources ----
static HDC     g_hdcMem = nullptr;
static HBITMAP g_hbmBuf = nullptr;
static HFONT   g_hfTitle = nullptr;
static HFONT   g_hfLabel = nullptr;
static HFONT   g_hfBody  = nullptr;
static HFONT   g_hfSmall = nullptr;
static int     g_cx = WIN_W, g_cy = WIN_H;
static float   g_lyScale = 1.0f;   // responsive layout scale
static bool    g_dirty = true;     // repaint-on-demand

// ---- pages (sidebar) ----
enum Page { PAGE_CLICK = 0, PAGE_MULTI, PAGE_SCROLL, PAGE_ADV, PAGE_COUNT };
static Page g_page = PAGE_CLICK;

// ---- slider ids ----
enum SliderId { SL_L = 0, SL_R, SL_MUL, SL_DEL, SL_MAX, SL_RAND, SL_COUNT };

// ---- interactive elements ----
enum Elem {
    E_NONE = -1,
    // sidebar navigation
    E_NAV_CLICK, E_NAV_MULTI, E_NAV_SCROLL, E_NAV_ADV,
    // click page
    E_TGL_L, E_TGL_R, E_SL_L, E_SL_R, E_BTN_KEY, E_BTN_KEEP,
    E_BTN_CANATK, E_CHIP_CANATK, E_BTN_CANATK_KEY,
    E_PRE_L0, E_PRE_L1, E_PRE_L2, E_PRE_L3,
    E_PRE_R0, E_PRE_R1, E_PRE_R2, E_PRE_R3,
    // multi page
    E_SL_MUL, E_SL_DEL, E_BTN_MKEY,
    E_PRE_M0, E_PRE_M1, E_PRE_M2, E_PRE_M3,
    E_PRE_D0, E_PRE_D1, E_PRE_D2, E_PRE_D3,
    // scroll page
    E_TGL_SCROLL, E_BTN_SCROLL_KEY, E_BTN_SCROLL_L, E_BTN_SCROLL_R, E_BTN_SCROLL_LR_KEY,
    // advanced page
    E_SL_MAX, E_INP_MAX, E_CHK_RAND, E_SL_RAND, E_CHK_AUTOSTOP, E_INP_AUTOSTOP,
    // title bar & status
    E_BTN_THEME, E_BTN_PIN,
    E_COUNT
};
struct HR { RECT r; Elem id; bool hover; };
static HR   g_hr[E_COUNT] = {};
static Elem g_drag = E_NONE;
static int  g_dx = 0;

// ---- text input state ----
enum InputTarget { IN_NONE = 0, IN_CPSMAX, IN_AUTOSTOP };
static bool        g_inputOn = false;
static int         g_inputTarget = IN_NONE;
static wchar_t     g_inputBuf[16] = {};

// ---- key capture state (rebinding feedback) ----
static Elem g_rebinding = E_NONE;

// ---- preset tables ----
static const int kCpsPresets[4]   = { 6, 10, 15, 20 };    // CPS
static const int kMulPresets[4]   = { 2, 3, 4, 5 };       // x倍
static const int kDelayPresets[4] = { 10, 25, 50, 100 };  // ms

// ---- layout rects ----
struct LY {
    RECT title, sidebar, nav[4], card[3], status;
    RECT btnPin, btnTheme;
    RECT track[SL_COUNT], thumb[SL_COUNT];
    RECT tglL, tglR, tglScroll;
    RECT btnKey, btnKeep, btnMKey;
    RECT btnCanAtk, canAtkChip, btnCanAtkKey;
    RECT preL[4], preR[4], preM[4], preD[4];
    RECT btnScrollKey, btnScrollLR, btnScrollLRKey;
    RECT inpMax, chkRand, chkAutoStop, inpAutoStop;
    RECT cntChip;
} L;

static bool PtIn(const RECT& r, int x, int y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

// responsive layout scale helper (usable anywhere)
static int S(int v) { return (int)(v * g_lyScale); }

// ============================================================
//  layout
// ============================================================
static void Layout()
{
    int W = g_cx, H = g_cy;

    // ---- title bar ----
    L.title   = { 0, 6, W, 46 };
    L.btnPin   = { W - 96, 12, W - 68, 40 };
    L.btnTheme = { W - 60, 12, W - 32, 40 };

    // ---- sidebar ----
    L.sidebar = { 12, 58, 80, H - 64 };
    int navY = L.sidebar.top + 14;
    int navGap = (L.sidebar.bottom - L.sidebar.top - 4 * 52 - 28) / 3;
    if (navGap < 8) navGap = 8;
    for (int i = 0; i < 4; i++) {
        L.nav[i] = { 14, navY, 78, navY + 52 };
        navY += 52 + navGap;
    }

    // ---- content cards: row1 (two side-by-side) + row2 (full width) ----
    int top = 58, gap = 14;
    int bh1 = 165, bh2 = 135;   // base row heights
    int nRows = 2;
    if (g_page == PAGE_SCROLL) { bh1 = 185; nRows = 1; }
    int avail = H - 52 - top - (nRows - 1) * gap;
    int sumBase = bh1 + (nRows == 2 ? bh2 : 0);
    float f = (float)avail / (float)sumBase;
    int extraGap = 0;
    if (f > 1.15f) {
        extraGap = (int)(avail - (float)sumBase * 1.15f);
        f = 1.15f;
    } else if (f < 0.72f) {
        f = 0.72f;
    }
    g_lyScale = f;

    int x0 = 92;
    int xr = W - 16;
    int mid = (x0 + xr) / 2;
    int h1 = (int)(bh1 * f);
    int h2 = (int)(bh2 * f);
    int y2 = top + h1 + gap + extraGap;
    L.card[0] = { x0, top, mid - 7, top + h1 };
    L.card[1] = { mid + 7, top, xr, top + h1 };
    L.card[2] = { x0, y2, xr, y2 + h2 };
    if (nRows == 1) {
        // single full-width card (scroll page)
        L.card[0] = { x0, top, xr, top + h1 };
        L.card[1] = { 0, 0, 0, 0 };
        L.card[2] = { 0, 0, 0, 0 };
    }

    // ---- status bar ----
    L.status = { 12, H - 52, W - 12, H - 16 };
    {
        int w = (L.status.right - L.status.left) / 5;
        L.cntChip = { L.status.left + 4 * w + 8, L.status.top + 8,
                      L.status.right - 8, L.status.bottom - 8 };
    }

    // ---- per-page controls ----
    switch (g_page) {
    case PAGE_CLICK:
        L.track[SL_L] = { L.card[0].left + 16, L.card[0].top + S(38),
                          L.card[0].right - 90, L.card[0].top + S(43) };
        L.track[SL_R] = { L.card[1].left + 16, L.card[1].top + S(38),
                          L.card[1].right - 90, L.card[1].top + S(43) };
        L.tglL = { L.card[0].right - 58, L.card[0].top + S(14),
                   L.card[0].right - 16, L.card[0].top + S(34) };
        L.tglR = { L.card[1].right - 58, L.card[1].top + S(14),
                   L.card[1].right - 16, L.card[1].top + S(34) };
        {
            int pw = (L.card[0].right - L.card[0].left - 32 - 24) / 4;
            for (int k = 0; k < 4; k++) {
                int xl = L.card[0].left + 16 + k * (pw + 8);
                int xr = L.card[1].left + 16 + k * (pw + 8);
                L.preL[k] = { xl, L.card[0].top + S(96), xl + pw, L.card[0].top + S(124) };
                L.preR[k] = { xr, L.card[1].top + S(96), xr + pw, L.card[1].top + S(124) };
            }
        }
        L.btnKey  = { L.card[2].left + 16, L.card[2].top + S(32),
                      L.card[2].right - 16, L.card[2].top + S(56) };
        L.btnKeep = { L.card[2].left + 16, L.card[2].top + S(66),
                      L.card[2].right - 16, L.card[2].top + S(90) };
        // row 3: can-attack gate toggle + live status chip + hotkey
        {
            int cw = L.card[2].right - L.card[2].left - 32;
            int w1 = (int)(cw * 0.50f), w2 = (int)(cw * 0.20f);
            L.btnCanAtk = { L.card[2].left + 16, L.card[2].top + S(96),
                            L.card[2].left + 16 + w1, L.card[2].top + S(118) };
            L.canAtkChip = { L.btnCanAtk.right + 8, L.btnCanAtk.top,
                             L.btnCanAtk.right + 8 + w2, L.btnCanAtk.bottom };
            L.btnCanAtkKey = { L.canAtkChip.right + 8, L.btnCanAtk.top,
                               L.card[2].right - 16, L.btnCanAtk.bottom };
        }
        break;
    case PAGE_MULTI:
        L.track[SL_MUL] = { L.card[0].left + 16, L.card[0].top + S(38),
                            L.card[0].right - 90, L.card[0].top + S(43) };
        L.track[SL_DEL] = { L.card[1].left + 16, L.card[1].top + S(38),
                            L.card[1].right - 90, L.card[1].top + S(43) };
        {
            int pw = (L.card[0].right - L.card[0].left - 32 - 24) / 4;
            for (int k = 0; k < 4; k++) {
                int xm = L.card[0].left + 16 + k * (pw + 8);
                int xd = L.card[1].left + 16 + k * (pw + 8);
                L.preM[k] = { xm, L.card[0].top + S(96), xm + pw, L.card[0].top + S(124) };
                L.preD[k] = { xd, L.card[1].top + S(96), xd + pw, L.card[1].top + S(124) };
            }
        }
        L.btnMKey = { L.card[2].left + 16, L.card[2].top + S(32),
                      L.card[2].right - 16, L.card[2].top + S(56) };
        break;
    case PAGE_SCROLL:
        L.tglScroll = { L.card[0].right - 58, L.card[0].top + S(16),
                        L.card[0].right - 16, L.card[0].top + S(36) };
        L.btnScrollKey = { L.card[0].left + 16, L.card[0].top + S(32),
                           L.card[0].left + 186, L.card[0].top + S(56) };
        L.btnScrollLR  = { L.card[0].left + 198, L.card[0].top + S(32),
                           L.card[0].right - 70, L.card[0].top + S(56) };
        L.btnScrollLRKey = { L.card[0].left + 198, L.card[0].top + S(66),
                             L.card[0].right - 70, L.card[0].top + S(90) };
        break;
    case PAGE_ADV:
        L.track[SL_MAX] = { L.card[0].left + 16, L.card[0].top + S(36),
                            L.card[0].right - 90, L.card[0].top + S(41) };
        L.inpMax = { L.track[SL_MAX].right + 6, L.card[0].top + S(30),
                     L.card[0].right - 16, L.card[0].top + S(47) };
        L.chkRand = { L.card[1].left + 16, L.card[1].top + S(32),
                      L.card[1].left + 170, L.card[1].top + S(54) };
        L.track[SL_RAND] = { L.card[1].left + 16, L.card[1].top + S(62),
                             L.card[1].right - 90, L.card[1].top + S(67) };
        L.chkAutoStop = { L.card[2].left + 16, L.card[2].top + S(34),
                          L.card[2].left + 124, L.card[2].top + S(56) };
        L.inpAutoStop = { L.card[2].left + 136, L.card[2].top + S(34),
                          L.card[2].left + 202, L.card[2].top + S(56) };
        break;
    }

    // ---- thumb rects ----
    for (int i = 0; i < SL_COUNT; i++) {
        L.thumb[i].top    = L.track[i].top - 7;
        L.thumb[i].bottom = L.track[i].bottom + 7;
    }

    // ---- hit rects ----
    for (auto& h : g_hr) { h.r = {}; h.hover = false; }
    g_hr[E_NAV_CLICK]  = { L.nav[0], E_NAV_CLICK, false };
    g_hr[E_NAV_MULTI]  = { L.nav[1], E_NAV_MULTI, false };
    g_hr[E_NAV_SCROLL] = { L.nav[2], E_NAV_SCROLL, false };
    g_hr[E_NAV_ADV]    = { L.nav[3], E_NAV_ADV, false };
    g_hr[E_BTN_THEME]  = { L.btnTheme, E_BTN_THEME, false };
    g_hr[E_BTN_PIN]    = { L.btnPin, E_BTN_PIN, false };

    switch (g_page) {
    case PAGE_CLICK:
        g_hr[E_TGL_L] = { L.tglL, E_TGL_L, false };
        g_hr[E_TGL_R] = { L.tglR, E_TGL_R, false };
        g_hr[E_SL_L] = { L.thumb[SL_L], E_SL_L, false };
        g_hr[E_SL_R] = { L.thumb[SL_R], E_SL_R, false };
        g_hr[E_BTN_KEY] = { L.btnKey, E_BTN_KEY, false };
        g_hr[E_BTN_KEEP] = { L.btnKeep, E_BTN_KEEP, false };
        g_hr[E_BTN_CANATK] = { L.btnCanAtk, E_BTN_CANATK, false };
        g_hr[E_CHIP_CANATK] = { L.canAtkChip, E_CHIP_CANATK, false };
        g_hr[E_BTN_CANATK_KEY] = { L.btnCanAtkKey, E_BTN_CANATK_KEY, false };
        for (int k = 0; k < 4; k++) {
            g_hr[E_PRE_L0 + k] = { L.preL[k], (Elem)(E_PRE_L0 + k), false };
            g_hr[E_PRE_R0 + k] = { L.preR[k], (Elem)(E_PRE_R0 + k), false };
        }
        break;
    case PAGE_MULTI:
        g_hr[E_SL_MUL] = { L.thumb[SL_MUL], E_SL_MUL, false };
        g_hr[E_SL_DEL] = { L.thumb[SL_DEL], E_SL_DEL, false };
        g_hr[E_BTN_MKEY] = { L.btnMKey, E_BTN_MKEY, false };
        for (int k = 0; k < 4; k++) {
            g_hr[E_PRE_M0 + k] = { L.preM[k], (Elem)(E_PRE_M0 + k), false };
            g_hr[E_PRE_D0 + k] = { L.preD[k], (Elem)(E_PRE_D0 + k), false };
        }
        break;
    case PAGE_SCROLL:
        g_hr[E_TGL_SCROLL] = { L.tglScroll, E_TGL_SCROLL, false };
        g_hr[E_BTN_SCROLL_KEY] = { L.btnScrollKey, E_BTN_SCROLL_KEY, false };
        g_hr[E_BTN_SCROLL_LR_KEY] = { L.btnScrollLRKey, E_BTN_SCROLL_LR_KEY, false };
        {
            RECT& b = L.btnScrollLR;
            int midX = (b.left + b.right) / 2;
            g_hr[E_BTN_SCROLL_L] = { { b.left, b.top, midX, b.bottom }, E_BTN_SCROLL_L, false };
            g_hr[E_BTN_SCROLL_R] = { { midX, b.top, b.right, b.bottom }, E_BTN_SCROLL_R, false };
        }
        break;
    case PAGE_ADV:
        g_hr[E_SL_MAX] = { L.thumb[SL_MAX], E_SL_MAX, false };
        g_hr[E_INP_MAX] = { L.inpMax, E_INP_MAX, false };
        g_hr[E_CHK_RAND] = { L.chkRand, E_CHK_RAND, false };
        g_hr[E_SL_RAND] = { L.thumb[SL_RAND], E_SL_RAND, false };
        g_hr[E_CHK_AUTOSTOP] = { L.chkAutoStop, E_CHK_AUTOSTOP, false };
        g_hr[E_INP_AUTOSTOP] = { L.inpAutoStop, E_INP_AUTOSTOP, false };
        break;
    }
}

// ============================================================
//  GDI init
// ============================================================
const wchar_t* g_uiFontName = L"Microsoft YaHei UI";

static void InitGDI()
{
    // ---- pick the nicest available CJK UI font on this system ----
    static const wchar_t* cnCandidates[] = {
        L"Noto Sans SC", L"HarmonyOS Sans SC", L"MiSans",
        L"Source Han Sans SC", L"Microsoft YaHei UI", L"Microsoft YaHei"
    };
    HDC dc = GetDC(nullptr);
    for (auto n : cnCandidates) {
        HFONT f = CreateFontW(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                              0, 0, CLEARTYPE_QUALITY, 0, n);
        if (!f) continue;
        HFONT old = (HFONT)SelectObject(dc, f);
        wchar_t face[128] = {};
        GetTextFaceW(dc, 128, face);
        SelectObject(dc, old);
        DeleteObject(f);
        if (_wcsicmp(face, n) == 0) { g_uiFontName = n; break; }
    }
    ReleaseDC(nullptr, dc);

    auto F = [](int h, int w, int q) -> HFONT {
        HFONT f = CreateFontW(h, 0, 0, 0, w, 0, 0, 0, DEFAULT_CHARSET, 0, 0, q, 0, g_uiFontName);
        if (!f) f = CreateFontW(h, 0, 0, 0, w, 0, 0, 0, DEFAULT_CHARSET, 0, 0, q, 0, L"Segoe UI");
        return f;
    };
    g_hfTitle = F(26, FW_BOLD,     CLEARTYPE_QUALITY);
    g_hfLabel = F(18, FW_SEMIBOLD, CLEARTYPE_QUALITY);
    g_hfBody  = F(17, FW_MEDIUM,   CLEARTYPE_QUALITY);
    g_hfSmall = F(14, FW_MEDIUM,   CLEARTYPE_QUALITY);
}

static void MakeBuf(HWND hwnd)
{
    RECT rc; GetClientRect(hwnd, &rc);
    g_cx = rc.right; g_cy = rc.bottom;
    HDC dc = GetDC(hwnd);
    g_hdcMem = CreateCompatibleDC(dc);
    g_hbmBuf = CreateCompatibleBitmap(dc, g_cx, g_cy);
    SelectObject(g_hdcMem, g_hbmBuf);
    ReleaseDC(hwnd, dc);
}
static void FreeBuf() { if (g_hbmBuf) { DeleteObject(g_hbmBuf); g_hbmBuf = nullptr; } if (g_hdcMem) { DeleteDC(g_hdcMem); g_hdcMem = nullptr; } }

// ============================================================
//  slider helpers
// ============================================================
static int ThumbX(int i)
{
    int left = L.track[i].left, span = L.track[i].right - L.track[i].left;
    if (span <= 0) return left;
    float r = 0;
    switch (i) {
    case SL_L:   r = (float)(cpsLeft10 - CPS_MIN10) / (float)(cpsMax * 10 - CPS_MIN10); break;
    case SL_R:   r = (float)(cpsRight10 - CPS_MIN10) / (float)(cpsMax * 10 - CPS_MIN10); break;
    case SL_MUL: r = (float)(multiMul - 1) / 4.0f; break;
    case SL_DEL: r = (float)(multiDelayMs - 1) / 199.0f; break;
    case SL_MAX: r = (float)(cpsMax - CPS_LIMIT_MIN) / (float)(CPS_LIMIT_MAX - CPS_LIMIT_MIN); break;
    case SL_RAND:r = (float)(randomCpsRange - 1) / 4.0f; break;
    }
    if (r < 0) r = 0;
    if (r > 1) r = 1;
    return left + (int)(r * span);
}
static void UpThumbs()
{
    for (int i = 0; i < SL_COUNT; i++) {
        int cx = ThumbX(i);
        L.thumb[i].left = cx - 8;
        L.thumb[i].right = cx + 8;
    }
    // sync hit rects of the active page's sliders
    auto sync = [](Elem e, int s) { g_hr[e].r = L.thumb[s]; };
    switch (g_page) {
    case PAGE_CLICK: sync(E_SL_L, SL_L); sync(E_SL_R, SL_R); break;
    case PAGE_MULTI: sync(E_SL_MUL, SL_MUL); sync(E_SL_DEL, SL_DEL); break;
    case PAGE_ADV:   sync(E_SL_MAX, SL_MAX); sync(E_SL_RAND, SL_RAND); break;
    default: break;
    }
}

// ============================================================
//  drawing helpers (Neumorphism)
// ============================================================
static void FillRoundRect(HDC dc, const RECT& r, int radius, COLORREF fill) {
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = (HPEN)GetStockObject(NULL_PEN);
    SelectObject(dc, p); SelectObject(dc, b);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius * 2, radius * 2);
    DeleteObject(b);
}
static void DrawRoundRect(HDC dc, const RECT& r, int radius, COLORREF border, int width) {
    HPEN p = CreatePen(PS_SOLID, width, border);
    SelectObject(dc, p); SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius * 2, radius * 2);
    DeleteObject(p);
}
static COLORREF LerpC(COLORREF a, COLORREF b, float t) {
    return RGB((int)(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t),
               (int)(GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t),
               (int)(GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t));
}

// exact rounded-rect region
static HRGN MakeRRgn(const RECT& r, int radius)
{
    int ew = radius * 2, eh = radius * 2;
    if (ew < 1) ew = 1;
    if (eh < 1) eh = 1;
    return CreateRoundRectRgn(r.left, r.top, r.right, r.bottom, ew, eh);
}

// raised surface: concentric dark/light crescents (uniform rounded distance),
// clipped to the bottom-right / top-left quadrants for the neumorphic light
static void NeuRaised(HDC dc, const RECT& r, int radius, int depth, COLORREF base)
{
    HRGN rgnBtn = MakeRRgn(r, radius);
    int cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    // dark shadow: inner layers first (darker near the element)
    for (int i = 1; i <= depth; ++i) {
        float t = (float)(depth - i + 1) / (float)(depth + 1);
        RECT rr = { r.left - i, r.top - i, r.right + i, r.bottom + i };
        HRGN rgnExp = MakeRRgn(rr, radius + i);
        HRGN rgnM = CreateRectRgn(0, 0, 0, 0);
        HRGN rgnQ = CreateRectRgn(cx, cy, rr.right, rr.bottom);
        HRGN rgnF = CreateRectRgn(0, 0, 0, 0);
        CombineRgn(rgnM, rgnExp, rgnBtn, RGN_DIFF);
        CombineRgn(rgnF, rgnM, rgnQ, RGN_AND);
        HBRUSH b = CreateSolidBrush(LerpC(base, SHADOW_DARK(), t));
        FillRgn(dc, rgnF, b);
        DeleteObject(b); DeleteObject(rgnExp); DeleteObject(rgnM); DeleteObject(rgnQ); DeleteObject(rgnF);
    }
    // light highlight: top-left quadrant
    for (int i = 1; i <= depth; ++i) {
        float t = (float)(depth - i + 1) / (float)(depth + 1);
        RECT rr = { r.left - i, r.top - i, r.right + i, r.bottom + i };
        HRGN rgnExp = MakeRRgn(rr, radius + i);
        HRGN rgnM = CreateRectRgn(0, 0, 0, 0);
        HRGN rgnQ = CreateRectRgn(rr.left, rr.top, cx, cy);
        HRGN rgnF = CreateRectRgn(0, 0, 0, 0);
        CombineRgn(rgnM, rgnExp, rgnBtn, RGN_DIFF);
        CombineRgn(rgnF, rgnM, rgnQ, RGN_AND);
        HBRUSH b = CreateSolidBrush(LerpC(base, SHADOW_LIGHT(), t));
        FillRgn(dc, rgnF, b);
        DeleteObject(b); DeleteObject(rgnExp); DeleteObject(rgnM); DeleteObject(rgnQ); DeleteObject(rgnF);
    }
    FillRoundRect(dc, r, radius, base);
    DeleteObject(rgnBtn);
}

// inset groove: concentric eroded strips keep the rounded corners perfectly
// aligned; upper half = dark shadow, lower half = light highlight
static void NeuInset(HDC dc, const RECT& r, int radius, int depth, COLORREF base)
{
    FillRoundRect(dc, r, radius, base);
    HRGN rgnBtn = MakeRRgn(r, radius);
    int cy = (r.top + r.bottom) / 2;
    // dark inner shadow: upper half of the strip
    for (int i = depth; i >= 1; --i) {
        float t = (float)(depth - i + 1) / (float)(depth + 1) * 0.8f;
        RECT rr = { r.left + i, r.top + i, r.right - i, r.bottom - i };
        HRGN rgnEr = MakeRRgn(rr, radius - i);
        HRGN rgnStrip = CreateRectRgn(0, 0, 0, 0);
        HRGN rgnHalf = CreateRectRgn(r.left, r.top, r.right, cy);
        HRGN rgnF = CreateRectRgn(0, 0, 0, 0);
        CombineRgn(rgnStrip, rgnBtn, rgnEr, RGN_DIFF);
        CombineRgn(rgnF, rgnStrip, rgnHalf, RGN_AND);
        HBRUSH b = CreateSolidBrush(LerpC(base, SHADOW_DARK(), t));
        FillRgn(dc, rgnF, b);
        DeleteObject(b); DeleteObject(rgnEr); DeleteObject(rgnStrip); DeleteObject(rgnHalf); DeleteObject(rgnF);
    }
    // light inner highlight: lower half of the strip
    for (int i = depth; i >= 1; --i) {
        float t = (float)(depth - i + 1) / (float)(depth + 1) * 0.6f;
        RECT rr = { r.left + i, r.top + i, r.right - i, r.bottom - i };
        HRGN rgnEr = MakeRRgn(rr, radius - i);
        HRGN rgnStrip = CreateRectRgn(0, 0, 0, 0);
        HRGN rgnHalf = CreateRectRgn(r.left, cy, r.right, r.bottom);
        HRGN rgnF = CreateRectRgn(0, 0, 0, 0);
        CombineRgn(rgnStrip, rgnBtn, rgnEr, RGN_DIFF);
        CombineRgn(rgnF, rgnStrip, rgnHalf, RGN_AND);
        HBRUSH b = CreateSolidBrush(LerpC(base, SHADOW_LIGHT(), t));
        FillRgn(dc, rgnF, b);
        DeleteObject(b); DeleteObject(rgnEr); DeleteObject(rgnStrip); DeleteObject(rgnHalf); DeleteObject(rgnF);
    }
    DeleteObject(rgnBtn);
}

// accent groove (active states) with exact concentric inner shadows
static void NeuInsetAccent(HDC dc, const RECT& r, int radius, int depth)
{
    COLORREF a = ACCENT();
    FillRoundRect(dc, r, radius, a);
    HRGN rgnBtn = MakeRRgn(r, radius);
    int cy = (r.top + r.bottom) / 2;
    for (int i = depth; i >= 1; --i) {
        float t = (float)(depth - i + 1) / (float)(depth + 1) * 0.55f;
        RECT rr = { r.left + i, r.top + i, r.right - i, r.bottom - i };
        HRGN rgnEr = MakeRRgn(rr, radius - i);
        HRGN rgnStrip = CreateRectRgn(0, 0, 0, 0);
        HRGN rgnHalf = CreateRectRgn(r.left, r.top, r.right, cy);
        HRGN rgnF = CreateRectRgn(0, 0, 0, 0);
        CombineRgn(rgnStrip, rgnBtn, rgnEr, RGN_DIFF);
        CombineRgn(rgnF, rgnStrip, rgnHalf, RGN_AND);
        HBRUSH b = CreateSolidBrush(LerpC(a, RGB(0, 0, 0), t));
        FillRgn(dc, rgnF, b);
        DeleteObject(b); DeleteObject(rgnEr); DeleteObject(rgnStrip); DeleteObject(rgnHalf); DeleteObject(rgnF);
    }
    for (int i = depth; i >= 1; --i) {
        float t = (float)(depth - i + 1) / (float)(depth + 1) * 0.4f;
        RECT rr = { r.left + i, r.top + i, r.right - i, r.bottom - i };
        HRGN rgnEr = MakeRRgn(rr, radius - i);
        HRGN rgnStrip = CreateRectRgn(0, 0, 0, 0);
        HRGN rgnHalf = CreateRectRgn(r.left, cy, r.right, r.bottom);
        HRGN rgnF = CreateRectRgn(0, 0, 0, 0);
        CombineRgn(rgnStrip, rgnBtn, rgnEr, RGN_DIFF);
        CombineRgn(rgnF, rgnStrip, rgnHalf, RGN_AND);
        HBRUSH b = CreateSolidBrush(LerpC(a, RGB(255, 255, 255), t));
        FillRgn(dc, rgnF, b);
        DeleteObject(b); DeleteObject(rgnEr); DeleteObject(rgnStrip); DeleteObject(rgnHalf); DeleteObject(rgnF);
    }
    DeleteObject(rgnBtn);
}

// soft drop shadow: concentric crescent below the element
static void NeuDropShadow(HDC dc, const RECT& r, int radius, int depth, COLORREF base)
{
    HRGN rgnBtn = MakeRRgn(r, radius);
    int cx = (r.left + r.right) / 2;
    for (int i = 1; i <= depth; ++i) {
        float t = (float)(depth - i + 1) / (float)(depth + 1);
        RECT rr = { r.left - i, r.top - i + 1, r.right + i, r.bottom + i + 1 };
        HRGN rgnExp = MakeRRgn(rr, radius + i);
        HRGN rgnM = CreateRectRgn(0, 0, 0, 0);
        HRGN rgnQ = CreateRectRgn(rr.left, (r.top + r.bottom) / 2, rr.right, rr.bottom);
        HRGN rgnF = CreateRectRgn(0, 0, 0, 0);
        CombineRgn(rgnM, rgnExp, rgnBtn, RGN_DIFF);
        CombineRgn(rgnF, rgnM, rgnQ, RGN_AND);
        HBRUSH b = CreateSolidBrush(LerpC(base, SHADOW_DARK(), t));
        FillRgn(dc, rgnF, b);
        DeleteObject(b); DeleteObject(rgnExp); DeleteObject(rgnM); DeleteObject(rgnQ); DeleteObject(rgnF);
    }
    DeleteObject(rgnBtn);
}

// generic button: raised with soft rounded accent glow on hover, insets when pressed
static void NeuButton(HDC dc, const RECT& r, int radius, bool hover, bool pressed)
{
    if (pressed) { NeuInset(dc, r, radius, 2, BTN()); return; }
    COLORREF base = BTN();
    if (hover) {
        // layered rounded glow: darker near the button, fading outward
        for (int i = 3; i >= 1; --i) {
            float t = (float)(4 - i) / 4.0f * 0.22f;
            RECT rr = { r.left - i, r.top - i, r.right + i, r.bottom + i };
            FillRoundRect(dc, rr, radius + i, LerpC(BG(), ACCENT(), t));
        }
        base = LerpC(base, SHADOW_LIGHT(), 0.45f);
    }
    NeuRaised(dc, r, radius, 3, base);
}

static void DrawCheck(HDC dc, const RECT& sq)
{
    HPEN wp = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
    SelectObject(dc, wp);
    MoveToEx(dc, sq.left + 3, sq.top + 8, nullptr);
    LineTo(dc, sq.left + 7, sq.top + 12);
    LineTo(dc, sq.left + 13, sq.top + 5);
    DeleteObject(wp);
}

static void DrawPin(HDC dc, int cx, int cy, bool on)
{
    COLORREF c = on ? RGB(255, 255, 255) : TXT_DIM();
    HPEN p = CreatePen(PS_SOLID, 2, c);
    HBRUSH b = CreateSolidBrush(c);
    SelectObject(dc, p); SelectObject(dc, b);
    Ellipse(dc, cx - 3, cy - 11, cx + 3, cy - 5);
    MoveToEx(dc, cx, cy - 5, nullptr);
    LineTo(dc, cx, cy + 2);
    POINT tri[3] = { { cx - 5, cy + 2 }, { cx + 5, cy + 2 }, { cx, cy + 9 } };
    Polygon(dc, tri, 3);
    DeleteObject(p); DeleteObject(b);
}

// ---- sidebar icons: drawn inside a 22x22 box centered at (cx, cy) ----
static void DrawMouseIcon(HDC dc, int cx, int cy, COLORREF c)
{
    HPEN p = CreatePen(PS_SOLID, 2, c);
    SelectObject(dc, p);
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, cx - 8, cy - 7, cx + 8, cy + 8, 7, 7);
    MoveToEx(dc, cx, cy - 7, nullptr);
    LineTo(dc, cx, cy - 1);
    MoveToEx(dc, cx - 4, cy - 1, nullptr);
    LineTo(dc, cx + 4, cy - 1);
    DeleteObject(p);
}
static void DrawMultiIcon(HDC dc, int cx, int cy, COLORREF c)
{
    HPEN p = CreatePen(PS_SOLID, 2, c);
    SelectObject(dc, p);
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, cx - 7, cy - 8, cx + 1, cy, 3, 3);
    RoundRect(dc, cx - 1, cy, cx + 7, cy + 8, 3, 3);
    DeleteObject(p);
}
static void DrawWheelIcon(HDC dc, int cx, int cy, COLORREF c)
{
    HPEN p = CreatePen(PS_SOLID, 2, c);
    SelectObject(dc, p);
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc, cx - 4, cy - 7, cx + 4, cy + 7);
    MoveToEx(dc, cx, cy - 2, nullptr);
    LineTo(dc, cx, cy + 2);
    MoveToEx(dc, cx - 3, cy - 10, nullptr);
    LineTo(dc, cx, cy - 13);
    LineTo(dc, cx + 3, cy - 10);
    MoveToEx(dc, cx - 3, cy + 10, nullptr);
    LineTo(dc, cx, cy + 13);
    LineTo(dc, cx + 3, cy + 10);
    DeleteObject(p);
}
static void DrawAdvIcon(HDC dc, int cx, int cy, COLORREF c)
{
    HPEN p = CreatePen(PS_SOLID, 2, c);
    HBRUSH b = CreateSolidBrush(c);
    SelectObject(dc, p); SelectObject(dc, b);
    for (int i = 0; i < 3; i++) {
        int yy = cy - 7 + i * 7;
        MoveToEx(dc, cx - 7, yy, nullptr);
        LineTo(dc, cx + 7, yy);
        int kx = cx - 3 + i * 3;
        Ellipse(dc, kx - 2, yy - 2, kx + 2, yy + 2);
    }
    DeleteObject(p); DeleteObject(b);
}

// ============================================================
//  render
// ============================================================
static void Paint()
{
    HDC dc = g_hdcMem;
    if (!dc || g_cx <= 0 || g_cy <= 0) return;
    SetBkMode(dc, TRANSPARENT);

    // ---- background ----
    { HBRUSH b = CreateSolidBrush(BG()); RECT a = { 0, 0, g_cx, g_cy }; FillRect(dc, &a, b); DeleteObject(b); }

    // ---- title (soft double drop-shadow text) ----
    {
        RECT tr = L.title;
        tr.left += 8; tr.top += 2; tr.right = L.btnPin.left - 12;
        SelectObject(dc, g_hfTitle);
        RECT sh2 = tr; sh2.left += 2; sh2.top += 2;
        SetTextColor(dc, LerpC(SHADOW_DARK(), BG(), 0.5f));
        DrawTextW(dc, L"AutoClicker", -1, &sh2, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT sh = tr; sh.left += 1; sh.top += 1;
        SetTextColor(dc, SHADOW_DARK());
        DrawTextW(dc, L"AutoClicker", -1, &sh, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SetTextColor(dc, TXT());
        DrawTextW(dc, L"AutoClicker", -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT vr = { tr.left, tr.top + 26, tr.right, tr.bottom };
        SetTextColor(dc, TXT_DIM());
        SelectObject(dc, g_hfSmall);
        DrawTextW(dc, L"v2.5", -1, &vr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    // ---- pin / theme buttons (with hover hints) ----
    {
        RECT& b = L.btnPin;
        bool hover = g_hr[E_BTN_PIN].hover;
        if (topmost) NeuInsetAccent(dc, b, 13, 2);
        else NeuButton(dc, b, 13, hover, false);
        int cx = (b.left + b.right) / 2, cy = (b.top + b.bottom) / 2;
        DrawPin(dc, cx, cy, topmost);
        if (hover) {
            RECT tip = { b.left - 10, 42, b.right + 10, 58 };
            SetTextColor(dc, TXT_DIM());
            SelectObject(dc, g_hfSmall);
            DrawTextW(dc, topmost ? L"\x53d6\x6d88\x7f6e\x9876" : L"\x7f6e\x9876\x7a97\x53e3",
                      -1, &tip, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }
    {
        RECT& b = L.btnTheme;
        bool hover = g_hr[E_BTN_THEME].hover;
        NeuButton(dc, b, 13, hover, false);
        SelectObject(dc, g_hfBody);
        SetTextColor(dc, TXT());
        DrawTextW(dc, g_theme == Theme::Dark ? L"\x2600" : L"\x263E",
                  -1, (RECT*)&b, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (hover) {
            RECT tip = { b.left - 10, 42, b.right + 10, 58 };
            SetTextColor(dc, TXT_DIM());
            SelectObject(dc, g_hfSmall);
            DrawTextW(dc, g_theme == Theme::Dark ? L"\x5207\x6362\x4eae\x8272" : L"\x5207\x6362\x6df1\x8272",
                      -1, &tip, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }

    // ---- sidebar ----
    NeuRaised(dc, L.sidebar, 14, 4, CARD());
    static const wchar_t* navNames[4] = { L"\x8fde\x70b9", L"\x591a\x500d", L"\x6eda\x8f6e", L"\x9ad8\x7ea7" };
    for (int i = 0; i < 4; i++) {
        RECT& b = L.nav[i];
        bool sel = ((int)g_page == i);
        bool hover = g_hr[E_NAV_CLICK + i].hover;
        if (sel) {
            NeuDropShadow(dc, b, 10, 2, BG());  // exact rounded shadow
            NeuInsetAccent(dc, b, 10, 2);
        } else {
            NeuButton(dc, b, 10, hover, false);
        }
        int cx = (b.left + b.right) / 2;
        int icY = b.top + 13;   // icon sits comfortably inside the button
        COLORREF ic = sel ? RGB(255, 255, 255) : (hover ? ACCENT() : TXT_DIM());
        switch (i) {
        case 0: DrawMouseIcon(dc, cx, icY, ic); break;
        case 1: DrawMultiIcon(dc, cx, icY, ic); break;
        case 2: DrawWheelIcon(dc, cx, icY, ic); break;
        case 3: DrawAdvIcon(dc, cx, icY, ic); break;
        }
        RECT lr = { b.left, b.top + 32, b.right, b.bottom - 3 };
        SetTextColor(dc, sel ? RGB(255, 255, 255) : (hover ? ACCENT() : TXT_DIM()));
        SelectObject(dc, g_hfSmall);
        DrawTextW(dc, navNames[i], -1, &lr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    // ---- content cards ----
    int cardCount = 0;
    switch (g_page) {
    case PAGE_CLICK: cardCount = 3; break;
    case PAGE_MULTI: cardCount = 3; break;
    case PAGE_SCROLL: cardCount = 1; break;
    case PAGE_ADV: cardCount = 3; break;
    default: break;
    }
    for (int i = 0; i < cardCount; i++)
        NeuRaised(dc, L.card[i], 14, 4, CARD());

    // ---- card labels ----
    SelectObject(dc, g_hfLabel);
    SetTextColor(dc, TXT());
    static const wchar_t* namesClick[3] = {
        L"\x5de6\x952e", L"\x53f3\x952e", L"\x5feb\x6377\x952e \x4e0e \x4fdd\x6301"
    };
    static const wchar_t* namesMulti[3] = {
        L"\x500d\x7387", L"\x5ef6\x8fdf", L"\x5feb\x6377\x952e"
    };
    static const wchar_t* namesScroll[1] = { L"\x6eda\x8f6e\x70b9\x51fb" };
    static const wchar_t* namesAdv[3] = {
        L"CPS \x4e0a\x9650", L"\x968f\x673a CPS", L"\x5b9a\x65f6\x505c\x6b62"
    };
    const wchar_t** names = namesClick;
    switch (g_page) {
    case PAGE_CLICK: names = namesClick; break;
    case PAGE_MULTI: names = namesMulti; break;
    case PAGE_SCROLL: names = namesScroll; break;
    case PAGE_ADV: names = namesAdv; break;
    }
    for (int i = 0; i < cardCount; i++) {
        RECT r = { L.card[i].left + 20, L.card[i].top + S(8),
                   L.card[i].right, L.card[i].top + S(26) };
        DrawTextW(dc, names[i], -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    auto DrawToggle = [&](const RECT& tg, bool on) {
        if (on) NeuInsetAccent(dc, tg, 12, 2);
        else NeuInset(dc, tg, 12, 2, BTN());
        int kw = tg.right - tg.left;
        int cy = (tg.top + tg.bottom) / 2, kr = (tg.bottom - tg.top) / 2 - 3;
        int kx = on ? tg.right - kr - 3 : tg.left + kr + 3;
        RECT krr = { kx - kr, cy - kr, kx + kr, cy + kr };
        // knob soft shadow
        for (int i = 2; i >= 1; --i) {
            RECT sr = { krr.left + i, krr.top + i, krr.right + i, krr.bottom + i };
            FillRoundRect(dc, sr, kr, LerpC(BTN(), SHADOW_DARK(), (float)i / 3.0f));
        }
        NeuRaised(dc, krr, kr, 2, on ? RGB(255, 255, 255) : BTN());
        if (on) DrawRoundRect(dc, krr, kr, LerpC(ACCENT(), RGB(0, 0, 0), 0.3f), 1);
    };

    auto DrawSlider = [&](int si, Elem elem) {
        RECT& trk = L.track[si];
        bool active = g_hr[elem].hover || g_drag == elem;
        COLORREF trkBase = active ? LerpC(TRACK(), SHADOW_LIGHT(), 0.25f) : TRACK();
        NeuInset(dc, trk, 4, 2, trkBase);
        int fx = ThumbX(si);
        if (fx > trk.left + 1) {
            RECT r = { trk.left, trk.top, fx, trk.bottom };
            FillRoundRect(dc, r, 4, ACCENT());
        }
        int cx = ThumbX(si), cy = (trk.top + trk.bottom) / 2, r = 8;
        RECT tr = { cx - r, cy - r, cx + r, cy + r };
        bool thumbHover = g_hr[elem].hover || g_drag == elem;
        // thumb soft shadow
        for (int i = 2; i >= 1; --i) {
            RECT sr = { tr.left + i, tr.top + i, tr.right + i, tr.bottom + i };
            FillRoundRect(dc, sr, r, LerpC(BTN(), SHADOW_DARK(), (float)i / 3.0f));
        }
        NeuRaised(dc, tr, r, 2, BTN());
        // glow ring while hovering/dragging
        if (thumbHover) {
            for (int i = 2; i >= 1; --i) {
                RECT gr = { tr.left - i, tr.top - i, tr.right + i, tr.bottom + i };
                DrawRoundRect(dc, gr, r + i, LerpC(ACCENT(), BG(), 0.35f + 0.2f * i), 1);
            }
        }
        DrawRoundRect(dc, tr, r,
                      thumbHover ? LerpC(ACCENT(), RGB(255, 255, 255), 0.4f) : ACCENT(), 2);
    };

    auto DrawPresetRow = [&](const RECT* pre, int baseElem,
                             const wchar_t* const* labels, int selIdx) {
        SelectObject(dc, g_hfSmall);
        for (int k = 0; k < 4; k++) {
            const RECT& b = pre[k];
            bool sel = (selIdx == k);
            bool hover = g_hr[baseElem + k].hover;
            if (sel) NeuInsetAccent(dc, b, 8, 2);
            else NeuButton(dc, b, 8, hover, false);
            SetTextColor(dc, sel ? RGB(255, 255, 255) : TXT());
            DrawTextW(dc, labels[k], -1, (RECT*)&b, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    };

    // ================= PAGE: CLICK =================
    if (g_page == PAGE_CLICK) {
        DrawToggle(L.tglL, leftenabled);
        DrawToggle(L.tglR, rightenabled);
        DrawSlider(SL_L, E_SL_L);
        DrawSlider(SL_R, E_SL_R);

        static const wchar_t* cpsLbl[4] = { L"6/s", L"10/s", L"15/s", L"20/s" };
        int selL = -1, selR = -1;
        for (int k = 0; k < 4; k++) {
            if (cpsLeft10  == kCpsPresets[k] * 10) selL = k;
            if (cpsRight10 == kCpsPresets[k] * 10) selR = k;
        }
        DrawPresetRow(L.preL, E_PRE_L0, cpsLbl, selL);
        DrawPresetRow(L.preR, E_PRE_R0, cpsLbl, selR);

        // values
        SelectObject(dc, g_hfLabel);
        for (int i = 0; i < 2; i++) {
            int c10 = (i == 0) ? cpsLeft10 : cpsRight10;
            int ms  = (i == 0) ? leftms : rightms;
            wchar_t buf[32];
            swprintf(buf, 32, L"%.1f \x6b21/\x79d2", c10 / 10.0f);
            RECT r = { L.card[i].left + 20, L.card[i].top + 60,
                       L.track[SL_L + i].right, L.card[i].top + 86 };
            SetTextColor(dc, TXT());
            DrawTextW(dc, buf, -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            swprintf(buf, 32, L"%d \x6beb\x79d2", ms);
            SetTextColor(dc, TXT_DIM());
            RECT r2 = { r.right + 8, r.top, L.card[i].right - 8, r.bottom };
            DrawTextW(dc, buf, -1, &r2, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }

        // hotkey
        {
            RECT& b = L.btnKey;
            std::wstring t = (g_rebinding == E_BTN_KEY)
                ? L"\x8bf7\x6309\x4e0b\x65b0\x952e\x2026"
                : L"\x5feb\x6377\x952e: " + getKeyName(vk_key);
            if (g_rebinding == E_BTN_KEY) NeuInsetAccent(dc, b, 10, 2);
            else NeuButton(dc, b, 10, g_hr[E_BTN_KEY].hover, false);
            SelectObject(dc, g_hfBody);
            SetTextColor(dc, g_rebinding == E_BTN_KEY ? RGB(255, 255, 255) : TXT());
            DrawTextW(dc, t.c_str(), -1, (RECT*)&b, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        // keep mode
        {
            RECT& b = L.btnKeep;
            bool hover = g_hr[E_BTN_KEEP].hover;
            if (keepClicke) NeuInsetAccent(dc, b, 10, 2);
            else NeuButton(dc, b, 10, hover, false);
            SelectObject(dc, g_hfBody);
            SetTextColor(dc, keepClicke ? RGB(255, 255, 255) : TXT());
            DrawTextW(dc, keepClicke ? L"\x4e0d\x9700\x8981\x6309\x4f4f\x8fde\x70b9: \x5f00"
                                     : L"\x4e0d\x9700\x8981\x6309\x4f4f\x8fde\x70b9: \x5173",
                      -1, (RECT*)&b, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        // can-attack gate: toggle + live status chip + hotkey
        {
            RECT& b = L.btnCanAtk;
            bool hover = g_hr[E_BTN_CANATK].hover;
            if (canAttackOnlyClick) NeuInsetAccent(dc, b, 10, 2);
            else NeuButton(dc, b, 10, hover, false);
            SelectObject(dc, g_hfBody);
            SetTextColor(dc, canAttackOnlyClick ? RGB(255, 255, 255) : TXT());
            DrawTextW(dc, canAttackOnlyClick
                              ? L"\x4ec5\x80fd\x653b\x51fb\x65f6\x8fde\x70b9: \x5f00"
                              : L"\x4ec5\x80fd\x653b\x51fb\x65f6\x8fde\x70b9: \x5173",
                      -1, (RECT*)&b, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        {
            // live 0/1 from the UDP stream: 可攻击 / 不可攻击 / 未连接
            RECT& chip = L.canAtkChip;
            COLORREF cc = TXT_DIM();
            const wchar_t* txt = L"\x672a\x8fde\x63a5";
            if (CanAttackConnected()) {
                if (g_canAttack.load(std::memory_order_relaxed) == 1) {
                    cc = GREEN(); txt = L"\x53ef\x653b\x51fb";
                } else {
                    cc = RED(); txt = L"\x4e0d\x53ef\x653b\x51fb";
                }
            }
            NeuInset(dc, chip, 8, 2, BTN());
            SetTextColor(dc, cc);
            SelectObject(dc, g_hfSmall);
            DrawTextW(dc, txt, -1, (RECT*)&chip, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        {
            RECT& b = L.btnCanAtkKey;
            std::wstring t = (g_rebinding == E_BTN_CANATK_KEY)
                ? L"\x8bf7\x6309\x4e0b\x65b0\x952e\x2026"
                : L"\x5feb\x6377\x952e: " + getKeyName(vk_canattack_key);
            if (g_rebinding == E_BTN_CANATK_KEY) NeuInsetAccent(dc, b, 10, 2);
            else NeuButton(dc, b, 10, g_hr[E_BTN_CANATK_KEY].hover, false);
            SelectObject(dc, g_hfBody);
            SetTextColor(dc, g_rebinding == E_BTN_CANATK_KEY ? RGB(255, 255, 255) : TXT());
            DrawTextW(dc, t.c_str(), -1, (RECT*)&b, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }

    // ================= PAGE: MULTI =================
    if (g_page == PAGE_MULTI) {
        DrawSlider(SL_MUL, E_SL_MUL);
        DrawSlider(SL_DEL, E_SL_DEL);

        static const wchar_t* mulLbl[4] = { L"2x", L"3x", L"4x", L"5x" };
        static const wchar_t* delLbl[4] = { L"10", L"25", L"50", L"100" };
        int selM = -1, selD = -1;
        for (int k = 0; k < 4; k++) {
            if (multiMul == kMulPresets[k])       selM = k;
            if (multiDelayMs == kDelayPresets[k]) selD = k;
        }
        DrawPresetRow(L.preM, E_PRE_M0, mulLbl, selM);
        DrawPresetRow(L.preD, E_PRE_D0, delLbl, selD);

        // values
        SelectObject(dc, g_hfLabel);
        wchar_t buf[32];
        swprintf(buf, 32, L"%d \x500d", multiMul);
        RECT r0 = { L.card[0].left + 20, L.card[0].top + 60,
                    L.track[SL_MUL].right, L.card[0].top + 86 };
        SetTextColor(dc, TXT());
        DrawTextW(dc, buf, -1, &r0, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        swprintf(buf, 32, L"%d \x6beb\x79d2", multiDelayMs);
        RECT r1 = { L.card[1].left + 20, L.card[1].top + 60,
                    L.track[SL_DEL].right, L.card[1].top + 86 };
        SetTextColor(dc, TXT());
        DrawTextW(dc, buf, -1, &r1, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // hotkey
        {
            RECT& b = L.btnMKey;
            std::wstring t = (g_rebinding == E_BTN_MKEY)
                ? L"\x8bf7\x6309\x4e0b\x65b0\x952e\x2026"
                : L"\x5feb\x6377\x952e: " + getKeyName(vk_multi_key);
            if (g_rebinding == E_BTN_MKEY) NeuInsetAccent(dc, b, 10, 2);
            else NeuButton(dc, b, 10, g_hr[E_BTN_MKEY].hover, false);
            SelectObject(dc, g_hfBody);
            SetTextColor(dc, g_rebinding == E_BTN_MKEY ? RGB(255, 255, 255) : TXT());
            DrawTextW(dc, t.c_str(), -1, (RECT*)&b, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        // hint
        RECT hint = { L.card[2].left + 20, L.card[2].top + S(68),
                      L.card[2].right - 20, L.card[2].top + S(90) };
        SetTextColor(dc, TXT_DIM());
        SelectObject(dc, g_hfSmall);
        DrawTextW(dc, L"\x6309 + / - \x952e\x53ef\x5feb\x901f\x5fae\x8c03\x500d\x6570",
                  -1, &hint, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    // ================= PAGE: SCROLL =================
    if (g_page == PAGE_SCROLL) {
        DrawToggle(L.tglScroll, isScrollClickActive);

        auto DrawKeyButton = [&](const RECT& b, const std::wstring& label,
                                 Elem elem, bool rebinding) {
            if (rebinding) NeuInsetAccent(dc, b, 10, 2);
            else NeuButton(dc, b, 10, g_hr[elem].hover, false);
            SelectObject(dc, g_hfBody);
            SetTextColor(dc, rebinding ? RGB(255, 255, 255) : TXT());
            DrawTextW(dc, label.c_str(), -1, (RECT*)&b, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        };
        {
            std::wstring t = (g_rebinding == E_BTN_SCROLL_KEY)
                ? L"\x8bf7\x6309\x4e0b\x65b0\x952e\x2026"
                : L"\x5feb\x6377\x952e: " + getKeyName(vk_scroll_key);
            DrawKeyButton(L.btnScrollKey, t, E_BTN_SCROLL_KEY,
                          g_rebinding == E_BTN_SCROLL_KEY);
        }
        {
            std::wstring t = (g_rebinding == E_BTN_SCROLL_LR_KEY)
                ? L"\x8bf7\x6309\x4e0b\x65b0\x952e\x2026"
                : L"\x5207\x6362 L/R: " + getKeyName(vk_scroll_lr_key);
            DrawKeyButton(L.btnScrollLRKey, t, E_BTN_SCROLL_LR_KEY,
                          g_rebinding == E_BTN_SCROLL_LR_KEY);
        }
        // left/right segmented selector
        {
            RECT& b = L.btnScrollLR;
            int midX = (b.left + b.right) / 2;
            NeuRaised(dc, b, 8, 3, BTN());
            SaveDC(dc);
            IntersectClipRect(dc,
                scrollClickButton == 0 ? b.left : midX, b.top,
                scrollClickButton == 0 ? midX : b.right, b.bottom);
            NeuInsetAccent(dc, b, 8, 2);
            RestoreDC(dc, -1);
            DrawRoundRect(dc, b, 8, LerpC(BG(), SHADOW_DARK(), 0.35f), 1); // unified outline
            SelectObject(dc, g_hfBody);
            RECT rl = { b.left, b.top, midX, b.bottom };
            RECT rr = { midX, b.top, b.right, b.bottom };
            SetTextColor(dc, scrollClickButton == 0 ? RGB(255, 255, 255) : TXT_DIM());
            DrawTextW(dc, L"\x5de6\x952e", -1, &rl, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SetTextColor(dc, scrollClickButton == 1 ? RGB(255, 255, 255) : TXT_DIM());
            DrawTextW(dc, L"\x53f3\x952e", -1, &rr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        // hint
        RECT hint = { L.card[0].left + 20, L.card[0].top + S(110),
                      L.card[0].right - 20, L.card[0].top + S(132) };
        SetTextColor(dc, TXT_DIM());
        SelectObject(dc, g_hfSmall);
        DrawTextW(dc, L"\x6eda\x52a8\x6eda\x8f6e\x65f6\x89e6\x53d1\x70b9\x51fb\xff0c\x5411\x4e0a/\x5411\x4e0b\x5747\x53ef",
                  -1, &hint, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    // ================= PAGE: ADVANCED =================
    if (g_page == PAGE_ADV) {
        // CPS limit
        DrawSlider(SL_MAX, E_SL_MAX);
        {
            RECT& bi = L.inpMax;
            bool focus = g_inputOn && g_inputTarget == IN_CPSMAX;
            NeuInset(dc, bi, 6, 2, BTN());
            if (focus) {
                // soft rounded focus glow
                for (int i = 2; i >= 1; --i)
                    DrawRoundRect(dc, { bi.left - i, bi.top - i, bi.right + i, bi.bottom + i },
                                  6 + i, LerpC(ACCENT(), BG(), 0.25f + 0.2f * i), 1);
                DrawRoundRect(dc, bi, 6, ACCENT(), 2);
            }
            SetTextColor(dc, TXT());
            SelectObject(dc, g_hfBody);
            if (focus) DrawTextW(dc, g_inputBuf, -1, &bi, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            else {
                wchar_t ibuf[8];
                swprintf(ibuf, 8, L"%d", cpsMax);
                DrawTextW(dc, ibuf, -1, &bi, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            RECT sec = { bi.right + 6, bi.top, bi.right + 60, bi.bottom };
            SetTextColor(dc, TXT_DIM());
            SelectObject(dc, g_hfSmall);
            DrawTextW(dc, L"\x6b21/\x79d2", -1, &sec, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }

        // random CPS
        {
            RECT& cb = L.chkRand;
            int box = cb.left;
            int by = (cb.top + cb.bottom) / 2;
            RECT sq = { box, by - 8, box + 16, by + 8 };
            bool hover = g_hr[E_CHK_RAND].hover;
            if (randomCpsEnabled) NeuInsetAccent(dc, sq, 4, 2);
            else NeuInset(dc, sq, 4, 2, hover ? LerpC(BTN(), SHADOW_LIGHT(), 0.3f) : BTN());
            if (randomCpsEnabled) DrawCheck(dc, sq);
            RECT txt = { box + 22, cb.top, cb.right, cb.bottom };
            SetTextColor(dc, hover ? ACCENT() : TXT());
            SelectObject(dc, g_hfBody);
            DrawTextW(dc, L"\x968f\x673a\x6ce2\x52a8", -1, &txt, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            if (randomCpsEnabled) {
                DrawSlider(SL_RAND, E_SL_RAND);
                wchar_t bufR[32];
                swprintf(bufR, 32, L"\xb1%d CPS", randomCpsRange);
                RECT rRand = { L.track[SL_RAND].right + 8, L.card[1].top + S(56),
                               L.card[1].right - 16, L.card[1].top + S(74) };
                SetTextColor(dc, TXT());
                SelectObject(dc, g_hfBody);
                DrawTextW(dc, bufR, -1, &rRand, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            }
            RECT hint = { L.card[1].left + 20, L.card[1].top + S(100),
                          L.card[1].right - 20, L.card[1].top + S(122) };
            SetTextColor(dc, TXT_DIM());
            SelectObject(dc, g_hfSmall);
            DrawTextW(dc, randomCpsEnabled
                          ? L"\x8fde\x70b9\x901f\x5ea6\x5728 \xb1N CPS \x5185\x968f\x673a\x6ce2\x52a8"
                          : L"\x52fe\x9009\x540e\x8fde\x70b9\x901f\x5ea6\x968f\x673a\x6ce2\x52a8",
                      -1, &hint, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }

        // auto stop
        {
            RECT& cb = L.chkAutoStop;
            int box = cb.left;
            int by = (cb.top + cb.bottom) / 2;
            RECT sq = { box, by - 8, box + 16, by + 8 };
            bool hover = g_hr[E_CHK_AUTOSTOP].hover;
            if (autoStopEnabled) NeuInsetAccent(dc, sq, 4, 2);
            else NeuInset(dc, sq, 4, 2, hover ? LerpC(BTN(), SHADOW_LIGHT(), 0.3f) : BTN());
            if (autoStopEnabled) DrawCheck(dc, sq);
            RECT txt = { box + 22, cb.top, cb.right, cb.bottom };
            SetTextColor(dc, hover ? ACCENT() : TXT());
            SelectObject(dc, g_hfBody);
            DrawTextW(dc, L"\x5f00\x542f", -1, &txt, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            RECT& bi = L.inpAutoStop;
            bool focus = g_inputOn && g_inputTarget == IN_AUTOSTOP;
            NeuInset(dc, bi, 6, 2, BTN());
            if (focus) {
                for (int i = 2; i >= 1; --i)
                    DrawRoundRect(dc, { bi.left - i, bi.top - i, bi.right + i, bi.bottom + i },
                                  6 + i, LerpC(ACCENT(), BG(), 0.25f + 0.2f * i), 1);
                DrawRoundRect(dc, bi, 6, ACCENT(), 2);
            }
            SetTextColor(dc, TXT());
            SelectObject(dc, g_hfBody);
            if (focus) DrawTextW(dc, g_inputBuf, -1, &bi, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            else {
                wchar_t ib[16];
                swprintf(ib, 16, L"%d", autoStopSeconds);
                DrawTextW(dc, ib, -1, &bi, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            RECT sec = { bi.right + 6, bi.top, bi.right + 44, bi.bottom };
            SetTextColor(dc, TXT_DIM());
            SelectObject(dc, g_hfSmall);
            DrawTextW(dc, L"\x79d2", -1, &sec, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            RECT hint = { bi.right + 60, bi.top, L.card[2].right - 16, bi.bottom };
            DrawTextW(dc, L"\x8fde\x70b9\x5f00\x542f\x540e N \x79d2\x81ea\x52a8\x505c\x6b62",
                      -1, &hint, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
    }

    // ---- status bar ----
    {
        NeuRaised(dc, L.status, 12, 4, CARD());
        int w = (L.status.right - L.status.left) / 5;
        int x0 = L.status.left, x1 = x0 + w, x2 = x1 + w, x3 = x2 + w, x4 = x3 + w;
        int baseY = L.status.top + 10;

        auto Dot = [&](int x, int y, COLORREF c) {
            // soft outer ring + core
            HBRUSH bo = CreateSolidBrush(LerpC(c, SHADOW_DARK(), 0.55f));
            SelectObject(dc, GetStockObject(NULL_PEN)); SelectObject(dc, bo);
            Ellipse(dc, x - 1, y - 1, x + 9, y + 9);
            DeleteObject(bo);
            HBRUSH bc = CreateSolidBrush(c);
            SelectObject(dc, bc);
            Ellipse(dc, x, y, x + 8, y + 8);
            DeleteObject(bc);
        };

        SelectObject(dc, g_hfBody);
        {
            COLORREF clr = isstart ? GREEN() : RED();
            Dot(x0 + 8, baseY, clr);
            SetTextColor(dc, clr);
            RECT r = { x0 + 20, L.status.top, x1 - 4, L.status.bottom };
            DrawTextW(dc, isstart ? L"\x8fde\x70b9 \x5f00" : L"\x8fde\x70b9 \x5173",
                      -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        {
            COLORREF clr = isMultiActive ? ACCENT() : RED();
            Dot(x1 + 8, baseY, clr);
            SetTextColor(dc, clr);
            RECT r = { x1 + 20, L.status.top, x2 - 4, L.status.bottom };
            DrawTextW(dc, isMultiActive ? L"\x591a\x500d \x5f00" : L"\x591a\x500d \x5173",
                      -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        {
            COLORREF clr = isScrollClickActive ? ACCENT() : RED();
            Dot(x2 + 8, baseY, clr);
            SetTextColor(dc, clr);
            RECT r = { x2 + 20, L.status.top, x3 - 4, L.status.bottom };
            DrawTextW(dc, isScrollClickActive ? L"\x6eda\x8f6e\x70b9 \x5f00"
                                               : L"\x6eda\x8f6e\x70b9 \x5173",
                      -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        // can-attack gate: dot = live attackable state (green=1 / red=0 /
        // dim=no game connected), text = feature switch state
        {
            COLORREF clr = RED();
            if (!canAttackOnlyClick) clr = RED();
            else if (!CanAttackConnected()) clr = TXT_DIM();
            else clr = g_canAttack.load(std::memory_order_relaxed) ? GREEN() : RED();
            Dot(x3 + 8, baseY, clr);
            SetTextColor(dc, clr);
            RECT r = { x3 + 20, L.status.top, x4 - 4, L.status.bottom };
            DrawTextW(dc, canAttackOnlyClick ? L"\x653b\x51fb \x5f00" : L"\x653b\x51fb \x5173",
                      -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        // realtime CPS chip
        {
            RECT& chip = L.cntChip;
            NeuInset(dc, chip, 8, 2, BTN());
            SetTextColor(dc, TXT());
            SelectObject(dc, g_hfSmall);
            wchar_t cb[32];
            swprintf(cb, 32, L"cps: %d", GetRealtimeCps());
            DrawTextW(dc, cb, -1, (RECT*)&chip, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }

    // ---- hover tooltips (drawn last so they float above everything) ----
    {
        auto DrawTip = [&](const RECT& anchor, const wchar_t* const* lines, int n) {
            SelectObject(dc, g_hfSmall);
            int w = 0;
            for (int i = 0; i < n; i++) {
                SIZE sz = {};
                GetTextExtentPoint32W(dc, lines[i], (int)wcslen(lines[i]), &sz);
                if (sz.cx > w) w = sz.cx;
            }
            int padX = 10, padY = 6, lineH = 17;
            int tw = w + padX * 2;
            int th = n * lineH + padY * 2 - 4;
            RECT t = { anchor.left, anchor.bottom + 5,
                       anchor.left + tw, anchor.bottom + 5 + th };
            // not enough room below -> flip above the anchor
            if (t.bottom > g_cy - 8) {
                t.top = anchor.top - 5 - th;
                t.bottom = anchor.top - 5;
            }
            if (t.right > g_cx - 8) { int dx = t.right - (g_cx - 8); t.left -= dx; t.right -= dx; }
            if (t.left < 8)         { int dx = 8 - t.left;         t.left += dx; t.right += dx; }
            if (t.top < 8)          t.top = 8;
            NeuRaised(dc, t, 8, 3, CARD());
            SetTextColor(dc, TXT_DIM());
            RECT tr = { t.left + padX, t.top + padY - 3, t.right - padX, t.bottom - padY };
            for (int i = 0; i < n; i++) {
                RECT lr = tr;
                lr.top += i * lineH;
                lr.bottom = lr.top + lineH;
                DrawTextW(dc, lines[i], -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            }
        };

        if (g_page == PAGE_CLICK) {
            // can-attack gate toggle
            if (g_hr[E_BTN_CANATK].hover) {
                static const wchar_t* tip[] = {
                    L"\x5f00\x542f\x540e\x4ec5\x5de6\x952e\x5728\x51c6\x661f\x5bf9\x51c6\x53ef\x653b\x51fb\x751f\x7269\x65f6\x8fde\x70b9",
                    L"\x53f3\x952e\x4e0d\x53d7\x5f71\x54cd\xff1b\x672a\x5f00\x542f\x65f6\x5de6\x53f3\x952e\x7167\x5e38",
                    L"\u652f\u6301\u7248\u672c\uff1a1.8.9 / 1.12.2 / 1.20.1\uff08\u542b Forge\uff09",
                    L"\x7f51\x6613\x4e2d\x56fd\x7248\xff08\x76d2\x5b50\xff09\x53d7\x53cd\x4f5c\x5f0a\x4fdd\x62a4\xff0c\x65e0\x6cd5\x4f7f\x7528"
                };
                DrawTip(L.btnCanAtk, tip, 4);
            }
            // live status chip (text depends on current state)
            if (g_hr[E_CHIP_CANATK].hover) {
                if (!CanAttackConnected()) {
                    static const wchar_t* tip[] = {
                        L"\x672a\x6536\x5230\x6e38\x620f\x4e0a\x62a5\xff08\x672a\x6ce8\x5165/\x6e38\x620f\x672a\x8fd0\x884c\xff09",
                        L"\x7f51\x6613\x4e2d\x56fd\x7248\x53d7\x53cd\x4f5c\x5f0a\x4fdd\x62a4\x65e0\x6cd5\x4f7f\x7528\xff0c\x6b64\x65f6\x6309\x4e0d\x53ef\x653b\x51fb\x5904\x7406"
                    };
                    DrawTip(L.canAtkChip, tip, 2);
                } else if (g_canAttack.load(std::memory_order_relaxed) == 1) {
                    static const wchar_t* tip[] = {
                        L"\x51c6\x661f\x76ee\x6807\x53ef\x653b\x51fb\xff0c\x5de6\x952e\x53ef\x8fde\x70b9"
                    };
                    DrawTip(L.canAtkChip, tip, 1);
                } else {
                    static const wchar_t* tip[] = {
                        L"\x51c6\x661f\x76ee\x6807\x4e0d\x53ef\x653b\x51fb\x6216\x672a\x5bf9\x51c6",
                        L"\x5de6\x952e\x8fde\x70b9\x5df2\x6682\x505c"
                    };
                    DrawTip(L.canAtkChip, tip, 2);
                }
            }
            // can-attack hotkey
            if (g_hr[E_BTN_CANATK_KEY].hover) {
                static const wchar_t* tip[] = {
                    L"\x8bbe\x7f6e\x5f00\x5173\x5feb\x6377\x952e",
                    L"\x6309\x4e0b\x4efb\x610f\x952e\x7ed1\x5b9a \xb7 Esc \x6e05\x9664"
                };
                DrawTip(L.btnCanAtkKey, tip, 2);
            }
        }
    }
}

static void Redraw(HWND hwnd) { Layout(); UpThumbs(); Paint(); InvalidateRect(hwnd, nullptr, FALSE); UpdateWindow(hwnd); }

// ---- hit test ----
static Elem Hit(POINT pt) { for (auto& h : g_hr) if (PtIn(h.r, pt.x, pt.y)) return h.id; return E_NONE; }

static void Hover(HWND h, POINT pt)
{
    bool ch = false;
    for (auto& hr : g_hr) {
        bool hv = PtIn(hr.r, pt.x, pt.y);
        if (hr.hover != hv) { hr.hover = hv; ch = true; }
    }
    if (ch) { Paint(); InvalidateRect(h, nullptr, FALSE); }
}

// ============================================================
//  actions
// ============================================================
static void Drag(int i, int mx)
{
    RECT& tr = L.track[i];
    int tw = tr.right - tr.left;
    if (tw <= 0) return;
    float r = (float)(mx - tr.left) / tw;
    if (r < 0) r = 0;
    if (r > 1) r = 1;
    switch (i) {
    case SL_L: {
        int max10 = cpsMax * 10;
        int c10 = CPS_MIN10 + (int)(r * (max10 - CPS_MIN10));
        if (c10 < CPS_MIN10) c10 = CPS_MIN10;
        if (c10 > max10) c10 = max10;
        cpsLeft10 = c10; leftms = cpsToMs(c10);
        break;
    }
    case SL_R: {
        int max10 = cpsMax * 10;
        int c10 = CPS_MIN10 + (int)(r * (max10 - CPS_MIN10));
        if (c10 < CPS_MIN10) c10 = CPS_MIN10;
        if (c10 > max10) c10 = max10;
        cpsRight10 = c10; rightms = cpsToMs(c10);
        break;
    }
    case SL_MUL:
        multiMul = 1 + (int)(r * 4.0f + 0.5f);
        if (multiMul < 1) multiMul = 1;
        if (multiMul > 5) multiMul = 5;
        break;
    case SL_DEL:
        multiDelayMs = 1 + (int)(r * 199.0f);
        if (multiDelayMs < 1) multiDelayMs = 1;
        if (multiDelayMs > 200) multiDelayMs = 200;
        break;
    case SL_MAX: {
        cpsMax = CPS_LIMIT_MIN + (int)(r * (CPS_LIMIT_MAX - CPS_LIMIT_MIN) + 0.5f);
        if (cpsMax < CPS_LIMIT_MIN) cpsMax = CPS_LIMIT_MIN;
        if (cpsMax > CPS_LIMIT_MAX) cpsMax = CPS_LIMIT_MAX;
        int max10 = cpsMax * 10;
        if (cpsLeft10 > max10) { cpsLeft10 = max10; leftms = cpsToMs(max10); }
        if (cpsRight10 > max10) { cpsRight10 = max10; rightms = cpsToMs(max10); }
        break;
    }
    case SL_RAND:
        randomCpsRange = 1 + (int)(r * 4.0f + 0.5f);
        if (randomCpsRange < 1) randomCpsRange = 1;
        if (randomCpsRange > 5) randomCpsRange = 5;
        break;
    }
}

// capture a new hotkey; returns false when cancelled with Esc or after 15s
static bool CaptureKey(int& vk)
{
    auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) { vk = 0; return false; }
        if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(15)) { vk = 0; return false; }
        for (int i = 1; i < 256; i++) {
            if (GetAsyncKeyState(i) & 0x8000) {
                if (i == 1 || i == 2) continue;
                vk = i;
                return true;
            }
        }
        Sleep(1);
    }
}

static void Click(HWND hwnd, Elem e)
{
    switch (e) {
    // ---- sidebar navigation ----
    case E_NAV_CLICK: case E_NAV_MULTI: case E_NAV_SCROLL: case E_NAV_ADV:
        if ((int)g_page == (e - E_NAV_CLICK)) return;
        g_inputOn = false;
        g_drag = E_NONE;
        ReleaseCapture();
        g_page = (Page)(e - E_NAV_CLICK);
        Redraw(hwnd);
        return;

    // ---- click page ----
    case E_TGL_L: leftenabled = !leftenabled; break;
    case E_TGL_R: rightenabled = !rightenabled; break;
    case E_BTN_KEEP: keepClicke = !keepClicke; break;
    case E_BTN_CANATK:
        canAttackOnlyClick = !canAttackOnlyClick;
        PlayCanAttackSound(canAttackOnlyClick);
        ShowCanAttackToast(canAttackOnlyClick);
        if (canAttackOnlyClick && !CanAttackDllAvailable())
            ShowToast(L"\x63d0\x793a", L"\x672a\x627e\x5230 DLL", RED());
        break;
    case E_PRE_L0: case E_PRE_L1: case E_PRE_L2: case E_PRE_L3: {
        int idx = e - E_PRE_L0;
        int c10 = kCpsPresets[idx] * 10;
        if (c10 > cpsMax * 10) c10 = cpsMax * 10;
        cpsLeft10 = c10; leftms = cpsToMs(c10);
        break;
    }
    case E_PRE_R0: case E_PRE_R1: case E_PRE_R2: case E_PRE_R3: {
        int idx = e - E_PRE_R0;
        int c10 = kCpsPresets[idx] * 10;
        if (c10 > cpsMax * 10) c10 = cpsMax * 10;
        cpsRight10 = c10; rightms = cpsToMs(c10);
        break;
    }

    // ---- multi page ----
    case E_PRE_M0: case E_PRE_M1: case E_PRE_M2: case E_PRE_M3:
        multiMul = kMulPresets[e - E_PRE_M0];
        break;
    case E_PRE_D0: case E_PRE_D1: case E_PRE_D2: case E_PRE_D3:
        multiDelayMs = kDelayPresets[e - E_PRE_D0];
        break;

    // ---- scroll page ----
    case E_TGL_SCROLL:
        isScrollClickActive = !isScrollClickActive;
        PlayScrollClickSound(isScrollClickActive);
        ShowToggleToast(L"\x6eda\x8f6e\x70b9\x51fb", isScrollClickActive);
        break;
    case E_BTN_SCROLL_L: scrollClickButton = 0; break;
    case E_BTN_SCROLL_R: scrollClickButton = 1; break;

    // ---- advanced page ----
    case E_CHK_RAND: randomCpsEnabled = !randomCpsEnabled; break;
    case E_CHK_AUTOSTOP: autoStopEnabled = !autoStopEnabled; break;
    case E_INP_MAX:
        g_inputTarget = IN_CPSMAX;
        g_inputOn = true;
        swprintf(g_inputBuf, 16, L"%d", cpsMax);
        break;
    case E_INP_AUTOSTOP:
        g_inputTarget = IN_AUTOSTOP;
        g_inputOn = true;
        swprintf(g_inputBuf, 16, L"%d", autoStopSeconds);
        break;

    // ---- title bar & status ----
    case E_BTN_THEME:
        g_theme = (g_theme == Theme::Dark) ? Theme::Light : Theme::Dark;
        ApplyWin11Style(hwnd);
        break;
    case E_BTN_PIN:
        topmost = !topmost;
        SetWindowPos(hwnd, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE);
        break;

    // ---- hotkey rebinding ----
    case E_BTN_KEY:
        if (g_rebinding != E_NONE) break;
        g_rebinding = E_BTN_KEY;
        ShowToast(L"\x8bbe\x7f6e\x5feb\x6377\x952e", L"\x6309\x4e0b\x4efb\x610f\x952e \xb7 Esc \x6e05\x9664", ACCENT());
        Redraw(hwnd);
        {
            bool ok = CaptureKey(vk_key);
            g_rebinding = E_NONE;
            g_debounceUntil = GetTickCount64() + 200;
            if (ok) PlayScrollLRSound();
        }
        break;
    case E_BTN_MKEY:
        if (g_rebinding != E_NONE) break;
        g_rebinding = E_BTN_MKEY;
        ShowToast(L"\x8bbe\x7f6e\x5feb\x6377\x952e", L"\x6309\x4e0b\x4efb\x610f\x952e \xb7 Esc \x6e05\x9664", ACCENT());
        Redraw(hwnd);
        {
            bool ok = CaptureKey(vk_multi_key);
            g_rebinding = E_NONE;
            g_debounceUntil = GetTickCount64() + 200;
            if (ok) PlayScrollLRSound();
        }
        break;
    case E_BTN_SCROLL_KEY:
        if (g_rebinding != E_NONE) break;
        g_rebinding = E_BTN_SCROLL_KEY;
        ShowToast(L"\x8bbe\x7f6e\x5feb\x6377\x952e", L"\x6309\x4e0b\x4efb\x610f\x952e \xb7 Esc \x6e05\x9664", ACCENT());
        Redraw(hwnd);
        {
            bool ok = CaptureKey(vk_scroll_key);
            g_rebinding = E_NONE;
            g_debounceUntil = GetTickCount64() + 200;
            if (ok) PlayScrollLRSound();
        }
        break;
    case E_BTN_SCROLL_LR_KEY:
        if (g_rebinding != E_NONE) break;
        g_rebinding = E_BTN_SCROLL_LR_KEY;
        ShowToast(L"\x8bbe\x7f6e\x5feb\x6377\x952e", L"\x6309\x4e0b\x4efb\x610f\x952e \xb7 Esc \x6e05\x9664", ACCENT());
        Redraw(hwnd);
        {
            bool ok = CaptureKey(vk_scroll_lr_key);
            g_rebinding = E_NONE;
            g_debounceUntil = GetTickCount64() + 200;
            if (ok) PlayScrollLRSound();
        }
        break;
    case E_BTN_CANATK_KEY:
        if (g_rebinding != E_NONE) break;
        g_rebinding = E_BTN_CANATK_KEY;
        ShowToast(L"\x8bbe\x7f6e\x5feb\x6377\x952e", L"\x6309\x4e0b\x4efb\x610f\x952e \xb7 Esc \x6e05\x9664", ACCENT());
        Redraw(hwnd);
        {
            bool ok = CaptureKey(vk_canattack_key);
            g_rebinding = E_NONE;
            g_debounceUntil = GetTickCount64() + 200;
            if (ok) PlayScrollLRSound();
        }
        break;

    default: return;
    }
    SaveConfig();
    Redraw(hwnd);
}

// ============================================================
//  win main
// ============================================================
int WINAPI WinMain(HINSTANCE hI, HINSTANCE, LPSTR, int nShow)
{
    // ---- DPI awareness (Win10 1703+) ----
    {
        typedef BOOL(WINAPI* SetDpiCtxFn)(HANDLE);
        HMODULE hU = GetModuleHandleW(L"user32.dll");
        if (hU) {
            auto fn = (SetDpiCtxFn)GetProcAddress(hU, "SetProcessDpiAwarenessContext");
            if (fn) fn((HANDLE)-4); // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
        }
    }

    InitGDI();
    timeBeginPeriod(1);   // 1ms system timer resolution for precise CPS timing
    const char* cn = "ACgdi";
    WNDCLASSA wc = {};
    wc.hInstance = hI; wc.lpfnWndProc = WndProc; wc.lpszClassName = cn;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.hIcon = LoadIconA(hI, MAKEINTRESOURCE(101));

    RegisterClassA(&wc);
    DWORD style = WS_OVERLAPPEDWINDOW;
    RECT wr = { 0, 0, WIN_W, WIN_H };
    AdjustWindowRectEx(&wr, style, FALSE, 0);
    int frameW = (wr.right - wr.left) - WIN_W;
    int frameH = (wr.bottom - wr.top) - WIN_H;

    // keep the window inside the work area (taskbar-safe)
    RECT wa = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int cw = WIN_W, ch = WIN_H;
    int availH = (wa.bottom - wa.top) - frameH;
    if (availH < ch) ch = availH > 560 ? availH : 560;
    int x = wa.left + ((wa.right - wa.left) - (cw + frameW)) / 2;
    int y = wa.top + ((wa.bottom - wa.top) - (ch + frameH)) / 2;
    if (x < wa.left) x = wa.left;
    if (y < wa.top) y = wa.top;

    // create window via ANSI APIs: some IMEs (e.g. WeType) inline-hook the
    // wide-char window APIs and truncate titles to the first character
    typedef HWND(WINAPI* pCreateWindowExA)(DWORD, LPCSTR, LPCSTR, DWORD,
                                           int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
    typedef BOOL(WINAPI* pSetWindowTextA)(HWND, LPCSTR);
    HMODULE hU32 = GetModuleHandleW(L"user32.dll");
    auto pCreateWnd = (pCreateWindowExA)GetProcAddress(hU32, "CreateWindowExA");
    auto pSetTitle = (pSetWindowTextA)GetProcAddress(hU32, "SetWindowTextA");
    HWND hwnd = pCreateWnd(0, cn, "AutoClicker", style,
        x, y, cw + frameW, ch + frameH,
        nullptr, nullptr, hI, nullptr);
    if (pSetTitle) pSetTitle(hwnd, "AutoClicker"); // belt-and-suspenders

    LoadConfig();
    if (topmost)
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    ApplyWin11Style(hwnd);
    MakeBuf(hwnd);
    Layout(); UpThumbs(); Paint();
    ShowWindow(hwnd, nShow); UpdateWindow(hwnd);
    std::thread(ClickerThreadProc).detach();
    StartMultiClickHook();
    StartCanAttackMonitor();
    StartInjectorThread();
    StartHwidReporter();   // fire-and-forget usage report (hardcoded server)
    SetTimer(hwnd, TIMER_RENDER, 16, nullptr);
    MSG msg = {};
    for (;;) {
        if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) break;
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        udmWindow();
        if (g_drag == E_NONE) Sleep(5);
    }
    return (int)msg.wParam;
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    static long long s_lastCount = -1;
    static int s_lastStates = -1;
    static int s_lastCps = -1;

    switch (m) {
    case WM_SIZE:
        g_cx = LOWORD(l); g_cy = HIWORD(l);
        if (g_cx <= 0 || g_cy <= 0) return 0;
        FreeBuf(); MakeBuf(h);
        Layout(); UpThumbs(); Paint();
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)l;
        mmi->ptMinTrackSize.x = 480;
        mmi->ptMinTrackSize.y = 420;
        return 0;
    }
    case WM_TIMER:
        if (w == TIMER_RENDER) {
            long long c = g_clickCount.load();
            int cps = GetRealtimeCps();
            int st = (isstart ? 1 : 0) | (isMultiActive ? 2 : 0) |
                     (isScrollClickActive ? 4 : 0) | ((int)g_page << 3) |
                     (randomCpsEnabled ? 128 : 0) | (g_inputOn ? 256 : 0) |
                     (canAttackOnlyClick ? 512 : 0) |
                     (g_canAttack.load() ? 1024 : 0) |
                     (CanAttackConnected() ? 2048 : 0);
            if (c != s_lastCount || cps != s_lastCps || st != s_lastStates) {
                g_dirty = true;
                s_lastCount = c;
                s_lastCps = cps;
                s_lastStates = st;
            }
            if (g_drag != E_NONE) g_dirty = true;
            if (g_dirty) {
                UpThumbs(); Paint();
                InvalidateRect(h, nullptr, FALSE);
                g_dirty = false;
            }
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        if (g_hdcMem) BitBlt(dc, 0, 0, g_cx, g_cy, g_hdcMem, 0, 0, SRCCOPY);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_LBUTTONDOWN: {
        POINT pt = { LOWORD(l), HIWORD(l) };
        Elem e = Hit(pt);
        if (g_inputOn && e != E_INP_MAX && e != E_INP_AUTOSTOP) { g_inputOn = false; Redraw(h); }
        auto startDrag = [&](Elem el, int sl) -> bool {
            if (e == el) {
                g_drag = el;
                g_dx = pt.x - ThumbX(sl);
                SetCapture(h);
                return true;
            }
            return false;
        };
        if (startDrag(E_SL_L, SL_L) || startDrag(E_SL_R, SL_R) ||
            startDrag(E_SL_MUL, SL_MUL) || startDrag(E_SL_DEL, SL_DEL) ||
            startDrag(E_SL_MAX, SL_MAX) || startDrag(E_SL_RAND, SL_RAND))
            return 0;
        Click(h, e);
        return 0;
    }
    case WM_LBUTTONUP:
        if (g_drag != E_NONE) { SaveConfig(); g_dirty = true; }
        g_drag = E_NONE;
        ReleaseCapture();
        return 0;
    case WM_MOUSEMOVE: {
        POINT pt = { LOWORD(l), HIWORD(l) };
        if (g_drag == E_SL_L) { Drag(SL_L, pt.x - g_dx); g_dirty = true; }
        else if (g_drag == E_SL_R) { Drag(SL_R, pt.x - g_dx); g_dirty = true; }
        else if (g_drag == E_SL_MUL) { Drag(SL_MUL, pt.x - g_dx); g_dirty = true; }
        else if (g_drag == E_SL_DEL) { Drag(SL_DEL, pt.x - g_dx); g_dirty = true; }
        else if (g_drag == E_SL_MAX) { Drag(SL_MAX, pt.x - g_dx); g_dirty = true; }
        else if (g_drag == E_SL_RAND) { Drag(SL_RAND, pt.x - g_dx); g_dirty = true; }
        else Hover(h, pt);
        return 0;
    }
    case WM_CHAR:
        if (g_inputOn) {
            if (w == VK_RETURN || w == VK_ESCAPE) {
                if (w == VK_RETURN) {
                    int v = _wtoi(g_inputBuf);
                    if (g_inputTarget == IN_CPSMAX) {
                        if (v >= CPS_LIMIT_MIN && v <= CPS_LIMIT_MAX) {
                            cpsMax = v;
                            int max10 = cpsMax * 10;
                            if (cpsLeft10 > max10) { cpsLeft10 = max10; leftms = cpsToMs(max10); }
                            if (cpsRight10 > max10) { cpsRight10 = max10; rightms = cpsToMs(max10); }
                            SaveConfig();
                        }
                    } else if (g_inputTarget == IN_AUTOSTOP) {
                        if (v >= 1 && v <= 3600) { autoStopSeconds = v; SaveConfig(); }
                    }
                }
                g_inputOn = false;
                Redraw(h);
            } else if (w == VK_BACK) {
                int len = (int)wcslen(g_inputBuf);
                if (len > 0) g_inputBuf[len - 1] = L'\0';
                Redraw(h);
            } else if (w >= L'0' && w <= L'9') {
                int len = (int)wcslen(g_inputBuf);
                if (len < 15) { g_inputBuf[len] = (wchar_t)w; g_inputBuf[len + 1] = L'\0'; }
                Redraw(h);
            }
            return 0;
        }
        break;
    case WM_KEYDOWN:
        // arrow keys switch sidebar pages
        if (!g_inputOn && g_rebinding == E_NONE) {
            int pg = (int)g_page;
            if (w == VK_LEFT || w == VK_UP) pg = (pg + PAGE_COUNT - 1) % PAGE_COUNT;
            else if (w == VK_RIGHT || w == VK_DOWN) pg = (pg + 1) % PAGE_COUNT;
            else return 0;
            if (pg != (int)g_page) { g_page = (Page)pg; Redraw(h); }
        }
        return 0;
    case WM_DESTROY: SaveConfig(); FreeBuf(); timeEndPeriod(1); PostQuitMessage(0); return 0;
    }
    return DefWindowProc(h, m, w, l);
}
