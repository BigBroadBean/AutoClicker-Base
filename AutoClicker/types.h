#pragma once

#include <Windows.h>

constexpr int WIN_W = 640;
constexpr int WIN_H = 480;

constexpr int CPS_MIN10 = 5;
constexpr int CPS_MAX10 = 1000;

constexpr int CPS_LIMIT_MIN = 20;
constexpr int CPS_LIMIT_MAX = 500;
constexpr int CPS_LIMIT_DEFAULT = 50;

constexpr int TIMER_RENDER = 1;

// ============================================================
//  玻璃态 (Glassmorphism) 色板
// ============================================================
// 整窗为分层透明窗口: 底色带 alpha 透出桌面, 面板/卡片为叠加的
// 半透明玻璃层 + 发丝描边。此处只定义 RGB; 各层透明度由渲染代码
// 按设计指定 (见 main.cpp / overlay.cpp 的 alpha 常量)。

// ---- 深空玻璃 (dark) ----
constexpr COLORREF CLR_BG_TOP_DARK     = RGB(18, 21, 32);     // 窗口基底渐变顶部
constexpr COLORREF CLR_BG_BOT_DARK     = RGB(10, 12, 21);     // 窗口基底渐变底部
constexpr COLORREF CLR_PANEL_DARK      = RGB(30, 36, 56);     // 面板玻璃
constexpr COLORREF CLR_CARD_DARK       = RGB(38, 46, 72);     // 卡片玻璃 (略亮一档)
constexpr COLORREF CLR_HAIRLINE_DARK   = RGB(255, 255, 255);  // 发丝描边 (低 alpha)
constexpr COLORREF CLR_HOVER_DARK      = RGB(255, 255, 255);  // 悬停填充 (低 alpha)
constexpr COLORREF CLR_TEXT_DARK       = RGB(237, 240, 248);
constexpr COLORREF CLR_TEXT_DIM_DARK   = RGB(151, 160, 184);
constexpr COLORREF CLR_TRACK_DARK      = RGB(255, 255, 255);  // 轨道/内凹 (低 alpha)
constexpr COLORREF CLR_GREEN_DARK      = RGB(84, 206, 140);
constexpr COLORREF CLR_RED_DARK        = RGB(240, 94, 108);
constexpr COLORREF CLR_SHEEN_DARK      = RGB(255, 255, 255);  // 玻璃顶部高光 (低 alpha)

// ---- 雾白玻璃 (light) ----
constexpr COLORREF CLR_BG_TOP_LIGHT    = RGB(243, 246, 251);
constexpr COLORREF CLR_BG_BOT_LIGHT    = RGB(231, 236, 244);
constexpr COLORREF CLR_PANEL_LIGHT     = RGB(255, 255, 255);
constexpr COLORREF CLR_CARD_LIGHT      = RGB(255, 255, 255);
constexpr COLORREF CLR_HAIRLINE_LIGHT  = RGB(28, 38, 62);
constexpr COLORREF CLR_HOVER_LIGHT     = RGB(28, 38, 62);
constexpr COLORREF CLR_TEXT_LIGHT      = RGB(30, 36, 54);
constexpr COLORREF CLR_TEXT_DIM_LIGHT  = RGB(106, 118, 142);
constexpr COLORREF CLR_TRACK_LIGHT     = RGB(28, 38, 62);
constexpr COLORREF CLR_GREEN_LIGHT     = RGB(34, 164, 104);
constexpr COLORREF CLR_RED_LIGHT       = RGB(214, 70, 88);
constexpr COLORREF CLR_SHEEN_LIGHT     = RGB(255, 255, 255);

// ---- 强调色 4 选 1 ----
constexpr int ACCENT_COUNT = 4;
extern int g_accentIdx;
constexpr COLORREF CLR_ACCENT_DARK[4]  = { RGB(99, 166, 255), RGB(170, 138, 255), RGB(84, 206, 140),  RGB(255, 173, 92) };
constexpr COLORREF CLR_ACCENT_LIGHT[4] = { RGB(45, 112, 244), RGB(124, 82, 246),  RGB(28, 158, 98),   RGB(232, 124, 28) };

// ---- Theme enum ----
enum class Theme : int { Dark = 0, Light = 1 };
extern Theme g_theme;

// CJK-capable UI font picked at startup (Noto Sans SC > YaHei, etc.)
extern const wchar_t* g_uiFontName;

// ---- Theme-aware color accessors ----
inline COLORREF BGTOP()      { return g_theme == Theme::Dark ? CLR_BG_TOP_DARK     : CLR_BG_TOP_LIGHT; }
inline COLORREF BGBOT()      { return g_theme == Theme::Dark ? CLR_BG_BOT_DARK     : CLR_BG_BOT_LIGHT; }
inline COLORREF PANEL()      { return g_theme == Theme::Dark ? CLR_PANEL_DARK      : CLR_PANEL_LIGHT; }
inline COLORREF CARD()       { return g_theme == Theme::Dark ? CLR_CARD_DARK       : CLR_CARD_LIGHT; }
inline COLORREF HAIRLINE()   { return g_theme == Theme::Dark ? CLR_HAIRLINE_DARK   : CLR_HAIRLINE_LIGHT; }
inline COLORREF HOVER()      { return g_theme == Theme::Dark ? CLR_HOVER_DARK      : CLR_HOVER_LIGHT; }
inline COLORREF SHEEN()      { return g_theme == Theme::Dark ? CLR_SHEEN_DARK      : CLR_SHEEN_LIGHT; }
inline COLORREF TRACK()      { return g_theme == Theme::Dark ? CLR_TRACK_DARK      : CLR_TRACK_LIGHT; }
inline COLORREF TXT()        { return g_theme == Theme::Dark ? CLR_TEXT_DARK       : CLR_TEXT_LIGHT; }
inline COLORREF TXT_DIM()    { return g_theme == Theme::Dark ? CLR_TEXT_DIM_DARK   : CLR_TEXT_DIM_LIGHT; }
inline COLORREF GREEN()      { return g_theme == Theme::Dark ? CLR_GREEN_DARK      : CLR_GREEN_LIGHT; }
inline COLORREF RED()        { return g_theme == Theme::Dark ? CLR_RED_DARK        : CLR_RED_LIGHT; }
inline COLORREF ACCENT()     {
    int i = g_accentIdx;
    if (i < 0 || i >= ACCENT_COUNT) i = 0;
    return g_theme == Theme::Dark ? CLR_ACCENT_DARK[i] : CLR_ACCENT_LIGHT[i];
}
inline COLORREF ACCENT_RAW(int i) {
    if (i < 0 || i >= ACCENT_COUNT) i = 0;
    return g_theme == Theme::Dark ? CLR_ACCENT_DARK[i] : CLR_ACCENT_LIGHT[i];
}

// 兼容旧代码的别名 (新渲染不再使用新拟态, 但保留引用点)
inline COLORREF BG()         { return g_theme == Theme::Dark ? CLR_BG_BOT_DARK     : CLR_BG_BOT_LIGHT; }
inline COLORREF SHADOW_DARK(){ return RGB(0, 0, 0); }
inline COLORREF SHADOW_LIGHT(){ return RGB(255, 255, 255); }
inline COLORREF BORDER()     { return HAIRLINE(); }
inline COLORREF BTN()        { return PANEL(); }
inline COLORREF BTN_HOVER()  { return HOVER(); }

constexpr int DWMA_DARK     = 20;
constexpr int DWMA_CORNER   = 33;
constexpr int DWMA_BACKDROP = 38;
constexpr int CORNER_ROUND  = 2;
constexpr int CORNER_DONOTROUND = 1;
constexpr int BACKDROP_MAIN = 2;

// ---- version (single source of truth) ----
// 当前版本号（Net / Base 双产品线共用同一版本号，发新版只改这里；
// main.cpp UI 显示、update.cpp 版本比较、httputil.cpp User-Agent 均引用）
inline constexpr const char*    APP_VERSION   = "2.8";
inline constexpr const wchar_t* APP_VERSION_W = L"2.8";
