#include "types.h"
#include "ui.h"
#include "glass.h"
#include "clicker.h"
#include "config.h"
#include "overlay.h"
#include "sound.h"
#include "canattack.h"
#ifdef AUTOCLICKER_NET
#include "report.h"
#include "update.h"
#endif

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

// ---- 玻璃态分层渲染 (glassmorphism layered rendering) ----
// g_chrome    全窗层: 基底玻璃 + 标题栏/方案芯片/图钉/主题/侧栏/状态栏底板
// g_chromeDyn 全窗层: 状态栏动态内容 (状态点/文字/迷你曲线, 10Hz 刷新)
// g_content   全窗层: 当前页内容 (页切换动画只移动这一层)
// g_surface   合成层: Present = 清空 + 依次混合三层 + UpdateLayeredWindow
static GLayer g_chrome, g_chromeDyn, g_content, g_surface;
static bool   g_layersOk = false;
static HFONT  g_hfTitle = nullptr;
static HFONT  g_hfLabel = nullptr;
static HFONT  g_hfBody  = nullptr;
static HFONT  g_hfSmall = nullptr;
static int    g_cx = WIN_W, g_cy = WIN_H;
static float  g_lyScale = 1.0f;   // responsive layout scale
static bool   g_dirty = true;     // repaint-on-demand
static bool   g_needChrome = true, g_needChromeDyn = true, g_needContent = true;

// UI 主窗口句柄 (连点线程热键切换方案后向它发 WM_APP_PROFILE)
HWND g_uiHwnd = nullptr;

// ---- pages (sidebar) ----
enum Page { PAGE_CLICK = 0, PAGE_MULTI, PAGE_SCROLL, PAGE_DASH, PAGE_ADV, PAGE_COUNT };
static Page g_page = PAGE_CLICK;

// ---- slider ids ----
enum SliderId { SL_L = 0, SL_R, SL_MUL, SL_DEL, SL_MAX, SL_RAND, SL_HUM, SL_COUNT };

// ---- interactive elements ----
enum Elem {
    E_NONE = -1,
    // sidebar navigation
    E_NAV_CLICK, E_NAV_MULTI, E_NAV_SCROLL, E_NAV_DASH, E_NAV_ADV,
    // click page
    E_TGL_L, E_TGL_R, E_SL_L, E_SL_R, E_BTN_KEY, E_BTN_KEEP,
    E_BTN_CANATK, E_CHIP_CANATK, E_BTN_CANATK_KEY,
    E_BTN_PLACE, E_CHIP_PLACE, E_BTN_PLACE_KEY,
    E_PRE_L0, E_PRE_L1, E_PRE_L2, E_PRE_L3, E_PRE_L4, E_PRE_L5,
    E_PRE_R0, E_PRE_R1, E_PRE_R2, E_PRE_R3, E_PRE_R4, E_PRE_R5,
    // multi page
    E_SL_MUL, E_SL_DEL, E_BTN_MKEY,
    E_PRE_M0, E_PRE_M1, E_PRE_M2, E_PRE_M3,
    E_PRE_D0, E_PRE_D1, E_PRE_D2, E_PRE_D3,
    // scroll page
    E_TGL_SCROLL, E_BTN_SCROLL_KEY, E_BTN_SCROLL_L, E_BTN_SCROLL_R, E_BTN_SCROLL_LR_KEY,
    // dashboard page
    E_ACC0, E_ACC1, E_ACC2, E_ACC3, E_INP_PROFILE, E_BTN_RENAME,
    // advanced page
    E_SL_MAX, E_INP_MAX, E_CHK_RAND, E_SL_RAND, E_CHK_AUTOSTOP, E_INP_AUTOSTOP,
    E_HM0, E_HM1, E_HM2, E_HM3, E_SL_HUM,
    // title bar & status
    E_PRF0, E_PRF1, E_PRF2, E_PRF3,
    E_CHIP_TARGET,
    E_BTN_MIN, E_BTN_MAX, E_BTN_CLOSE,
    E_BTN_THEME, E_BTN_PIN, E_BTN_SOUND, E_BTN_CURSOR,
    E_COUNT
};
struct HR { RECT r; Elem id; bool hover; };
static HR   g_hr[E_COUNT] = {};
static Elem g_drag = E_NONE;
static int  g_dx = 0;

// ---- text input state ----
enum InputTarget { IN_NONE = 0, IN_CPSMAX, IN_AUTOSTOP, IN_PROFILE };
static bool        g_inputOn = false;
static int         g_inputTarget = IN_NONE;
static wchar_t     g_inputBuf[64] = {};

// ---- key capture state (rebinding feedback) ----
static Elem g_rebinding = E_NONE;

// ---- page transition animation ----
static bool       g_animActive = false;
static int        g_animDir = 0;
static ULONGLONG  g_animStartMs = 0;
static float      g_animT = 0.0f;
static constexpr ULONGLONG kAnimMs = 160;

// ---- realtime CPS sparkline (sampled every 100ms in WM_TIMER) ----
static constexpr int kSparkN = 120;          // ~12s 窗口
static int      s_spark[kSparkN] = {};
static int      s_sparkCount = 0;            // 已采样总数 (可 > kSparkN)
static DWORD    s_sparkLastMs = 0;
static ULONGLONG s_sessionStartMs = 0;       // 会话开始 (面板显示用时)

// ---- preset tables ----
static const int kCpsPresets[6]   = { 6, 10, 15, 20, 30, 40 };    // CPS
static const int kMulPresets[4]   = { 2, 3, 4, 5 };       // x倍
static const int kDelayPresets[4] = { 10, 25, 50, 100 };  // ms

// ---- layout rects ----
struct LY {
    RECT title, sidebar, nav[5], card[3], status;
    RECT btnPin, btnTheme, btnSound, btnCursor, btnMin, btnMax, btnClose;
    RECT prfChip[4];                 // 标题栏方案芯片
    RECT track[SL_COUNT], thumb[SL_COUNT];
    RECT tglL, tglR, tglScroll;
    RECT btnKey, btnKeep, btnMKey;
    RECT btnCanAtk, canAtkChip, btnCanAtkKey;
    RECT btnPlace, placeChip, btnPlaceKey;
    RECT preL[6], preR[6], preM[4], preD[4];
    RECT btnScrollKey, btnScrollLR, btnScrollLRKey;
    RECT inpMax, chkRand, chkAutoStop, inpAutoStop;
    RECT hm[4], humLevelLbl;         // 拟人化模式芯片 + 强度标签
    RECT dashPlot, dashCps, dashStats;   // 面板页: 曲线区 / 当前CPS / 统计行
    RECT dashDiv, dashRow[4], inpProfile, btnRename, accDot[4], accLbl, renameLbl;
    RECT stSlot[6];                  // 状态栏 6 槽 (连点/辅助/攻击/放置/目标/CPS)
} L;

static bool PtIn(const RECT& r, int x, int y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

// responsive layout scale helper (usable anywhere)
static int S(int v) { return (int)(v * g_lyScale); }

// 页面切换动画的当前水平偏移 (新页面从侧面滑入, ease-out)
static int AnimOffsetX()
{
    if (!g_animActive) return 0;
    float t = g_animT;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    float ease = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
    return (int)(g_animDir * (float)g_cx * 0.38f * (1.0f - ease));
}

// ============================================================
//  layout
// ============================================================
static void Layout()
{
    int W = g_cx, H = g_cy;

    // ---- title bar ----
    L.title   = { 0, 6, W, 46 };
    // 右簇: [方案芯片] 光标 提示音 置顶 主题 最小化 最大化 关闭 (各 28px, 8px 间距)
    L.btnClose  = { W - 44, 12, W - 16, 40 };
    L.btnMax    = { W - 80, 12, W - 52, 40 };
    L.btnMin    = { W - 116, 12, W - 88, 40 };
    L.btnTheme  = { W - 152, 12, W - 124, 40 };
    L.btnPin    = { W - 188, 12, W - 160, 40 };
    L.btnSound  = { W - 224, 12, W - 196, 40 };
    L.btnCursor = { W - 260, 12, W - 232, 40 };
    // 方案芯片: 排布在光标按钮左侧 (紧凑胶囊; 长名自动省略号)
    {
        int cw = 40, gap = 4;
        int x = L.btnCursor.left - 10 - (4 * cw + 3 * gap);
        for (int i = 0; i < 4; i++) {
            L.prfChip[i] = { x, 11, x + cw, 37 };
            x += cw + gap;
        }
    }

    // ---- sidebar ----
    L.sidebar = { 12, 58, 80, H - 64 };
    int navY = L.sidebar.top + 14;
    int navGap = (L.sidebar.bottom - L.sidebar.top - 5 * 52 - 28) / 4;
    if (navGap < 8) navGap = 8;
    for (int i = 0; i < 5; i++) {
        L.nav[i] = { 14, navY, 78, navY + 52 };
        navY += 52 + navGap;
    }

    // ---- content cards: row1 (two side-by-side) + row2 (full width) ----
    int top = 58, gap = 14;
    int bh1 = 165, bh2 = 135;   // base row heights
    int nRows = 2;
    if (g_page == PAGE_SCROLL) { bh1 = 185; nRows = 1; }
    if (g_page == PAGE_CLICK) bh2 = 180;   // bottom card hosts 4 rows (hotkey/keep + can-attack + can-place gates)
    if (g_page == PAGE_DASH) { bh1 = 205; bh2 = 190; }   // 曲线大卡 + 状态/个性化大卡
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
    if (g_page == PAGE_DASH) {
        // dashboard: two stacked full-width cards
        L.card[0] = { x0, top, xr, top + h1 };
        L.card[1] = { 0, 0, 0, 0 };
    }

    // ---- status bar ----
    L.status = { 12, H - 52, W - 12, H - 16 };
    {
        // 6 槽: 连点 / 辅助 / 攻击 / 放置 / 目标 / CPS曲线 (目标与CPS更宽)
        float wgt[6] = { 0.74f, 0.74f, 0.74f, 0.74f, 1.60f, 1.90f };
        float wsum = 0;
        for (float f : wgt) wsum += f;
        int totalW = (L.status.right - L.status.left) - 16;
        int x = L.status.left + 8;
        for (int i = 0; i < 6; i++) {
            int w = (int)(totalW * wgt[i] / wsum);
            L.stSlot[i] = { x, L.status.top + 4, x + w, L.status.bottom - 4 };
            x += w;
        }
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
            // 6 档预设 (含 30/s、40/s): 5 个 8px 间隔
            int pw = (L.card[0].right - L.card[0].left - 32 - 40) / 6;
            for (int k = 0; k < 6; k++) {
                int xl = L.card[0].left + 16 + k * (pw + 8);
                int xr = L.card[1].left + 16 + k * (pw + 8);
                L.preL[k] = { xl, L.card[0].top + S(96), xl + pw, L.card[0].top + S(124) };
                L.preR[k] = { xr, L.card[1].top + S(96), xr + pw, L.card[1].top + S(124) };
            }
        }
        L.btnKey  = { L.card[2].left + 16, L.card[2].top + S(32),
                      L.card[2].right - 16, L.card[2].top + S(56) };
        L.btnKeep = { L.card[2].left + 16, L.card[2].top + S(62),
                      L.card[2].right - 16, L.card[2].top + S(84) };
        // row 3: can-attack gate toggle + live status chip + hotkey
        // row 4: can-place  gate toggle + live status chip + hotkey
        {
            int cw = L.card[2].right - L.card[2].left - 32;
            int w1 = (int)(cw * 0.50f), w2 = (int)(cw * 0.20f);
            L.btnCanAtk = { L.card[2].left + 16, L.card[2].top + S(88),
                            L.card[2].left + 16 + w1, L.card[2].top + S(110) };
            L.canAtkChip = { L.btnCanAtk.right + 8, L.btnCanAtk.top,
                             L.btnCanAtk.right + 8 + w2, L.btnCanAtk.bottom };
            L.btnCanAtkKey = { L.canAtkChip.right + 8, L.btnCanAtk.top,
                               L.card[2].right - 16, L.btnCanAtk.bottom };
            L.btnPlace = { L.card[2].left + 16, L.card[2].top + S(114),
                           L.card[2].left + 16 + w1, L.card[2].top + S(136) };
            L.placeChip = { L.btnPlace.right + 8, L.btnPlace.top,
                            L.btnPlace.right + 8 + w2, L.btnPlace.bottom };
            L.btnPlaceKey = { L.placeChip.right + 8, L.btnPlace.top,
                              L.card[2].right - 16, L.btnPlace.bottom };
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
    case PAGE_DASH:
        L.dashCps   = { L.card[0].right - 150, L.card[0].top + S(6),
                        L.card[0].right - 20, L.card[0].top + S(30) };
        L.dashPlot  = { L.card[0].left + 16, L.card[0].top + S(36),
                        L.card[0].right - 16, L.card[0].top + S(148) };
        L.dashStats = { L.card[0].left + 20, L.card[0].top + S(154),
                        L.card[0].right - 20, L.card[0].top + S(178) };
        {
            int mid2 = L.card[2].left + (L.card[2].right - L.card[2].left) / 2;
            L.dashDiv = { mid2, L.card[2].top + S(14), mid2 + 1, L.card[2].bottom - S(10) };
            for (int i = 0; i < 4; i++) {
                L.dashRow[i] = { L.card[2].left + 20, L.card[2].top + S(36) + i * S(30),
                                 mid2 - 10, L.card[2].top + S(58) + i * S(30) };
            }
            L.renameLbl  = { mid2 + 10, L.card[2].top + S(24), L.card[2].right - 20, L.card[2].top + S(44) };
            L.inpProfile = { mid2 + 10, L.card[2].top + S(46), L.card[2].right - 20, L.card[2].top + S(72) };
            L.btnRename  = { mid2 + 10, L.card[2].top + S(76), mid2 + 120, L.card[2].top + S(100) };
            L.accLbl     = { mid2 + 10, L.card[2].top + S(108), L.card[2].right - 20, L.card[2].top + S(128) };
            for (int i = 0; i < 4; i++) {
                int cx = mid2 + 24 + i * 34;
                L.accDot[i] = { cx - 9, L.card[2].top + S(130), cx + 9, L.card[2].top + S(148) };
            }
        }
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
        // 拟人化节奏: 卡片右半 (模式芯片 4 个 + 强度滑块)
        {
            int mid2 = L.card[2].left + (int)((L.card[2].right - L.card[2].left) * 0.52f);
            int hx0 = mid2 + 12;
            int hw = (L.card[2].right - 16 - hx0 - 3 * 6) / 4;
            if (hw < 30) hw = 30;
            for (int k = 0; k < 4; k++) {
                L.hm[k] = { hx0 + k * (hw + 6), L.card[2].top + S(34),
                            hx0 + k * (hw + 6) + hw, L.card[2].top + S(56) };
            }
            L.humLevelLbl = { hx0, L.card[2].top + S(60), hx0 + 44, L.card[2].top + S(80) };
            L.track[SL_HUM] = { hx0 + 48, L.card[2].top + S(68),
                                L.card[2].right - 16, L.card[2].top + S(73) };
        }
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
    g_hr[E_NAV_DASH]   = { L.nav[3], E_NAV_DASH, false };
    g_hr[E_NAV_ADV]    = { L.nav[4], E_NAV_ADV, false };
    g_hr[E_BTN_THEME]  = { L.btnTheme, E_BTN_THEME, false };
    g_hr[E_BTN_PIN]    = { L.btnPin, E_BTN_PIN, false };
    g_hr[E_BTN_SOUND]  = { L.btnSound, E_BTN_SOUND, false };
    g_hr[E_BTN_CURSOR] = { L.btnCursor, E_BTN_CURSOR, false };
    g_hr[E_BTN_MIN]    = { L.btnMin, E_BTN_MIN, false };
    g_hr[E_BTN_MAX]    = { L.btnMax, E_BTN_MAX, false };
    g_hr[E_BTN_CLOSE]  = { L.btnClose, E_BTN_CLOSE, false };
    for (int k = 0; k < 4; k++)
        g_hr[E_PRF0 + k] = { L.prfChip[k], (Elem)(E_PRF0 + k), false };
    g_hr[E_CHIP_TARGET] = { L.stSlot[4], E_CHIP_TARGET, false };

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
        g_hr[E_BTN_PLACE] = { L.btnPlace, E_BTN_PLACE, false };
        g_hr[E_CHIP_PLACE] = { L.placeChip, E_CHIP_PLACE, false };
        g_hr[E_BTN_PLACE_KEY] = { L.btnPlaceKey, E_BTN_PLACE_KEY, false };
        for (int k = 0; k < 6; k++) {
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
    case PAGE_DASH:
        g_hr[E_ACC0] = { L.accDot[0], E_ACC0, false };
        g_hr[E_ACC1] = { L.accDot[1], E_ACC1, false };
        g_hr[E_ACC2] = { L.accDot[2], E_ACC2, false };
        g_hr[E_ACC3] = { L.accDot[3], E_ACC3, false };
        g_hr[E_INP_PROFILE] = { L.inpProfile, E_INP_PROFILE, false };
        g_hr[E_BTN_RENAME] = { L.btnRename, E_BTN_RENAME, false };
        break;
    case PAGE_ADV:
        g_hr[E_SL_MAX] = { L.thumb[SL_MAX], E_SL_MAX, false };
        g_hr[E_INP_MAX] = { L.inpMax, E_INP_MAX, false };
        g_hr[E_CHK_RAND] = { L.chkRand, E_CHK_RAND, false };
        g_hr[E_SL_RAND] = { L.thumb[SL_RAND], E_SL_RAND, false };
        g_hr[E_CHK_AUTOSTOP] = { L.chkAutoStop, E_CHK_AUTOSTOP, false };
        g_hr[E_INP_AUTOSTOP] = { L.inpAutoStop, E_INP_AUTOSTOP, false };
        for (int k = 0; k < 4; k++)
            g_hr[E_HM0 + k] = { L.hm[k], (Elem)(E_HM0 + k), false };
        g_hr[E_SL_HUM] = { L.thumb[SL_HUM], E_SL_HUM, false };
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
    // 玻璃态必须用灰度抗锯齿 (ClearType 的彩色边缘在 alpha 提升后
    // 会留下彩边, 在透明背景上非常难看)
    g_hfTitle = F(26, FW_BOLD,     ANTIALIASED_QUALITY);
    g_hfLabel = F(18, FW_SEMIBOLD, ANTIALIASED_QUALITY);
    g_hfBody  = F(17, FW_MEDIUM,   ANTIALIASED_QUALITY);
    g_hfSmall = F(14, FW_MEDIUM,   ANTIALIASED_QUALITY);
}

static void FreeLayers();

static void InitLayers()
{
    FreeLayers();
    g_chrome    = GLCreate(g_cx, g_cy);
    g_chromeDyn = GLCreate(g_cx, g_cy);
    g_content   = GLCreate(g_cx, g_cy);
    g_surface   = GLCreate(g_cx, g_cy);
    g_layersOk = g_chrome.dc && g_chromeDyn.dc && g_content.dc && g_surface.dc;
    g_needChrome = g_needChromeDyn = g_needContent = true;
}
static void FreeLayers()
{
    GLFree(g_chrome);
    GLFree(g_chromeDyn);
    GLFree(g_content);
    GLFree(g_surface);
    g_layersOk = false;
}

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
    case SL_HUM: r = (float)(humanizeLevel - 1) / 4.0f; break;
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
    case PAGE_ADV:   sync(E_SL_MAX, SL_MAX); sync(E_SL_RAND, SL_RAND); sync(E_SL_HUM, SL_HUM); break;
    default: break;
    }
}

// ============================================================
//  glass primitives (玻璃态绘制原语)
// ============================================================
// 所有绘制进 GLayer (g_chrome / g_chromeDyn / g_content):
//   - 半透明图形走 glass.cpp 软件光栅 (预乘 alpha, 1px 抗锯齿)
//   - 文字/图标/折线走 GDI, 画完用 GLLiftAlphaRect 提升 alpha
// 注意: GDI 画进层的颜色不能是纯黑 (提升判定用 RGB!=0)。

// GDI 文字辅助: 透明背景 + 灰度抗锯齿, 画完提升该区域 alpha
static void GText(GLayer& l, HFONT f, COLORREF c, const wchar_t* s, const RECT& r, UINT fmt)
{
    HDC dc = l.dc;
    SelectObject(dc, f);
    SetTextColor(dc, c);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, s, -1, (RECT*)&r, fmt);
    GLLiftAlphaRect(l, r);
}

// 玻璃面板: 柔和投影 + 玻璃填充 + 顶部高光渐变 + 发丝描边
static void GPanel(GLayer& l, const RECT& r, int rad, COLORREF fill, BYTE fillA)
{
    GLShadow(l, r, rad, 7, RGB(0, 0, 0), (BYTE)(g_theme == Theme::Dark ? 44 : 30));
    GLFillRound(l, r, rad, fill, fillA);
    GLFillV(l, r, rad, SHEEN(), (BYTE)(g_theme == Theme::Dark ? 12 : 34), SHEEN(), 0);
    GLRing(l, r, rad, 1, HAIRLINE(), (BYTE)(g_theme == Theme::Dark ? 26 : 34));
}

// 轻量面板 (拖动滑块等高频重绘时用: 无阴影无高光, 帧耗更低)
static void GPanelLite(GLayer& l, const RECT& r, int rad, COLORREF fill, BYTE fillA)
{
    GLFillRound(l, r, rad, fill, fillA);
    GLRing(l, r, rad, 1, HAIRLINE(), (BYTE)(g_theme == Theme::Dark ? 26 : 34));
}

// 玻璃按钮
static void GButton(GLayer& l, const RECT& r, int rad, bool hover, bool pressed, bool selected)
{
    if (pressed) {
        GLFillRound(l, r, rad, ACCENT(), 150);
        GLRing(l, r, rad, 1, ACCENT(), 200);
    } else if (selected) {
        GLShadow(l, r, rad, 5, ACCENT(), 50);
        GLFillRound(l, r, rad, ACCENT(), 225);
        GLRing(l, r, rad, 1, HAIRLINE(), (BYTE)48);
    } else if (hover) {
        GLShadow(l, r, rad, 4, ACCENT(), 26);
        GLFillRound(l, r, rad, HOVER(), (BYTE)(g_theme == Theme::Dark ? 26 : 46));
        GLRing(l, r, rad, 1, ACCENT(), (BYTE)110);
    } else {
        GLFillRound(l, r, rad, HOVER(), (BYTE)(g_theme == Theme::Dark ? 12 : 22));
        GLRing(l, r, rad, 1, HAIRLINE(), (BYTE)(g_theme == Theme::Dark ? 18 : 26));
    }
}

// 输入框/槽
static void GInset(GLayer& l, const RECT& r, int rad, bool focus)
{
    GLFillRound(l, r, rad, TRACK(), (BYTE)(g_theme == Theme::Dark ? 16 : 26));
    if (focus) {
        GLRing(l, r, rad, 1, ACCENT(), 230);
        GLShadow(l, r, rad, 4, ACCENT(), 30);
    } else {
        GLRing(l, r, rad, 1, TRACK(), (BYTE)(g_theme == Theme::Dark ? 28 : 48));
    }
}

// 玻璃开关
static void GToggle(GLayer& l, const RECT& tg, bool on)
{
    GLFillRound(l, tg, 12, TRACK(), (BYTE)(g_theme == Theme::Dark ? 22 : 38));
    GLRing(l, tg, 12, 1, TRACK(), (BYTE)(g_theme == Theme::Dark ? 34 : 58));
    if (on) {
        GLShadow(l, tg, 12, 4, ACCENT(), 46);
        GLFillRound(l, tg, 12, ACCENT(), 232);
    }
    int kw = tg.right - tg.left;
    int cy = (tg.top + tg.bottom) / 2, kr = (tg.bottom - tg.top) / 2 - 3;
    int kx = on ? tg.right - kr - 3 : tg.left + kr + 3;
    RECT krr = { kx - kr, cy - kr, kx + kr, cy + kr };
    GLShadow(l, krr, kr, 3, RGB(0, 0, 0), 40);
    GLFillRound(l, krr, kr, RGB(255, 255, 255), 255);
    GLRing(l, krr, kr, 1, HAIRLINE(), (BYTE)(g_theme == Theme::Dark ? 40 : 80));
}

// 玻璃滑块
static void GSlider(GLayer& l, int si, Elem elem)
{
    RECT& trk = L.track[si];
    bool active = g_hr[elem].hover || g_drag == elem;
    GLFillRound(l, trk, 4, TRACK(), (BYTE)(g_theme == Theme::Dark ? 20 : 36));
    GLRing(l, trk, 4, 1, TRACK(), (BYTE)(g_theme == Theme::Dark ? 30 : 56));
    int fx = ThumbX(si);
    if (fx > trk.left + 1) {
        RECT fr = { trk.left, trk.top, fx, trk.bottom };
        GLFillRound(l, fr, 4, ACCENT(), 235);
    }
    int cx = ThumbX(si), cy = (trk.top + trk.bottom) / 2, r = 8;
    RECT tr = { cx - r, cy - r, cx + r, cy + r };
    GLShadow(l, tr, r, 3, RGB(0, 0, 0), 48);
    GLFillRound(l, tr, r, RGB(255, 255, 255), 255);
    GLRing(l, tr, r, 1, active ? ACCENT() : HAIRLINE(), (BYTE)(active ? 210 : (g_theme == Theme::Dark ? 44 : 90)));
    if (active) GLShadow(l, tr, r, 3, ACCENT(), 42);
}

// 状态圆点 (可选发光)
static void GDot(GLayer& l, int x, int y, COLORREF c, bool glow)
{
    RECT d = { x, y, x + 8, y + 8 };
    if (glow) GLShadow(l, d, 4, 3, c, 70);
    GLFillRound(l, d, 4, c, 255);
}

// 只读信息芯片
static void GChip(GLayer& l, const RECT& r, int rad)
{
    GLFillRound(l, r, rad, HOVER(), (BYTE)(g_theme == Theme::Dark ? 14 : 24));
    GLRing(l, r, rad, 1, HAIRLINE(), (BYTE)(g_theme == Theme::Dark ? 16 : 24));
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

// 提示音开关图标: 喇叭 + 声波弧 (开) / 静音斜线 (关)
static void DrawSoundGlyph(HDC dc, int cx, int cy, bool on, COLORREF c)
{
    HPEN p = CreatePen(PS_SOLID, 2, c);
    HBRUSH b = CreateSolidBrush(c);
    SelectObject(dc, p); SelectObject(dc, b);
    // 喇叭本体: 矩形箱体 + 指向右上的号角三角
    POINT spk[5] = { { cx - 8, cy - 4 }, { cx - 4, cy - 4 },
                     { cx + 2, cy - 9 }, { cx + 2, cy + 9 }, { cx - 4, cy + 4 } };
    Polygon(dc, spk, 5);
    DeleteObject(b);
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    if (on) {
        // 两条声波弧 (从号角口向右扩开, 55° 开角)。
        // 注意: GDI Arc() 在此 DIB 环境只画出起终点小点、弧线不渲染,
        // 改用 Polyline 逐点逼近 (每弧 11 点, 2px 笔)。
        for (int r = 5; r <= 9; r += 4) {
            POINT pts[12];
            int n = 0;
            for (int k = 0; k <= 10; k++) {
                float th = (-55.0f + 11.0f * k) * 3.14159265f / 180.0f;
                pts[n].x = cx + 2 + (int)(r * cosf(th) + 0.5f);
                pts[n].y = cy - (int)(r * sinf(th) + 0.5f);
                n++;
            }
            Polyline(dc, pts, n);
        }
    } else {
        // 静音斜线
        MoveToEx(dc, cx + 2, cy - 8, nullptr);
        LineTo(dc, cx + 11, cy + 7);
    }
    DeleteObject(p);
}

// ---- 标题栏窗控图标 (无边框玻璃窗自绘) ----
static void DrawMinGlyph(HDC dc, int cx, int cy, COLORREF c)
{
    HPEN p = CreatePen(PS_SOLID, 2, c);
    SelectObject(dc, p);
    MoveToEx(dc, cx - 5, cy, nullptr);
    LineTo(dc, cx + 5, cy);
    DeleteObject(p);
}
static void DrawMaxGlyph(HDC dc, int cx, int cy, COLORREF c)
{
    HPEN p = CreatePen(PS_SOLID, 2, c);
    SelectObject(dc, p);
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, cx - 5, cy - 5, cx + 5, cy + 5, 2, 2);
    DeleteObject(p);
}
static void DrawRestoreGlyph(HDC dc, int cx, int cy, COLORREF c)
{
    HPEN p = CreatePen(PS_SOLID, 2, c);
    SelectObject(dc, p);
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    // 后层方块 (右下)
    RoundRect(dc, cx - 2, cy - 3, cx + 6, cy + 5, 2, 2);
    // 前层方块 (左上)
    RoundRect(dc, cx - 6, cy - 5, cx + 2, cy + 3, 2, 2);
    DeleteObject(p);
}
static void DrawCloseGlyph(HDC dc, int cx, int cy, COLORREF c)
{
    HPEN p = CreatePen(PS_SOLID, 2, c);
    SelectObject(dc, p);
    MoveToEx(dc, cx - 5, cy - 5, nullptr);
    LineTo(dc, cx + 5, cy + 5);
    MoveToEx(dc, cx + 5, cy - 5, nullptr);
    LineTo(dc, cx - 5, cy + 5);
    DeleteObject(p);
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
static void DrawDashIcon(HDC dc, int cx, int cy, COLORREF c)
{
    HPEN p = CreatePen(PS_SOLID, 2, c);
    SelectObject(dc, p);
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, cx - 8, cy - 8, cx + 8, cy + 8, 3, 3);
    MoveToEx(dc, cx - 5, cy + 4, nullptr);
    LineTo(dc, cx - 1, cy - 1);
    LineTo(dc, cx + 2, cy + 2);
    LineTo(dc, cx + 6, cy - 4);
    DeleteObject(p);
}
// 鼠标光标小图标 (箭头三角 + 尾线)
static void DrawCursorGlyph(HDC dc, int cx, int cy, COLORREF c)
{
    HPEN p = CreatePen(PS_SOLID, 2, c);
    HBRUSH b = CreateSolidBrush(c);
    SelectObject(dc, p);
    SelectObject(dc, b);
    POINT pts[3] = { { cx - 7, cy - 8 }, { cx - 7, cy + 4 }, { cx + 3, cy - 1 } };
    Polygon(dc, pts, 3);
    SelectObject(dc, GetStockObject(NULL_BRUSH));
    MoveToEx(dc, cx + 2, cy - 3, nullptr);
    LineTo(dc, cx + 9, cy + 5);
    LineTo(dc, cx + 3, cy + 8);
    LineTo(dc, cx + 1, cy - 1);
    DeleteObject(p);
    DeleteObject(b);
}
// ============================================================
//  glass render: 三层渲染 (chrome / chromeDyn / content) + 合成呈现
// ============================================================
// 由 main.cpp 包含 (render_new.inc)。渲染顺序: 基底玻璃在 chrome 层,
// 状态栏动态内容在 chromeDyn 层, 页面内容在 content 层; Present 按
// chrome -> chromeDyn -> content(动画偏移+淡入) 依次混合后 ULW 呈现。

// ---- 悬停提示气泡 ----
static void RenderTip(GLayer& l, const RECT& anchor, const wchar_t* const* lines, int n)
{
    HDC dc = l.dc;
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
    if (t.bottom > g_cy - 8) {
        t.top = anchor.top - 5 - th;
        t.bottom = anchor.top - 5;
    }
    if (t.right > g_cx - 8) { int dx = t.right - (g_cx - 8); t.left -= dx; t.right -= dx; }
    if (t.left < 8)         { int dx = 8 - t.left;         t.left += dx; t.right += dx; }
    if (t.top < 8)          t.top = 8;
    // 气泡用更实的玻璃底, 保证可读性
    GPanel(l, t, 8, PANEL(), (BYTE)(g_theme == Theme::Dark ? 168 : 200));
    RECT tr = { t.left + padX, t.top + padY - 3, t.right - padX, t.bottom - padY };
    for (int i = 0; i < n; i++) {
        RECT lr = tr;
        lr.top += i * lineH;
        lr.bottom = lr.top + lineH;
        GText(l, g_hfSmall, TXT_DIM(), lines[i], lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
}

// ============================================================
//  chrome 层: 基底玻璃 + 标题 + 芯片 + 侧栏 + 状态栏底板
// ============================================================
static void RenderChrome()
{
    if (!g_layersOk) return;
    GLayer& l = g_chrome;
    GLClear(l);
    HDC dc = l.dc;

    // ---- 基底玻璃: 垂直渐变 + 顶部氛围高光 + 玻璃边缘描边 ----
    // 无边框窗: 四角圆角(最大化时直角), 桌面从圆角外透出; 发丝描边
    // 勾勒玻璃轮廓 (无 DWM 阴影时保证边缘清晰)
    {
        int rad = IsZoomed(g_uiHwnd) ? 0 : 12;
        BYTE aTop = (BYTE)(g_theme == Theme::Dark ? 214 : 222);
        BYTE aBot = (BYTE)(g_theme == Theme::Dark ? 202 : 212);
        GLFillV(l, { 0, 0, g_cx, g_cy }, rad, BGTOP(), aTop, BGBOT(), aBot);
        GLFillV(l, { 0, 0, g_cx, g_cy / 3 }, rad, SHEEN(),
                (BYTE)(g_theme == Theme::Dark ? 7 : 30), SHEEN(), 0);
        if (rad > 0)
            GLRing(l, { 0, 0, g_cx, g_cy }, rad, 1, HAIRLINE(),
                   (BYTE)(g_theme == Theme::Dark ? 46 : 40));
    }

    // ---- 标题 ----
    {
        RECT tr = L.title;
        tr.left += 10; tr.top += 1; tr.right = L.btnSound.left - 14;
        GText(l, g_hfTitle, TXT(), L"AutoClicker", tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT vr = { tr.left, tr.top + 25, tr.right, tr.bottom };
        std::wstring verStr = std::wstring(L"v") + APP_VERSION_W;
        GText(l, g_hfSmall, TXT_DIM(), verStr.c_str(), vr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    // ---- 光标门控 / 提示音 / 图钉 / 主题 / 最小化 / 最大化 / 关闭 (自绘窗控) ----
    {
        RECT& b = L.btnCursor;
        bool hover = g_hr[E_BTN_CURSOR].hover;
        GButton(l, b, 13, hover, false, cursorOnlyClick.load(std::memory_order_relaxed));
        int cx = (b.left + b.right) / 2, cy = (b.top + b.bottom) / 2;
        DrawCursorGlyph(dc, cx, cy,
                        cursorOnlyClick.load(std::memory_order_relaxed) ? RGB(255, 255, 255)
                                                                        : (hover ? ACCENT() : TXT_DIM()));
        GLLiftAlphaRect(l, b);
        // 悬停提示画在 content 层 (见 RenderContent 的"悬停提示(最上层)"),
        // chrome 层会被内容卡片盖住下半部分。
    }
    {
        RECT& b = L.btnSound;
        bool hover = g_hr[E_BTN_SOUND].hover;
        GButton(l, b, 13, hover, false, soundEnabled);
        int cx = (b.left + b.right) / 2, cy = (b.top + b.bottom) / 2;
        DrawSoundGlyph(dc, cx, cy, soundEnabled,
                       soundEnabled ? RGB(255, 255, 255)
                                    : (hover ? ACCENT() : TXT_DIM()));
        GLLiftAlphaRect(l, b);
        if (hover) {
            RECT tip = { b.left - 10, 42, b.right + 10, 58 };
            GText(l, g_hfSmall, TXT_DIM(),
                  soundEnabled ? L"关闭提示音" : L"开启提示音",
                  tip, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }
    {
        RECT& b = L.btnPin;
        bool hover = g_hr[E_BTN_PIN].hover;
        GButton(l, b, 13, hover, false, topmost);
        int cx = (b.left + b.right) / 2, cy = (b.top + b.bottom) / 2;
        DrawPin(dc, cx, cy, topmost);
        GLLiftAlphaRect(l, b);
        if (hover) {
            RECT tip = { b.left - 10, 42, b.right + 10, 58 };
            GText(l, g_hfSmall, TXT_DIM(),
                  topmost ? L"取消置顶" : L"置顶窗口",
                  tip, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }
    {
        RECT& b = L.btnTheme;
        bool hover = g_hr[E_BTN_THEME].hover;
        GButton(l, b, 13, hover, false, false);
        GText(l, g_hfBody, TXT(), g_theme == Theme::Dark ? L"\x2600" : L"\x263E",
              b, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (hover) {
            RECT tip = { b.left - 10, 42, b.right + 10, 58 };
            GText(l, g_hfSmall, TXT_DIM(),
                  g_theme == Theme::Dark ? L"切换亮色" : L"切换深色",
                  tip, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }
    {
        RECT& b = L.btnMin;
        bool hover = g_hr[E_BTN_MIN].hover;
        GButton(l, b, 13, hover, false, false);
        int cx = (b.left + b.right) / 2, cy = (b.top + b.bottom) / 2;
        DrawMinGlyph(dc, cx, cy, hover ? ACCENT() : TXT_DIM());
        GLLiftAlphaRect(l, b);
        if (hover) {
            RECT tip = { b.left - 10, 42, b.right + 10, 58 };
            GText(l, g_hfSmall, TXT_DIM(), L"最小化", tip, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }
    {
        RECT& b = L.btnMax;
        bool hover = g_hr[E_BTN_MAX].hover;
        bool zoomed = IsZoomed(g_uiHwnd);
        GButton(l, b, 13, hover, false, false);
        int cx = (b.left + b.right) / 2, cy = (b.top + b.bottom) / 2;
        if (zoomed) DrawRestoreGlyph(dc, cx, cy, hover ? ACCENT() : TXT_DIM());
        else        DrawMaxGlyph(dc, cx, cy, hover ? ACCENT() : TXT_DIM());
        GLLiftAlphaRect(l, b);
        if (hover) {
            RECT tip = { b.left - 10, 42, b.right + 10, 58 };
            GText(l, g_hfSmall, TXT_DIM(), zoomed ? L"还原" : L"最大化",
                  tip, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }
    {
        RECT& b = L.btnClose;
        bool hover = g_hr[E_BTN_CLOSE].hover;
        if (hover) {
            GLShadow(l, b, 13, 3, RED(), 40);
            GLFillRound(l, b, 13, RED(), 210);
        } else {
            GButton(l, b, 13, hover, false, false);
        }
        int cx = (b.left + b.right) / 2, cy = (b.top + b.bottom) / 2;
        DrawCloseGlyph(dc, cx, cy, hover ? RGB(255, 255, 255) : TXT_DIM());
        GLLiftAlphaRect(l, b);
        if (hover) {
            RECT tip = { b.left - 10, 42, b.right + 10, 58 };
            GText(l, g_hfSmall, TXT_DIM(), L"关闭", tip, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }

    // ---- 方案芯片 (标题栏) ----
    for (int i = 0; i < 4; i++) {
        RECT& b = L.prfChip[i];
        bool sel = ((i + 1) == g_activeProfile);
        bool hover = g_hr[E_PRF0 + i].hover;
        GButton(l, b, 12, hover, false, sel);
        RECT tr = { b.left + 4, b.top, b.right - 4, b.bottom };
        GText(l, g_hfSmall, sel ? RGB(255, 255, 255) : (hover ? ACCENT() : TXT()),
              g_profileNames[i].c_str(), tr,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    // ---- 侧边栏 ----
    GPanel(l, L.sidebar, 14, PANEL(), (BYTE)(g_theme == Theme::Dark ? 62 : 104));
    static const wchar_t* navNames[5] = { L"连点", L"多倍", L"滚轮", L"面板", L"高级" };
    for (int i = 0; i < 5; i++) {
        RECT& b = L.nav[i];
        bool sel = ((int)g_page == i);
        bool hover = g_hr[E_NAV_CLICK + i].hover;
        GButton(l, b, 10, hover, false, sel);
        int cx = (b.left + b.right) / 2;
        int icY = b.top + 13;
        COLORREF ic = sel ? RGB(255, 255, 255) : (hover ? ACCENT() : TXT_DIM());
        switch (i) {
        case 0: DrawMouseIcon(dc, cx, icY, ic); break;
        case 1: DrawMultiIcon(dc, cx, icY, ic); break;
        case 2: DrawWheelIcon(dc, cx, icY, ic); break;
        case 3: DrawDashIcon(dc, cx, icY, ic); break;
        case 4: DrawAdvIcon(dc, cx, icY, ic); break;
        }
        GLLiftAlphaRect(l, b);
        RECT lr = { b.left, b.top + 32, b.right, b.bottom - 3 };
        GText(l, g_hfSmall, sel ? RGB(255, 255, 255) : (hover ? ACCENT() : TXT_DIM()),
              navNames[i], lr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    // ---- 状态栏底板 ----
    GPanel(l, L.status, 12, PANEL(), (BYTE)(g_theme == Theme::Dark ? 62 : 104));
}

// ============================================================
//  chromeDyn 层: 状态栏动态内容 (10Hz 级刷新, 整层很小)
// ============================================================
static void RenderChromeDyn()
{
    if (!g_layersOk) return;
    GLayer& l = g_chromeDyn;
    GLClear(l);
    HDC dc = l.dc;
    int baseY = L.status.top + 10;

    // 槽1 连点
    {
        RECT& s = L.stSlot[0];
        COLORREF clr = isstart ? GREEN() : RED();
        GDot(l, s.left + 4, baseY, clr, isstart);
        RECT r = { s.left + 16, s.top, s.right - 2, s.bottom };
        GText(l, g_hfSmall, clr, isstart ? L"连点 开" : L"连点 关",
              r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    // 槽2 辅助 (多倍 + 滚轮)
    {
        RECT& s = L.stSlot[1];
        bool on = isMultiActive || isScrollClickActive;
        COLORREF clr = on ? ACCENT() : RED();
        GDot(l, s.left + 4, baseY, clr, on);
        RECT r = { s.left + 16, s.top, s.right - 2, s.bottom };
        GText(l, g_hfSmall, clr, on ? L"辅助 开" : L"辅助 关",
              r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    // 槽3 攻击门控: dot = 实时可攻击态, 文字 = 功能开关
    {
        RECT& s = L.stSlot[2];
        COLORREF clr = RED();
        if (!canAttackOnlyClick) clr = RED();
        else if (!CanAttackConnected()) clr = TXT_DIM();
        else clr = g_canAttack.load(std::memory_order_relaxed) ? GREEN() : RED();
        GDot(l, s.left + 4, baseY, clr, canAttackOnlyClick && g_canAttack.load() == 1);
        RECT r = { s.left + 16, s.top, s.right - 2, s.bottom };
        GText(l, g_hfSmall, clr, canAttackOnlyClick ? L"攻击 开" : L"攻击 关",
              r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    // 槽4 放置门控
    {
        RECT& s = L.stSlot[3];
        COLORREF clr = RED();
        if (!placeOnlyRightClick) clr = RED();
        else if (!CanAttackConnected()) clr = TXT_DIM();
        else clr = g_canPlace.load(std::memory_order_relaxed) ? GREEN() : RED();
        GDot(l, s.left + 4, baseY, clr, placeOnlyRightClick && g_canPlace.load() == 1);
        RECT r = { s.left + 16, s.top, s.right - 2, s.bottom };
        GText(l, g_hfSmall, clr, placeOnlyRightClick ? L"放置 开" : L"放置 关",
              r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    // 槽5 目标: 准星目标类名 (短名)
    {
        RECT& s = L.stSlot[4];
        bool conn = CanAttackConnected();
        char tname[128] = {};
        GetShmTargetName(tname, sizeof(tname));
        COLORREF clr = TXT_DIM();
        std::wstring txt = L"目标 —";
        if (conn && tname[0]) {
            clr = ACCENT();
            const char* last = tname;
            for (const char* p = tname; *p; p++)
                if (*p == '.' || *p == '/' || *p == '$') last = p + 1;
            wchar_t wnm[128];
            MultiByteToWideChar(CP_UTF8, 0, last, -1, wnm, 128);
            txt = L"目标 " + std::wstring(wnm);
        }
        GDot(l, s.left + 4, baseY, clr, conn && tname[0]);
        RECT r = { s.left + 16, s.top, s.right - 2, s.bottom };
        GText(l, g_hfSmall, clr, txt.c_str(), r,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    // 槽6 CPS: 迷你曲线 + 数值
    {
        RECT& s = L.stSlot[5];
        GChip(l, s, 8);
        int sw = (s.right - s.left) - 54;
        if (sw > 76) sw = 76;
        if (sw < 20) sw = 20;
        int n = s_sparkCount < kSparkN ? s_sparkCount : kSparkN;
        int need = sw / 2;
        if (need > n) need = n;
        if (need >= 2) {
            int maxV = 1;
            for (int i = 0; i < need; i++) {
                int v = s_spark[(s_sparkCount - 1 - i + kSparkN * 4) % kSparkN];
                if (v > maxV) maxV = v;
            }
            if (maxV < 10) maxV = 10;
            POINT pts[64];
            for (int i = 0; i < need; i++) {
                int v = s_spark[(s_sparkCount - 1 - i + kSparkN * 4) % kSparkN];
                pts[i] = { s.left + 4 + sw - i * 2,
                           s.bottom - 4 - (s.bottom - s.top - 10) * v / maxV };
            }
            HPEN sp = CreatePen(PS_SOLID, 1, ACCENT());
            HGDIOBJ op = SelectObject(dc, sp);
            Polyline(dc, pts, need);
            SelectObject(dc, op);
            DeleteObject(sp);
            GLLiftAlphaRect(l, s);
        }
        wchar_t cb[32];
        swprintf(cb, 32, L"%d/s", GetRealtimeCps());
        RECT tr = { s.left + sw + 10, s.top, s.right - 4, s.bottom };
        GText(l, g_hfSmall, TXT(), cb, tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
}

// ============================================================
//  content 层: 当前页全部内容 (页切换动画只移动这一层)
// ============================================================
static void RenderContent()
{
    if (!g_layersOk) return;
    GLayer& l = g_content;
    GLClear(l);
    HDC dc = l.dc;

    // ---- 卡片 ----
    int cardCount = 0;
    switch (g_page) {
    case PAGE_CLICK: cardCount = 3; break;
    case PAGE_MULTI: cardCount = 3; break;
    case PAGE_SCROLL: cardCount = 1; break;
    case PAGE_DASH: cardCount = 2; break;
    case PAGE_ADV: cardCount = 3; break;
    default: break;
    }
    for (int i = 0; i < cardCount; i++) {
        if (g_drag != E_NONE)
            GPanelLite(l, L.card[i], 14, CARD(), (BYTE)(g_theme == Theme::Dark ? 74 : 116));
        else
            GPanel(l, L.card[i], 14, CARD(), (BYTE)(g_theme == Theme::Dark ? 74 : 116));
    }

    // ---- 卡片标签 ----
    static const wchar_t* namesClick[3] = {
        L"左键", L"右键", L"快捷键 与 保持"
    };
    static const wchar_t* namesMulti[3] = {
        L"倍率", L"延迟", L"快捷键"
    };
    static const wchar_t* namesScroll[1] = { L"滚轮点击" };
    static const wchar_t* namesDash[2] = {
        L"实时 CPS 面板", L"游戏状态 与 个性化"
    };
    static const wchar_t* namesAdv[3] = {
        L"CPS 上限", L"随机 CPS", L"定时停止 与 拟人化"
    };
    const wchar_t** names = namesClick;
    switch (g_page) {
    case PAGE_CLICK: names = namesClick; break;
    case PAGE_MULTI: names = namesMulti; break;
    case PAGE_SCROLL: names = namesScroll; break;
    case PAGE_DASH: names = namesDash; break;
    case PAGE_ADV: names = namesAdv; break;
    }
    for (int i = 0; i < cardCount; i++) {
        RECT r = { L.card[i].left + 20, L.card[i].top + S(8),
                   L.card[i].right, L.card[i].top + S(26) };
        GText(l, g_hfLabel, TXT(), names[i], r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    auto DrawKeyBtn = [&](const RECT& b, const std::wstring& label,
                          Elem elem, bool rebinding) {
        GButton(l, b, 10, g_hr[elem].hover, false, rebinding);
        GText(l, g_hfBody, rebinding ? RGB(255, 255, 255) : TXT(),
              label.c_str(), b, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    };

    auto DrawPresetRow = [&](const RECT* pre, int baseElem,
                             const wchar_t* const* labels, int selIdx, int n) {
        for (int k = 0; k < n; k++) {
            const RECT& b = pre[k];
            bool sel = (selIdx == k);
            bool hover = g_hr[baseElem + k].hover;
            GButton(l, b, 8, hover, false, sel);
            GText(l, g_hfSmall, sel ? RGB(255, 255, 255) : (hover ? ACCENT() : TXT()),
                  labels[k], b, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    };

    // ================= PAGE: CLICK =================
    if (g_page == PAGE_CLICK) {
        GToggle(l, L.tglL, leftenabled);
        GToggle(l, L.tglR, rightenabled);
        GSlider(l, SL_L, E_SL_L);
        GSlider(l, SL_R, E_SL_R);

        static const wchar_t* cpsLbl[6] = { L"6/s", L"10/s", L"15/s", L"20/s", L"30/s", L"40/s" };
        int selL = -1, selR = -1;
        for (int k = 0; k < 6; k++) {
            if (cpsLeft10  == kCpsPresets[k] * 10) selL = k;
            if (cpsRight10 == kCpsPresets[k] * 10) selR = k;
        }
        DrawPresetRow(L.preL, E_PRE_L0, cpsLbl, selL, 6);
        DrawPresetRow(L.preR, E_PRE_R0, cpsLbl, selR, 6);

        // 数值
        for (int i = 0; i < 2; i++) {
            int c10 = (i == 0) ? cpsLeft10 : cpsRight10;
            int ms  = (i == 0) ? leftms : rightms;
            wchar_t buf[32];
            swprintf(buf, 32, L"%.1f 次/秒", c10 / 10.0f);
            RECT r = { L.card[i].left + 20, L.card[i].top + 60,
                       L.track[SL_L + i].right, L.card[i].top + 86 };
            GText(l, g_hfLabel, TXT(), buf, r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            swprintf(buf, 32, L"%d 毫秒", ms);
            RECT r2 = { r.right + 8, r.top, L.card[i].right - 8, r.bottom };
            GText(l, g_hfSmall, TXT_DIM(), buf, r2, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }

        // 快捷键
        {
            RECT& b = L.btnKey;
            std::wstring t = (g_rebinding == E_BTN_KEY)
                ? L"请按下新键…"
                : L"快捷键: " + getKeyName(vk_key);
            DrawKeyBtn(b, t, E_BTN_KEY, g_rebinding == E_BTN_KEY);
        }
        // 保持模式
        {
            RECT& b = L.btnKeep;
            bool hover = g_hr[E_BTN_KEEP].hover;
            GButton(l, b, 10, hover, false, keepClicke);
            GText(l, g_hfBody, keepClicke ? RGB(255, 255, 255) : TXT(),
                  keepClicke ? L"不需要按住连点: 开" : L"不需要按住连点: 关",
                  b, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        // 仅能攻击时连点
        {
            RECT& b = L.btnCanAtk;
            bool hover = g_hr[E_BTN_CANATK].hover;
            GButton(l, b, 10, hover, false, canAttackOnlyClick);
            GText(l, g_hfBody, canAttackOnlyClick ? RGB(255, 255, 255) : TXT(),
                  canAttackOnlyClick ? L"仅能攻击时连点: 开" : L"仅能攻击时连点: 关",
                  b, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        {
            RECT& chip = L.canAtkChip;
            GChip(l, chip, 8);
            COLORREF cc = TXT_DIM();
            const wchar_t* txt = L"未连接";
            if (CanAttackConnected()) {
                if (g_canAttack.load(std::memory_order_relaxed) == 1) {
                    cc = GREEN(); txt = L"可攻击";
                } else {
                    cc = RED(); txt = L"不可攻击";
                }
            }
            GText(l, g_hfSmall, cc, txt, chip, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        {
            RECT& b = L.btnCanAtkKey;
            std::wstring t = (g_rebinding == E_BTN_CANATK_KEY)
                ? L"请按下新键…"
                : L"快捷键: " + getKeyName(vk_canattack_key);
            DrawKeyBtn(b, t, E_BTN_CANATK_KEY, g_rebinding == E_BTN_CANATK_KEY);
        }
        // 仅手持放置物时右键连点
        {
            RECT& b = L.btnPlace;
            bool hover = g_hr[E_BTN_PLACE].hover;
            GButton(l, b, 10, hover, false, placeOnlyRightClick);
            GText(l, g_hfBody, placeOnlyRightClick ? RGB(255, 255, 255) : TXT(),
                  placeOnlyRightClick ? L"仅手持放置物时右键连点: 开" : L"仅手持放置物时右键连点: 关",
                  b, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        {
            RECT& chip = L.placeChip;
            GChip(l, chip, 8);
            COLORREF cc = TXT_DIM();
            const wchar_t* txt = L"未连接";
            if (CanAttackConnected()) {
                if (g_canPlace.load(std::memory_order_relaxed) == 1) {
                    cc = GREEN(); txt = L"手持放置物";
                } else {
                    cc = RED(); txt = L"非放置物";
                }
            }
            GText(l, g_hfSmall, cc, txt, chip, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        {
            RECT& b = L.btnPlaceKey;
            std::wstring t = (g_rebinding == E_BTN_PLACE_KEY)
                ? L"请按下新键…"
                : L"快捷键: " + getKeyName(vk_place_key);
            DrawKeyBtn(b, t, E_BTN_PLACE_KEY, g_rebinding == E_BTN_PLACE_KEY);
        }
    }

    // ================= PAGE: MULTI =================
    if (g_page == PAGE_MULTI) {
        GSlider(l, SL_MUL, E_SL_MUL);
        GSlider(l, SL_DEL, E_SL_DEL);

        static const wchar_t* mulLbl[4] = { L"2x", L"3x", L"4x", L"5x" };
        static const wchar_t* delLbl[4] = { L"10", L"25", L"50", L"100" };
        int selM = -1, selD = -1;
        for (int k = 0; k < 4; k++) {
            if (multiMul == kMulPresets[k])       selM = k;
            if (multiDelayMs == kDelayPresets[k]) selD = k;
        }
        DrawPresetRow(L.preM, E_PRE_M0, mulLbl, selM, 4);
        DrawPresetRow(L.preD, E_PRE_D0, delLbl, selD, 4);

        wchar_t buf[32];
        swprintf(buf, 32, L"%d 倍", multiMul);
        RECT r0 = { L.card[0].left + 20, L.card[0].top + 60,
                    L.track[SL_MUL].right, L.card[0].top + 86 };
        GText(l, g_hfLabel, TXT(), buf, r0, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        swprintf(buf, 32, L"%d 毫秒", multiDelayMs);
        RECT r1 = { L.card[1].left + 20, L.card[1].top + 60,
                    L.track[SL_DEL].right, L.card[1].top + 86 };
        GText(l, g_hfLabel, TXT(), buf, r1, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        {
            RECT& b = L.btnMKey;
            std::wstring t = (g_rebinding == E_BTN_MKEY)
                ? L"请按下新键…"
                : L"快捷键: " + getKeyName(vk_multi_key);
            DrawKeyBtn(b, t, E_BTN_MKEY, g_rebinding == E_BTN_MKEY);
        }
        RECT hint = { L.card[2].left + 20, L.card[2].top + S(68),
                      L.card[2].right - 20, L.card[2].top + S(90) };
        GText(l, g_hfSmall, TXT_DIM(), L"按 + / - 键可快速微调倍数",
              hint, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    // ================= PAGE: SCROLL =================
    if (g_page == PAGE_SCROLL) {
        GToggle(l, L.tglScroll, isScrollClickActive);

        {
            std::wstring t = (g_rebinding == E_BTN_SCROLL_KEY)
                ? L"请按下新键…"
                : L"快捷键: " + getKeyName(vk_scroll_key);
            DrawKeyBtn(L.btnScrollKey, t, E_BTN_SCROLL_KEY,
                       g_rebinding == E_BTN_SCROLL_KEY);
        }
        {
            std::wstring t = (g_rebinding == E_BTN_SCROLL_LR_KEY)
                ? L"请按下新键…"
                : L"切换 L/R: " + getKeyName(vk_scroll_lr_key);
            DrawKeyBtn(L.btnScrollLRKey, t, E_BTN_SCROLL_LR_KEY,
                       g_rebinding == E_BTN_SCROLL_LR_KEY);
        }
        // 左/右分段选择器 (胶囊式)
        {
            RECT& b = L.btnScrollLR;
            int midX = (b.left + b.right) / 2;
            GButton(l, b, 8, false, false, false);
            RECT half = { scrollClickButton == 0 ? b.left : midX, b.top,
                          scrollClickButton == 0 ? midX : b.right, b.bottom };
            GLFillRound(l, half, 8, ACCENT(), 216);
            GText(l, g_hfBody, scrollClickButton == 0 ? RGB(255, 255, 255) : TXT_DIM(),
                  L"左键", { b.left, b.top, midX, b.bottom },
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            GText(l, g_hfBody, scrollClickButton == 1 ? RGB(255, 255, 255) : TXT_DIM(),
                  L"右键", { midX, b.top, b.right, b.bottom },
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        RECT hint = { L.card[0].left + 20, L.card[0].top + S(110),
                      L.card[0].right - 20, L.card[0].top + S(132) };
        GText(l, g_hfSmall, TXT_DIM(), L"滚动滚轮时触发点击，向上/向下均可",
              hint, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    // ================= PAGE: DASH =================
    if (g_page == PAGE_DASH) {
        // 实时 CPS 大字
        {
            wchar_t buf[48];
            swprintf(buf, 48, L"%d 次/秒", GetRealtimeCps());
            GText(l, g_hfTitle, ACCENT(), buf, L.dashCps,
                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
        // CPS 曲线
        {
            RECT& pr = L.dashPlot;
            GInset(l, pr, 10, false);
            int pw = pr.right - pr.left;
            int ph = pr.bottom - pr.top;
            for (int i = 1; i <= 3; i++) {
                int gy = pr.top + ph * i / 4;
                GLHLine(l, pr.left + 6, pr.right - 6, gy, TRACK(),
                        (BYTE)(g_theme == Theme::Dark ? 14 : 26));
            }
            int n = s_sparkCount < kSparkN ? s_sparkCount : kSparkN;
            if (n >= 2) {
                int maxV = 1;
                for (int i = 0; i < n; i++)
                    if (s_spark[(s_sparkCount - 1 - i + kSparkN * 4) % kSparkN] > maxV)
                        maxV = s_spark[(s_sparkCount - 1 - i + kSparkN * 4) % kSparkN];
                if (maxV < 10) maxV = 10;
                POINT pts[kSparkN + 2];
                int xStep = (pw - 24) / (kSparkN - 1);
                for (int i = 0; i < n; i++) {
                    int v = s_spark[(s_sparkCount - 1 - i + kSparkN * 4) % kSparkN];
                    int x = pr.right - 12 - i * xStep;
                    int y = pr.bottom - 8 - (pr.bottom - pr.top - 20) * v / maxV;
                    pts[i] = { x, y };
                }
                // 面积 (软件列填充, 半透明)
                for (int i = 0; i < n; i++) {
                    int x = pts[i].x;
                    int y = pts[i].y;
                    if (y < pr.bottom - 7)
                        GLFillRound(l, { x, y, x + 2, pr.bottom - 7 }, 1, ACCENT(), 44);
                }
                // 折线 (GDI, 不透明)
                HPEN alp = CreatePen(PS_SOLID, 1, ACCENT());
                HGDIOBJ op = SelectObject(dc, alp);
                Polyline(dc, pts, n);
                SelectObject(dc, op);
                DeleteObject(alp);
                GLLiftAlphaRect(l, pr);
                // 最新点
                GDot(l, pts[0].x - 4, pts[0].y - 4, ACCENT(), true);
                wchar_t mb[24];
                swprintf(mb, 24, L"%d", maxV);
                RECT mr = { pr.left + 10, pr.top + 4, pr.left + 60, pr.top + 22 };
                GText(l, g_hfSmall, TXT_DIM(), mb, mr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            } else {
                RECT er = { pr.left, pr.top, pr.right, pr.bottom };
                GText(l, g_hfSmall, TXT_DIM(), L"正在采集实时 CPS…",
                      er, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
        }
        // 统计行
        {
            wchar_t sb[64];
            long long total = g_clickCount.load(std::memory_order_relaxed);
            swprintf(sb, 64, L"累计点击  %lld", total);
            RECT r = { L.dashStats.left, L.dashStats.top, L.dashStats.right - 140, L.dashStats.bottom };
            GText(l, g_hfBody, TXT(), sb, r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            ULONGLONG sec = (GetTickCount64() - s_sessionStartMs) / 1000;
            swprintf(sb, 64, L"会话  %02llu:%02llu", sec / 60, sec % 60);
            RECT r2 = { L.dashStats.right - 140, L.dashStats.top, L.dashStats.right, L.dashStats.bottom };
            GText(l, g_hfBody, TXT_DIM(), sb, r2, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
        // 游戏状态 (左列)
        {
            bool conn = CanAttackConnected();
            char tname[128] = {};
            GetShmTargetName(tname, sizeof(tname));
            int inGame = GetShmInGame();
            int hit = GetShmHitType();

            const wchar_t* rowNames[4] = { L"连接", L"游戏内", L"目标", L"命中" };
            std::wstring rowVal[4];
            COLORREF rowClr[4] = { TXT_DIM(), TXT_DIM(), TXT_DIM(), TXT_DIM() };
            if (conn) {
                rowVal[0] = L"已连接"; rowClr[0] = GREEN();
                rowVal[1] = inGame ? L"在游戏中" : L"主菜单";
                rowClr[1] = inGame ? GREEN() : TXT_DIM();
                if (tname[0]) {
                    wchar_t wnm[128];
                    MultiByteToWideChar(CP_UTF8, 0, tname, -1, wnm, 128);
                    rowVal[2] = wnm; rowClr[2] = TXT();
                } else {
                    rowVal[2] = L"无"; rowClr[2] = TXT_DIM();
                }
                switch (hit) {
                case 1:  rowVal[3] = L"方块"; rowClr[3] = TXT(); break;
                case 2:  rowVal[3] = L"实体"; rowClr[3] = ACCENT(); break;
                default: rowVal[3] = L"未命中"; rowClr[3] = TXT_DIM(); break;
                }
            } else {
                rowVal[0] = L"未连接";
                rowVal[1] = L"—";
                rowVal[2] = L"—";
                rowVal[3] = L"—";
            }
            for (int i = 0; i < 4; i++) {
                RECT& r = L.dashRow[i];
                RECT lr = { r.left, r.top, r.left + 76, r.bottom };
                GText(l, g_hfBody, TXT_DIM(), rowNames[i], lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                RECT vr = { r.left + 80, r.top, r.right, r.bottom };
                GText(l, g_hfBody, rowClr[i], rowVal[i].c_str(), vr,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
        }
        // 分隔线
        GLFillRound(l, L.dashDiv, 0, TRACK(), (BYTE)(g_theme == Theme::Dark ? 22 : 30));
        // 方案重命名 (右列)
        {
            GText(l, g_hfSmall, TXT_DIM(), L"当前方案", L.renameLbl,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        {
            RECT& b = L.inpProfile;
            bool focus = g_inputOn && g_inputTarget == IN_PROFILE;
            GInset(l, b, 8, focus);
            if (focus) GText(l, g_hfBody, TXT(), g_inputBuf, b,
                             DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            else GText(l, g_hfBody, TXT(), g_profileNames[g_activeProfile - 1].c_str(), b,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
        {
            RECT& b = L.btnRename;
            bool hover = g_hr[E_BTN_RENAME].hover;
            GButton(l, b, 10, hover, false, g_inputOn && g_inputTarget == IN_PROFILE);
            GText(l, g_hfBody, (g_inputOn && g_inputTarget == IN_PROFILE) ? RGB(255, 255, 255) : TXT(),
                  L"重命名", b, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        // 强调色 (右列)
        {
            GText(l, g_hfSmall, TXT_DIM(), L"强调色", L.accLbl,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        for (int i = 0; i < 4; i++) {
            RECT& b = L.accDot[i];
            bool sel = (i == g_accentIdx);
            bool hover = g_hr[E_ACC0 + i].hover;
            GLFillRound(l, b, 9, ACCENT_RAW(i), 255);
            if (sel) {
                GLRing(l, { b.left - 3, b.top - 3, b.right + 3, b.bottom + 3 }, 12, 1, TXT(), 220);
                GLShadow(l, b, 9, 3, ACCENT_RAW(i), 60);
            } else if (hover) {
                GLRing(l, { b.left - 3, b.top - 3, b.right + 3, b.bottom + 3 }, 12, 1, ACCENT_RAW(i), 130);
            }
        }
        // 提示
        {
            RECT hint = { L.card[2].left + 20, L.card[2].top + S(162),
                          L.card[2].right - 20, L.card[2].top + S(184) };
            GText(l, g_hfSmall, TXT_DIM(), L"标题栏点按方案可快速切换 · 可绑定切换热键",
                  hint, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
    }

    // ================= PAGE: ADV =================
    if (g_page == PAGE_ADV) {
        // CPS 上限
        GSlider(l, SL_MAX, E_SL_MAX);
        {
            RECT& bi = L.inpMax;
            bool focus = g_inputOn && g_inputTarget == IN_CPSMAX;
            GInset(l, bi, 6, focus);
            if (focus) GText(l, g_hfBody, TXT(), g_inputBuf, bi, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            else {
                wchar_t ibuf[8];
                swprintf(ibuf, 8, L"%d", cpsMax);
                GText(l, g_hfBody, TXT(), ibuf, bi, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            RECT sec = { bi.right + 6, bi.top, bi.right + 60, bi.bottom };
            GText(l, g_hfSmall, TXT_DIM(), L"次/秒", sec, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        // 随机 CPS
        {
            RECT& cb = L.chkRand;
            int box = cb.left;
            int by = (cb.top + cb.bottom) / 2;
            RECT sq = { box, by - 8, box + 16, by + 8 };
            bool hover = g_hr[E_CHK_RAND].hover;
            GButton(l, sq, 4, hover, false, randomCpsEnabled);
            if (randomCpsEnabled) {
                DrawCheck(dc, sq);
                GLLiftAlphaRect(l, sq);
            }
            RECT txt = { box + 22, cb.top, cb.right, cb.bottom };
            GText(l, g_hfBody, hover ? ACCENT() : TXT(), L"随机波动", txt,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            if (randomCpsEnabled) {
                GSlider(l, SL_RAND, E_SL_RAND);
                wchar_t bufR[32];
                swprintf(bufR, 32, L"±%d CPS", randomCpsRange);
                RECT rRand = { L.track[SL_RAND].right + 8, L.card[1].top + S(56),
                               L.card[1].right - 16, L.card[1].top + S(74) };
                GText(l, g_hfBody, TXT(), bufR, rRand, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            }
            RECT hint = { L.card[1].left + 20, L.card[1].top + S(100),
                          L.card[1].right - 20, L.card[1].top + S(122) };
            GText(l, g_hfSmall, TXT_DIM(),
                  randomCpsEnabled ? L"连点速度在 ±N CPS 内随机波动"
                                   : L"勾选后连点速度随机波动",
                  hint, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        // 定时停止
        {
            RECT& cb = L.chkAutoStop;
            int box = cb.left;
            int by = (cb.top + cb.bottom) / 2;
            RECT sq = { box, by - 8, box + 16, by + 8 };
            bool hover = g_hr[E_CHK_AUTOSTOP].hover;
            GButton(l, sq, 4, hover, false, autoStopEnabled);
            if (autoStopEnabled) {
                DrawCheck(dc, sq);
                GLLiftAlphaRect(l, sq);
            }
            RECT txt = { box + 22, cb.top, cb.right, cb.bottom };
            GText(l, g_hfBody, hover ? ACCENT() : TXT(), L"开启", txt,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            RECT& bi = L.inpAutoStop;
            bool focus = g_inputOn && g_inputTarget == IN_AUTOSTOP;
            GInset(l, bi, 6, focus);
            if (focus) GText(l, g_hfBody, TXT(), g_inputBuf, bi, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            else {
                wchar_t ib[16];
                swprintf(ib, 16, L"%d", autoStopSeconds);
                GText(l, g_hfBody, TXT(), ib, bi, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
            RECT sec = { bi.right + 6, bi.top, bi.right + 44, bi.bottom };
            GText(l, g_hfSmall, TXT_DIM(), L"秒", sec, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            RECT hint = { L.card[2].left + 20, L.card[2].top + S(62),
                          L.card[2].left + 216, L.card[2].top + S(84) };
            GText(l, g_hfSmall, TXT_DIM(), L"开启后 N 秒自动停止",
                  hint, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        // 拟人化节奏
        {
            RECT sub = { L.hm[0].left, L.card[2].top + S(12),
                         L.card[2].right - 16, L.card[2].top + S(30) };
            GText(l, g_hfSmall, TXT_DIM(), L"拟人化节奏", sub,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        {
            static const wchar_t* hmLbl[4] = { L"均匀", L"双击", L"呼吸", L"疲劳" };
            for (int k = 0; k < 4; k++) {
                RECT& b = L.hm[k];
                bool sel = (humanizeMode == k);
                bool hover = g_hr[E_HM0 + k].hover;
                GButton(l, b, 9, hover, false, sel);
                GText(l, g_hfSmall, sel ? RGB(255, 255, 255) : (hover ? ACCENT() : TXT()),
                      hmLbl[k], b, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
        }
        {
            GSlider(l, SL_HUM, E_SL_HUM);
            wchar_t lb[32];
            swprintf(lb, 32, L"%d", humanizeLevel);
            GText(l, g_hfSmall, TXT(), lb, L.humLevelLbl,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        {
            RECT hint = { L.hm[0].left, L.card[2].top + S(84),
                          L.card[2].right - 16, L.card[2].top + S(104) };
            GText(l, g_hfSmall, TXT_DIM(), L"双击=短促连招 · 呼吸=CPS起伏 · 疲劳=越按越慢",
                  hint, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }

    // ================= 悬停提示 (最上层) =================
    {
        // 方案芯片 (标题栏)
        for (int i = 0; i < 4; i++) {
            if (!g_hr[E_PRF0 + i].hover) continue;
            static const wchar_t* tip[] = {
                L"切换到该方案",
                L"方案保存全部设置，可在面板页重命名",
                L"可绑定切换热键（高级页）"
            };
            RenderTip(l, L.prfChip[i], tip, 3);
        }
        // 光标门控开关 (标题栏; 提示需画在 content 层才不会被卡片盖住)
        if (g_hr[E_BTN_CURSOR].hover) {
            static const wchar_t* tip[] = {
                L"开启后仅检测不到光标的时候允许连点",
                L"光标可见时（背包/聊天/菜单）自动暂停"
            };
            RenderTip(l, L.btnCursor, tip, 2);
        }
        // 目标芯片 (状态栏)
        if (g_hr[E_CHIP_TARGET].hover) {
            char tname[128] = {};
            GetShmTargetName(tname, sizeof(tname));
            if (!CanAttackConnected()) {
                static const wchar_t* tip[] = {
                    L"未收到游戏上报",
                    L"开启攻击/放置门控后自动注入 DLL"
                };
                RenderTip(l, L.stSlot[4], tip, 2);
            } else if (tname[0]) {
                wchar_t full[140];
                MultiByteToWideChar(CP_UTF8, 0, tname, -1, full, 140);
                const wchar_t* tip[2] = { L"准星目标实体", full };
                RenderTip(l, L.stSlot[4], tip, 2);
            } else {
                static const wchar_t* tip[] = { L"准星未对准任何实体" };
                RenderTip(l, L.stSlot[4], tip, 1);
            }
        }
        if (g_page == PAGE_CLICK) {
            if (g_hr[E_BTN_CANATK].hover) {
                static const wchar_t* tip[] = {
                    L"开启后仅左键在准星对准可攻击生物时连点",
                    L"右键不受影响；未开启时左右键照常",
                    L"支持版本：1.8.8 ~ 1.21.11 几乎全版本（原版/Forge/Fabric/NeoForge）",
                    L"网易中国版已实测可用（1.20.1 Forge）"
                };
                RenderTip(l, L.btnCanAtk, tip, 4);
            }
            if (g_hr[E_CHIP_CANATK].hover) {
                if (!CanAttackConnected()) {
                    static const wchar_t* tip[] = {
                        L"未收到游戏上报（未注入/游戏未运行）",
                        L"请先启动游戏并开启此开关，此时按不可攻击处理"
                    };
                    RenderTip(l, L.canAtkChip, tip, 2);
                } else if (g_canAttack.load(std::memory_order_relaxed) == 1) {
                    static const wchar_t* tip[] = { L"准星目标可攻击，左键可连点" };
                    RenderTip(l, L.canAtkChip, tip, 1);
                } else {
                    static const wchar_t* tip[] = {
                        L"准星目标不可攻击或未对准",
                        L"左键连点已暂停"
                    };
                    RenderTip(l, L.canAtkChip, tip, 2);
                }
            }
            if (g_hr[E_BTN_CANATK_KEY].hover) {
                static const wchar_t* tip[] = {
                    L"设置开关快捷键",
                    L"按下任意键绑定 · Esc 清除"
                };
                RenderTip(l, L.btnCanAtkKey, tip, 2);
            }
            if (g_hr[E_BTN_PLACE].hover) {
                static const wchar_t* tip[] = {
                    L"开启后只有左手持有放置物（方块）时",
                    L"右键才会连点，左键照常",
                    L"支持版本：1.8.8 ~ 1.21.11 几乎全版本（原版/Forge/Fabric/NeoForge）"
                };
                RenderTip(l, L.btnPlace, tip, 3);
            }
            if (g_hr[E_CHIP_PLACE].hover) {
                if (!CanAttackConnected()) {
                    static const wchar_t* tip[] = { L"未收到游戏上报（未注入/游戏未运行）" };
                    RenderTip(l, L.placeChip, tip, 1);
                } else if (g_canPlace.load(std::memory_order_relaxed) == 1) {
                    static const wchar_t* tip[] = { L"左手所持为放置物，右键可连点" };
                    RenderTip(l, L.placeChip, tip, 1);
                } else {
                    static const wchar_t* tip[] = {
                        L"左手不是放置物或空手",
                        L"右键连点已暂停"
                    };
                    RenderTip(l, L.placeChip, tip, 2);
                }
            }
            if (g_hr[E_BTN_PLACE_KEY].hover) {
                static const wchar_t* tip[] = {
                    L"设置开关快捷键",
                    L"按下任意键绑定 · Esc 清除"
                };
                RenderTip(l, L.btnPlaceKey, tip, 2);
            }
        }
        if (g_page == PAGE_DASH) {
            static const wchar_t* accNames[4] = { L"蓝", L"紫", L"绿", L"橙" };
            for (int i = 0; i < 4; i++) {
                if (!g_hr[E_ACC0 + i].hover) continue;
                std::wstring t = std::wstring(L"强调色: ") + accNames[i];
                const wchar_t* tip[1] = { t.c_str() };
                RenderTip(l, L.accDot[i], tip, 1);
            }
            if (g_hr[E_BTN_RENAME].hover) {
                static const wchar_t* tip[] = {
                    L"重命名当前方案",
                    L"输入后回车确认 · Esc 取消"
                };
                RenderTip(l, L.btnRename, tip, 2);
            }
        }
        if (g_page == PAGE_ADV) {
            static const wchar_t* hmTip[4][2] = {
                { L"均匀: 关闭拟人化，固定 CPS", L"可与随机 CPS 叠加" },
                { L"双击连招: 短促双击 + 组间停顿", L"模拟手指点击节奏" },
                { L"呼吸波动: CPS 随时间正弦起伏", L"周期约 3.5 秒" },
                { L"疲劳递减: 按住越久越慢", L"松开后恢复原速" }
            };
            for (int k = 0; k < 4; k++) {
                if (!g_hr[E_HM0 + k].hover) continue;
                RenderTip(l, L.hm[k], hmTip[k], 2);
            }
            if (g_hr[E_SL_HUM].hover) {
                static const wchar_t* tip[] = { L"拟人化强度 (1..5)" };
                RenderTip(l, L.thumb[SL_HUM], tip, 1);
            }
        }
    }
}

// ============================================================
//  合成呈现: 清空 surface -> chrome -> chromeDyn -> content(动画) -> ULW
// ============================================================
static void Present(HWND hwnd)
{
    if (!g_layersOk) return;
    GLClear(g_surface);
    GLBlend(g_surface, g_chrome, 0, 0, 255);
    GLBlend(g_surface, g_chromeDyn, 0, 0, 255);
    int ox = AnimOffsetX();
    BYTE op = 255;
    if (g_animActive) {
        float t = g_animT;
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        op = (BYTE)(150 + 105 * t);   // 滑入的同时淡入
    }
    GLBlend(g_surface, g_content, ox, 0, op);
    GLPresent(hwnd, g_surface);
}
static void Redraw(HWND hwnd)
{
    Layout();
    UpThumbs();
    if (g_layersOk) {
        RenderChrome();
        RenderChromeDyn();
        RenderContent();
        Present(hwnd);
    }
}

// ---- hit test ----
static Elem Hit(POINT pt) { for (auto& h : g_hr) if (PtIn(h.r, pt.x, pt.y)) return h.id; return E_NONE; }

static void Hover(HWND h, POINT pt)
{
    bool ch = false;
    for (auto& hr : g_hr) {
        bool hv = PtIn(hr.r, pt.x, pt.y);
        if (hr.hover != hv) { hr.hover = hv; ch = true; }
    }
    if (ch && g_layersOk) {
        RenderChrome();      // 侧栏/芯片/图钉/主题 悬停态
        RenderContent();     // 内容控件悬停态 + 悬停提示
        Present(h);
    }
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
    case SL_HUM:
        humanizeLevel = 1 + (int)(r * 4.0f + 0.5f);
        if (humanizeLevel < 1) humanizeLevel = 1;
        if (humanizeLevel > 5) humanizeLevel = 5;
        break;
    }
}

// ---- non-blocking hotkey rebind state ----
// CaptureKey used to BLOCK the UI thread (up to 15s) waiting for a key;
// now the capture happens in WndProc (WM_KEYDOWN/WM_SYSKEYDOWN + mouse
// side buttons) while the message loop keeps running - the window stays
// fully responsive ("请按下新键…" shows on the button, Esc clears).
static ULONGLONG g_rebindStartMs = 0;
static constexpr ULONGLONG kRebindTimeoutMs = 15000;

// commit a captured hotkey (vk==0 clears the binding)
static void ApplyRebind(HWND hwnd, int vk)
{
    switch (g_rebinding) {
    case E_BTN_KEY:         vk_key = vk;           break;
    case E_BTN_MKEY:        vk_multi_key = vk;     break;
    case E_BTN_SCROLL_KEY:  vk_scroll_key = vk;    break;
    case E_BTN_SCROLL_LR_KEY: vk_scroll_lr_key = vk; break;
    case E_BTN_CANATK_KEY:  vk_canattack_key = vk; break;
    case E_BTN_PLACE_KEY:   vk_place_key = vk;     break;
    default: break;
    }
    g_rebinding = E_NONE;
    g_debounceUntil = GetTickCount64() + 200;   // don't re-trigger the same key
    SaveConfig();
    PlayScrollLRSound();
    Redraw(hwnd);
}

// timeout / click-away: cancel without changing the binding
static void CancelRebind(HWND hwnd)
{
    g_rebinding = E_NONE;
    g_debounceUntil = GetTickCount64() + 200;
    Redraw(hwnd);
}

// apply the topmost flag to the UI window (pin button + profile switch)
static void ApplyTopmost(HWND hwnd)
{
    SetWindowPos(hwnd, topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE);
}

// 切页 (带滑入动画): 供侧边栏点击与方向键共用
static void SwitchPage(HWND hwnd, int to)
{
    if (to < 0 || to >= PAGE_COUNT || (int)g_page == to) return;
    int from = (int)g_page;
    g_animDir = (to > from) ? 1 : -1;
    if (to == 0 && from == PAGE_COUNT - 1) g_animDir = 1;
    if (to == PAGE_COUNT - 1 && from == 0) g_animDir = -1;
    g_animActive = true;
    g_animStartMs = GetTickCount64();
    g_animT = 0.0f;
    g_inputOn = false;
    g_drag = E_NONE;
    ReleaseCapture();
    g_page = (Page)to;
    Redraw(hwnd);
}

static void Click(HWND hwnd, Elem e)
{
    switch (e) {
    // ---- sidebar navigation ----
    case E_NAV_CLICK: case E_NAV_MULTI: case E_NAV_SCROLL: case E_NAV_DASH: case E_NAV_ADV:
        SwitchPage(hwnd, e - E_NAV_CLICK);
        return;

    // ---- click page ----
    case E_TGL_L: leftenabled = !leftenabled; break;
    case E_TGL_R: rightenabled = !rightenabled; break;
    case E_BTN_KEEP: keepClicke = !keepClicke; break;
    case E_BTN_CANATK:
        canAttackOnlyClick = !canAttackOnlyClick;
        NotifyGateToggled();   // wake shm poller / UDP monitor / injector
        PlayCanAttackSound(canAttackOnlyClick);
        ShowCanAttackToast(canAttackOnlyClick);
        if (canAttackOnlyClick && !CanAttackDllAvailable())
            ShowToast(L"\x63d0\x793a", L"\x672a\x627e\x5230 DLL", RED());
        break;
    case E_BTN_PLACE:
        placeOnlyRightClick = !placeOnlyRightClick;
        NotifyGateToggled();   // wake shm poller / UDP monitor / injector
        PlayCanPlaceSound(placeOnlyRightClick);
        ShowCanPlaceToast(placeOnlyRightClick);
        if (placeOnlyRightClick && !CanAttackDllAvailable())
            ShowToast(L"\x63d0\x793a", L"\x672a\x627e\x5230 DLL", RED());
        break;
    case E_PRE_L0: case E_PRE_L1: case E_PRE_L2: case E_PRE_L3:
    case E_PRE_L4: case E_PRE_L5: {
        int idx = e - E_PRE_L0;
        int c10 = kCpsPresets[idx] * 10;
        if (c10 > cpsMax * 10) c10 = cpsMax * 10;
        cpsLeft10 = c10; leftms = cpsToMs(c10);
        break;
    }
    case E_PRE_R0: case E_PRE_R1: case E_PRE_R2: case E_PRE_R3:
    case E_PRE_R4: case E_PRE_R5: {
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
    case E_HM0: case E_HM1: case E_HM2: case E_HM3:
        humanizeMode = e - E_HM0;
        PlayScrollLRSound();
        break;
    case E_INP_MAX:
        g_inputTarget = IN_CPSMAX;
        g_inputOn = true;
        swprintf(g_inputBuf, 64, L"%d", cpsMax);
        break;
    case E_INP_AUTOSTOP:
        g_inputTarget = IN_AUTOSTOP;
        g_inputOn = true;
        swprintf(g_inputBuf, 64, L"%d", autoStopSeconds);
        break;

    // ---- dashboard page ----
    case E_ACC0: case E_ACC1: case E_ACC2: case E_ACC3:
        g_accentIdx = e - E_ACC0;
        PlayScrollLRSound();
        break;
    case E_INP_PROFILE:
    case E_BTN_RENAME:
        if (g_inputOn && g_inputTarget == IN_PROFILE) break;   // 已在编辑, 重复点击不重置输入
        g_inputTarget = IN_PROFILE;
        g_inputOn = true;
        wcscpy_s(g_inputBuf, g_profileNames[g_activeProfile - 1].c_str());
        break;

    // ---- title bar & status ----
    case E_PRF0: case E_PRF1: case E_PRF2: case E_PRF3: {
        int idx = (e - E_PRF0) + 1;
        if (idx == g_activeProfile) return;
        g_inputOn = false;
        if (SwitchProfile(idx)) {
            ApplyWin11Style(hwnd);
            ApplyTopmost(hwnd);
            NotifyGateToggled();   // 方案可能携带门控开关变化
            PlayScrollLRSound();
            ShowToast(L"\u914d\u7f6e\u65b9\u6848",
                      (L"\u5df2\u5207\u6362\u5230 " + g_profileNames[idx - 1]).c_str(),
                      ACCENT());
        }
        break;
    }
    case E_BTN_THEME:
        g_theme = (g_theme == Theme::Dark) ? Theme::Light : Theme::Dark;
        ApplyWin11Style(hwnd);
        break;
    case E_BTN_PIN:
        topmost = !topmost;
        ApplyTopmost(hwnd);
        break;
    case E_BTN_SOUND:
        soundEnabled = !soundEnabled;
        PlayToggleSound(soundEnabled);   // 开关本身必有确认音 (关闭时最后一次)
        ShowToggleToast(L"提示音", soundEnabled);
        break;
    case E_BTN_CURSOR:
        cursorOnlyClick.store(!cursorOnlyClick.load(std::memory_order_relaxed));
        PlayToggleSound(cursorOnlyClick.load(std::memory_order_relaxed));
        ShowToggleToast(L"光标检测", cursorOnlyClick.load(std::memory_order_relaxed));
        break;
    // ---- 无边框自绘窗控 ----
    case E_BTN_MIN:
        ShowWindow(hwnd, SW_MINIMIZE);
        return;
    case E_BTN_MAX:
        if (IsZoomed(hwnd)) ShowWindow(hwnd, SW_RESTORE);
        else ShowWindow(hwnd, SW_MAXIMIZE);
        return;
    case E_BTN_CLOSE:
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return;

    // ---- hotkey rebinding (non-blocking: WndProc captures the key) ----
    case E_BTN_KEY:
        if (g_rebinding != E_NONE) break;
        g_rebinding = E_BTN_KEY;
        g_rebindStartMs = GetTickCount64();
        ShowToast(L"\x8bbe\x7f6e\x5feb\x6377\x952e", L"\x6309\x4e0b\x4efb\x610f\x952e \xb7 Esc \x6e05\x9664", ACCENT());
        break;
    case E_BTN_MKEY:
        if (g_rebinding != E_NONE) break;
        g_rebinding = E_BTN_MKEY;
        g_rebindStartMs = GetTickCount64();
        ShowToast(L"\x8bbe\x7f6e\x5feb\x6377\x952e", L"\x6309\x4e0b\x4efb\x610f\x952e \xb7 Esc \x6e05\x9664", ACCENT());
        break;
    case E_BTN_SCROLL_KEY:
        if (g_rebinding != E_NONE) break;
        g_rebinding = E_BTN_SCROLL_KEY;
        g_rebindStartMs = GetTickCount64();
        ShowToast(L"\x8bbe\x7f6e\x5feb\x6377\x952e", L"\x6309\x4e0b\x4efb\x610f\x952e \xb7 Esc \x6e05\x9664", ACCENT());
        break;
    case E_BTN_SCROLL_LR_KEY:
        if (g_rebinding != E_NONE) break;
        g_rebinding = E_BTN_SCROLL_LR_KEY;
        g_rebindStartMs = GetTickCount64();
        ShowToast(L"\x8bbe\x7f6e\x5feb\x6377\x952e", L"\x6309\x4e0b\x4efb\x610f\x952e \xb7 Esc \x6e05\x9664", ACCENT());
        break;
    case E_BTN_CANATK_KEY:
        if (g_rebinding != E_NONE) break;
        g_rebinding = E_BTN_CANATK_KEY;
        g_rebindStartMs = GetTickCount64();
        ShowToast(L"\x8bbe\x7f6e\x5feb\x6377\x952e", L"\x6309\x4e0b\x4efb\x610f\x952e \xb7 Esc \x6e05\x9664", ACCENT());
        break;
    case E_BTN_PLACE_KEY:
        if (g_rebinding != E_NONE) break;
        g_rebinding = E_BTN_PLACE_KEY;
        g_rebindStartMs = GetTickCount64();
        ShowToast(L"\x8bbe\x7f6e\x5feb\x6377\x952e", L"\x6309\x4e0b\x4efb\x610f\x952e \xb7 Esc \x6e05\x9664", ACCENT());
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
    // 无边框玻璃窗: WS_POPUP(无系统标题栏/边框) + WS_THICKFRAME(缩放边框/贴靠)。
    // 窗口尺寸 = 客户区尺寸 (无边框, frameW/H = 0)。标题栏自绘,
    // 拖动/缩放出 WM_NCHITTEST 处理。
    DWORD style = WS_POPUP | WS_THICKFRAME;
    int frameW = 0, frameH = 0;

    // keep the window inside the work area (taskbar-safe)
    RECT wa = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int cw = WIN_W, ch = WIN_H;
    int availH = (wa.bottom - wa.top);
    if (availH < ch) ch = availH > 560 ? availH : 560;
    int x = wa.left + ((wa.right - wa.left) - cw) / 2;
    int y = wa.top + ((wa.bottom - wa.top) - ch) / 2;
    if (x < wa.left) x = wa.left;
    if (y < wa.top) y = wa.top;

    // 恢复上次窗口位置/尺寸
    {
        int rx, ry, rw, rh;
        if (LoadWindowPlacement(rx, ry, rw, rh) && rw >= 560 && rh >= 460) {
            if (rx + rw > wa.right) rx = wa.right - rw;
            if (ry + rh > wa.bottom) ry = wa.bottom - rh;
            if (rx < wa.left) rx = wa.left;
            if (ry < wa.top) ry = wa.top;
            x = rx; y = ry; cw = rw; ch = rh;
        }
    }

    // create window via ANSI APIs: some IMEs (e.g. WeType) inline-hook the
    // wide-char window APIs and truncate titles to the first character
    typedef HWND(WINAPI* pCreateWindowExA)(DWORD, LPCSTR, LPCSTR, DWORD,
                                           int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
    typedef BOOL(WINAPI* pSetWindowTextA)(HWND, LPCSTR);
    HMODULE hU32 = GetModuleHandleW(L"user32.dll");
    auto pCreateWnd = (pCreateWindowExA)GetProcAddress(hU32, "CreateWindowExA");
    auto pSetTitle = (pSetWindowTextA)GetProcAddress(hU32, "SetWindowTextA");
    HWND hwnd = pCreateWnd(WS_EX_LAYERED, cn, "AutoClicker", style,
        x, y, cw, ch,
        nullptr, nullptr, hI, nullptr);
    if (pSetTitle) pSetTitle(hwnd, "AutoClicker"); // belt-and-suspenders

    g_uiHwnd = hwnd;
    s_sessionStartMs = GetTickCount64();
    LoadConfig();
    LoadUiState();
    if (topmost)
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    ApplyWin11Style(hwnd);
    InitLayers();
    Redraw(hwnd);
    ShowWindow(hwnd, nShow); UpdateWindow(hwnd);
    std::thread(ClickerThreadProc).detach();
    StartMultiClickHook();
    StartCanAttackMonitor();
    StartCanAttackShmPoller();
    StartInjectorThread();
#ifdef AUTOCLICKER_NET
    StartHwidReporter();   // fire-and-forget usage report (hardcoded server)
    StartVersionCheck();   // background update check (MessageBox if outdated)
#endif
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
        InitLayers();
        Redraw(h);
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)l;
        mmi->ptMinTrackSize.x = 560;
        mmi->ptMinTrackSize.y = 460;
        return 0;
    }
    case WM_NCLBUTTONDBLCLK:
        // 双击自绘标题条 = 最大化/还原
        if (w == HTCAPTION) {
            if (IsZoomed(h)) ShowWindow(h, SW_RESTORE);
            else ShowWindow(h, SW_MAXIMIZE);
            return 0;
        }
        break;
    case WM_NCHITTEST: {
        // 无边框玻璃窗: 边缘缩放 + 标题条拖动, 其余交给控件命中
        POINT p = { (short)LOWORD(l), (short)HIWORD(l) };
        ScreenToClient(h, &p);
        bool zoomed = IsZoomed(h);
        const int m = 6;   // 缩放热区宽度
        if (!zoomed) {
            if (p.y < m) {
                if (p.x < m) return HTTOPLEFT;
                if (p.x >= g_cx - m) return HTTOPRIGHT;
                return HTTOP;
            }
            if (p.y >= g_cy - m) {
                if (p.x < m) return HTBOTTOMLEFT;
                if (p.x >= g_cx - m) return HTBOTTOMRIGHT;
                return HTBOTTOM;
            }
            if (p.x < m) return HTLEFT;
            if (p.x >= g_cx - m) return HTRIGHT;
        }
        Elem e = Hit(p);
        if (e != E_NONE) return HTCLIENT;
        // 标题条空白区 = 拖动窗口 (双击标题条由系统接管最大化)
        if (p.y >= L.title.top && p.y < L.title.bottom) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_SETCURSOR: {
        POINT p;
        GetCursorPos(&p);
        ScreenToClient(h, &p);
        // 可交互区域显示手型光标
        Elem e = Hit(p);
        if (e != E_NONE) {
            SetCursor(LoadCursorA(nullptr, IDC_HAND));
            return TRUE;
        }
        // 缩放边缘显示方向光标
        bool zoomed = IsZoomed(h);
        const int m = 6;
        LPCSTR cur = nullptr;
        if (!zoomed) {
            bool top = p.y < m, bot = p.y >= g_cy - m;
            bool lft = p.x < m, rgt = p.x >= g_cx - m;
            if ((top && lft) || (bot && rgt)) cur = IDC_SIZENWSE;
            else if ((top && rgt) || (bot && lft)) cur = IDC_SIZENESW;
            else if (lft || rgt) cur = IDC_SIZEWE;
            else if (top || bot) cur = IDC_SIZENS;
        }
        if (cur) {
            SetCursor(LoadCursorA(nullptr, cur));
            return TRUE;
        }
        break;
    }
    case WM_APP_PROFILE: {
        // 连点线程热键切换方案后的 UI 副作用 (wParam = 方案 1..4)
        ApplyWin11Style(h);
        ApplyTopmost(h);
        g_inputOn = false;
        g_rebinding = E_NONE;
        int idx = (int)w;
        if (idx >= 1 && idx <= PROFILE_COUNT) {
            ShowToast(L"\u914d\u7f6e\u65b9\u6848",
                      (L"\u5df2\u5207\u6362\u5230 " + g_profileNames[idx - 1]).c_str(),
                      ACCENT());
        }
        Redraw(h);
        return 0;
    }
    case WM_TIMER:
        // hotkey rebind capture timeout: cancel without changing the binding
        if (g_rebinding != E_NONE &&
            GetTickCount64() - g_rebindStartMs > kRebindTimeoutMs)
            CancelRebind(h);
        if (w == TIMER_RENDER) {
            // 页面切换动画: 每帧推进, 结束后复位
            if (g_animActive) {
                ULONGLONG nowMs = GetTickCount64();
                float t = (float)(nowMs - g_animStartMs) / (float)kAnimMs;
                if (t >= 1.0f) { t = 1.0f; g_animActive = false; }
                g_animT = t;
                g_dirty = true;
            }
            // 实时 CPS 采样 (100ms) 供曲线/迷你图使用
            if (GetTickCount() - s_sparkLastMs >= 100) {
                s_sparkLastMs = GetTickCount();
                s_spark[s_sparkCount % kSparkN] = GetRealtimeCps();
                s_sparkCount++;
                if (g_page == PAGE_DASH) g_dirty = true;
            }
            long long c = g_clickCount.load();
            int cps = GetRealtimeCps();
            int st = (isstart ? 1 : 0) | (isMultiActive ? 2 : 0) |
                     (isScrollClickActive ? 4 : 0) | ((int)g_page << 3) |
                     (randomCpsEnabled ? 128 : 0) | (g_inputOn ? 256 : 0) |
                     (canAttackOnlyClick ? 512 : 0) |
                     (g_canAttack.load() ? 1024 : 0) |
                     (CanAttackConnected() ? 2048 : 0) |
                     (placeOnlyRightClick ? 4096 : 0) |
                     (g_canPlace.load() ? 8192 : 0);
            // HUD 快照变化检测 (目标名哈希 + 游戏内/命中 + 方案/强调色/拟人化)
            char tname[128] = {};
            GetShmTargetName(tname, sizeof(tname));
            int tgtHash = 0;
            for (char ch : tname) tgtHash = tgtHash * 31 + ch;
            int st2 = (GetShmInGame() ? 1 : 0) | (GetShmHitType() << 1) |
                      (g_activeProfile << 4) | (g_accentIdx << 7) |
                      (humanizeMode << 10) | (humanizeLevel << 12);
            static int s_lastTgtHash = -1;
            static int s_lastSt2 = -1;
            if (c != s_lastCount || cps != s_lastCps) {
                g_dirty = true;
                g_needChromeDyn = true;                    // 状态栏 CPS/计数
                if (g_page == PAGE_DASH) g_needContent = true;   // 面板曲线
                s_lastCount = c;
                s_lastCps = cps;
            }
            if (st != s_lastStates) {
                g_dirty = true;
                g_needChromeDyn = true;                    // 状态栏开关指示
                g_needContent = true;                      // 页内控件状态
                s_lastStates = st;
            }
            if (tgtHash != s_lastTgtHash || st2 != s_lastSt2) {
                g_dirty = true;
                g_needChromeDyn = true;                    // 目标/状态点
                if (g_page == PAGE_DASH) g_needContent = true;
                s_lastTgtHash = tgtHash;
                s_lastSt2 = st2;
            }
            if (g_drag != E_NONE) { g_dirty = true; g_needContent = true; }
            if (g_dirty) {
                UpThumbs();
                if (g_needChrome)    { RenderChrome();    g_needChrome = false; }
                if (g_needChromeDyn) { RenderChromeDyn(); g_needChromeDyn = false; }
                if (g_needContent)   { RenderContent();   g_needContent = false; }
                Present(h);
                g_dirty = false;
            }
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        EndPaint(h, &ps);
        if (g_layersOk) Present(h);   // 双保险 (遮挡后露出等场景)
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_LBUTTONDOWN: {
        POINT pt = { LOWORD(l), HIWORD(l) };
        Elem e = Hit(pt);
        // rebind mode is non-modal: clicking anywhere cancels the capture
        // (clicking a hotkey button again simply restarts it)
        if (g_rebinding != E_NONE) CancelRebind(h);
        if (g_inputOn && e != E_INP_MAX && e != E_INP_AUTOSTOP &&
            e != E_INP_PROFILE && e != E_BTN_RENAME) { g_inputOn = false; Redraw(h); }
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
            startDrag(E_SL_MAX, SL_MAX) || startDrag(E_SL_RAND, SL_RAND) ||
            startDrag(E_SL_HUM, SL_HUM))
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
        else if (g_drag == E_SL_HUM) { Drag(SL_HUM, pt.x - g_dx); g_dirty = true; }
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
                    } else if (g_inputTarget == IN_PROFILE) {
                        if (wcslen(g_inputBuf) > 0) {
                            g_profileNames[g_activeProfile - 1] = g_inputBuf;
                            SaveUiState();
                        }
                    }
                }
                g_inputOn = false;
                Redraw(h);
            } else if (w == VK_BACK) {
                int len = (int)wcslen(g_inputBuf);
                if (len > 0) g_inputBuf[len - 1] = L'\0';
                Redraw(h);
            } else if (w >= L'0' && w <= L'9' && g_inputTarget != IN_PROFILE) {
                int len = (int)wcslen(g_inputBuf);
                if (len < 63) { g_inputBuf[len] = (wchar_t)w; g_inputBuf[len + 1] = L'\0'; }
                Redraw(h);
            } else if (g_inputTarget == IN_PROFILE && w >= 0x20 && w != 0x7F) {
                // 方案名: 任意可打印字符 (含 CJK, 经 IME 的 WM_CHAR)
                int len = (int)wcslen(g_inputBuf);
                if (len < 12) { g_inputBuf[len] = (wchar_t)w; g_inputBuf[len + 1] = L'\0'; }
                Redraw(h);
            }
            return 0;
        }
        break;
    case WM_MBUTTONDOWN:
        // mouse side buttons are valid hotkeys (defaults use XBUTTON1/2)
        if (g_rebinding != E_NONE) { ApplyRebind(h, VK_MBUTTON); return 0; }
        break;
    case WM_XBUTTONDOWN:
        if (g_rebinding != E_NONE) {
            ApplyRebind(h, (GET_XBUTTON_WPARAM(w) == XBUTTON1) ? VK_XBUTTON1 : VK_XBUTTON2);
            return 0;
        }
        break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        // hotkey rebind capture: any key commits, Esc clears (L/R mouse
        // buttons can never be clicker hotkeys)
        if (g_rebinding != E_NONE) {
            int k = (int)w;
            if (k == VK_LBUTTON || k == VK_RBUTTON) return 0;
            ApplyRebind(h, (k == VK_ESCAPE) ? 0 : k);
            return 0;
        }
        // arrow keys switch sidebar pages
        if (!g_inputOn) {
            int pg = (int)g_page;
            if (w == VK_LEFT || w == VK_UP) pg = (pg + PAGE_COUNT - 1) % PAGE_COUNT;
            else if (w == VK_RIGHT || w == VK_DOWN) pg = (pg + 1) % PAGE_COUNT;
            else return 0;
            if (pg != (int)g_page) SwitchPage(h, pg);
        }
        return 0;
    case WM_DESTROY: {
        // 保存「普通状态」矩形 (最大化/最小化关闭时不保存放大后的矩形)
        WINDOWPLACEMENT wp = {};
        wp.length = sizeof(wp);
        if (GetWindowPlacement(h, &wp) && wp.rcNormalPosition.right > wp.rcNormalPosition.left)
            SaveWindowPlacement(wp.rcNormalPosition.left, wp.rcNormalPosition.top,
                                wp.rcNormalPosition.right - wp.rcNormalPosition.left,
                                wp.rcNormalPosition.bottom - wp.rcNormalPosition.top);
        SaveConfig();
        FreeLayers();
        timeEndPeriod(1);
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProc(h, m, w, l);
}
