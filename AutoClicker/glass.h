#pragma once

#include <Windows.h>

// ============================================================
//  玻璃态渲染内核 (glassmorphism render core)
// ============================================================
// 整窗分层透明渲染。每层 = 32bpp top-down DIB，像素为「预乘 Alpha」
// (premultiplied)，可直接作为 AlphaBlend / UpdateLayeredWindow 的
// AC_SRC_ALPHA 源。
//
// 分层策略 (main.cpp):
//   g_chrome  全窗层: 基底玻璃 + 标题栏/侧栏/状态栏 (变化少)
//   g_content 全窗层: 当前页内容 (页切换动画只移动这一层)
//   g_surface 合成层: 每次 Present = 清空 + 混合两层 + ULW 呈现
//
// 半透明图形 (圆角填充/渐变/发丝描边/软阴影) 全部软件光栅化到像素
// (带 1px 边缘抗锯齿); GDI 文字/图标/曲线画进层后做二进制 alpha 提升
// (GLLiftAlpha)。文字必须使用灰度抗锯齿字体 + TRANSPARENT 背景模式。

struct GLayer {
    HDC      dc;       // 选中 DIB 的兼容 DC (可直接用 GDI 画)
    HBITMAP  bmp;
    BYTE*    bits;     // BGRA 像素基址 (预乘 alpha)
    int      w, h;
};

// 创建/销毁 32bpp top-down DIB 层 (失败时 dc=nullptr)
GLayer GLCreate(int w, int h);
void   GLFree(GLayer& l);
void   GLClear(GLayer& l);                      // 全层清零 (RGB+alpha)

// ---- 软件填充 (预乘 alpha, src-over, 1px 边缘抗锯齿) ----
void GLFillRound(GLayer& l, const RECT& r, int rad, COLORREF c, BYTE a);
void GLFillV(GLayer& l, const RECT& r, int rad,
             COLORREF cTop, BYTE aTop, COLORREF cBot, BYTE aBot);   // 圆角垂直渐变
void GLRing(GLayer& l, const RECT& r, int rad, int thickness, COLORREF c, BYTE a); // 圆角描边
void GLShadow(GLayer& l, const RECT& r, int rad, int depth, COLORREF c, BYTE maxA); // 同心软阴影
void GLHLine(GLayer& l, int x0, int x1, int y, COLORREF c, BYTE a);

// ---- GDI 绘制进层后提升 alpha (只在 alpha==0 且 RGB!=0 的像素上) ----
// 注意: 画进层的颜色不能是纯黑 RGB(0,0,0), 玻璃色板均为近黑, 天然满足。
void GLLiftAlpha(GLayer& l);                    // 全层提升
void GLLiftAlphaRect(GLayer& l, const RECT& r); // 区域提升 (更快)

// ---- 合成 ----
// dst 上按 (x,y) 偏移、opacity(0..255) 全局不透明度叠加 src 层
void GLBlend(GLayer& dst, const GLayer& src, int x, int y, BYTE opacity);

// 把 surface 层呈现到 hwnd (UpdateLayeredWindow, 保持窗口当前位置)
void GLPresent(HWND hwnd, const GLayer& surface);
