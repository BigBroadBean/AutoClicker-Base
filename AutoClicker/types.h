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

// ---- Dark theme (Neumorphism) ----
// 新拟态：表面与背景同色，靠"暗影 + 高光"双阴影表现浮雕
constexpr COLORREF CLR_BG_DARK        = RGB(43, 44, 52);
constexpr COLORREF CLR_CARD_DARK      = RGB(43, 44, 52);
constexpr COLORREF CLR_SHADOW_DK_DARK = RGB(27, 28, 34);   // 右下暗影
constexpr COLORREF CLR_SHADOW_LT_DARK = RGB(60, 62, 72);   // 左上高光
constexpr COLORREF CLR_ACCENT_DARK    = RGB(92, 156, 255);
constexpr COLORREF CLR_TEXT_DARK      = RGB(226, 228, 236);
constexpr COLORREF CLR_TEXT_DIM_DARK  = RGB(146, 148, 158);
constexpr COLORREF CLR_BTN_DARK       = RGB(43, 44, 52);
constexpr COLORREF CLR_BTN_HOVER_DARK = RGB(52, 54, 63);
constexpr COLORREF CLR_GREEN_DARK     = RGB(88, 200, 132);
constexpr COLORREF CLR_RED_DARK       = RGB(236, 92, 100);
constexpr COLORREF CLR_TRACK_DARK     = RGB(43, 44, 52);

// ---- Light theme (Neumorphism) ----
constexpr COLORREF CLR_BG_LIGHT        = RGB(228, 232, 240);
constexpr COLORREF CLR_CARD_LIGHT      = RGB(228, 232, 240);
constexpr COLORREF CLR_SHADOW_DK_LIGHT = RGB(197, 202, 212);
constexpr COLORREF CLR_SHADOW_LT_LIGHT = RGB(255, 255, 255);
constexpr COLORREF CLR_ACCENT_LIGHT    = RGB(56, 132, 255);
constexpr COLORREF CLR_TEXT_LIGHT      = RGB(46, 50, 60);
constexpr COLORREF CLR_TEXT_DIM_LIGHT  = RGB(126, 132, 144);
constexpr COLORREF CLR_BTN_LIGHT       = RGB(228, 232, 240);
constexpr COLORREF CLR_BTN_HOVER_LIGHT = RGB(220, 225, 234);
constexpr COLORREF CLR_GREEN_LIGHT     = RGB(46, 168, 104);
constexpr COLORREF CLR_RED_LIGHT       = RGB(216, 76, 88);
constexpr COLORREF CLR_TRACK_LIGHT     = RGB(228, 232, 240);

// ---- Theme enum ----
enum class Theme : int { Dark = 0, Light = 1 };
extern Theme g_theme;

// CJK-capable UI font picked at startup (Noto Sans SC > YaHei, etc.)
extern const wchar_t* g_uiFontName;

// ---- Theme-aware color accessors ----
inline COLORREF BG()         { return g_theme == Theme::Dark ? CLR_BG_DARK        : CLR_BG_LIGHT; }
inline COLORREF CARD()       { return g_theme == Theme::Dark ? CLR_CARD_DARK      : CLR_CARD_LIGHT; }
inline COLORREF SHADOW_DARK(){ return g_theme == Theme::Dark ? CLR_SHADOW_DK_DARK : CLR_SHADOW_DK_LIGHT; }
inline COLORREF SHADOW_LIGHT(){ return g_theme == Theme::Dark ? CLR_SHADOW_LT_DARK : CLR_SHADOW_LT_LIGHT; }
inline COLORREF BORDER()     { return g_theme == Theme::Dark ? CLR_SHADOW_DK_DARK : CLR_SHADOW_DK_LIGHT; }
inline COLORREF ACCENT()     { return g_theme == Theme::Dark ? CLR_ACCENT_DARK    : CLR_ACCENT_LIGHT; }
inline COLORREF TXT()        { return g_theme == Theme::Dark ? CLR_TEXT_DARK      : CLR_TEXT_LIGHT; }
inline COLORREF TXT_DIM()    { return g_theme == Theme::Dark ? CLR_TEXT_DIM_DARK  : CLR_TEXT_DIM_LIGHT; }
inline COLORREF BTN()        { return g_theme == Theme::Dark ? CLR_BTN_DARK       : CLR_BTN_LIGHT; }
inline COLORREF BTN_HOVER()  { return g_theme == Theme::Dark ? CLR_BTN_HOVER_DARK : CLR_BTN_HOVER_LIGHT; }
inline COLORREF GREEN()      { return g_theme == Theme::Dark ? CLR_GREEN_DARK     : CLR_GREEN_LIGHT; }
inline COLORREF RED()        { return g_theme == Theme::Dark ? CLR_RED_DARK       : CLR_RED_LIGHT; }
inline COLORREF TRACK()      { return g_theme == Theme::Dark ? CLR_TRACK_DARK     : CLR_TRACK_LIGHT; }

constexpr int DWMA_DARK     = 20;
constexpr int DWMA_CORNER   = 33;
constexpr int DWMA_BACKDROP = 38;
constexpr int CORNER_ROUND  = 2;
constexpr int BACKDROP_MAIN = 2;

// ---- version (single source of truth) ----
// 当前版本号（Net / Base 双产品线共用同一版本号，发新版只改这里；
// main.cpp UI 显示、update.cpp 版本比较、httputil.cpp User-Agent 均引用）
inline constexpr const char*    APP_VERSION   = "2.4";
inline constexpr const wchar_t* APP_VERSION_W = L"2.4";
