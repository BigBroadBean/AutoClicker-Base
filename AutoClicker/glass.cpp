#include "glass.h"
#include <cmath>

#pragma comment(lib, "msimg32.lib")   // AlphaBlend

// ============================================================
//  玻璃态渲染内核实现 (行级优化)
// ============================================================
// 所有软件填充均为「逐行解析跨度」: 中间像素走快路径 (预乘 src-over),
// 仅左右边界 2 个像素做 1px 抗锯齿覆盖率计算。形状坐标全部钳制在
// 层内 (阴影会外扩到层外, 越界部分直接丢弃)。

GLayer GLCreate(int w, int h)
{
    GLayer l = {};
    if (w <= 0 || h <= 0) return l;
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;   // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) {
        if (bmp) DeleteObject(bmp);
        return l;
    }
    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) {
        DeleteObject(bmp);
        return l;
    }
    SelectObject(dc, bmp);
    l.dc = dc;
    l.bmp = bmp;
    l.bits = (BYTE*)bits;
    l.w = w;
    l.h = h;
    return l;
}

void GLFree(GLayer& l)
{
    if (l.dc) DeleteDC(l.dc);
    if (l.bmp) DeleteObject(l.bmp);
    l = {};
}

void GLClear(GLayer& l)
{
    if (l.bits) memset(l.bits, 0, (size_t)l.w * l.h * 4);
}

// ---- 像素级 src-over (src 已预乘) ----
// 参数 sr/sg/sb = R/G/B 通道值; 像素内存布局为 BGRA (p[0]=B, p[2]=R)
static inline void BlendPx(BYTE* p, int sr, int sg, int sb, int sa)
{
    if (sa <= 0) return;
    int da = p[3];
    if (da == 0) {
        p[0] = (BYTE)sb; p[1] = (BYTE)sg; p[2] = (BYTE)sr; p[3] = (BYTE)sa;
        return;
    }
    int inv = 255 - sa;
    int outA = sa + da * inv / 255;
    if (outA <= 0) { p[0] = p[1] = p[2] = p[3] = 0; return; }
    p[0] = (BYTE)(sb + p[0] * inv / 255);
    p[1] = (BYTE)(sg + p[1] * inv / 255);
    p[2] = (BYTE)(sr + p[2] * inv / 255);
    p[3] = (BYTE)outA;
}

// 行内圆角水平内缩量 (返回该行因圆角收缩的 inset; 整行在外时返回 <0)
static inline float RowInset(int y, float cy, float hh, float rr)
{
    float ty = fabsf((float)y - cy) - (hh - rr);
    if (ty <= 0) return 0;                 // 直边区
    if (ty >= rr) return -1;               // 整行在圆角外
    return rr - sqrtf(rr * rr - ty * ty);
}

// 边界像素覆盖率 (1px 抗锯齿): cov = clamp(0.5 + (x - xs))
static inline float EdgeCov(float x, float xs)
{
    float c = 0.5f + (x - xs);
    return c < 0 ? 0 : (c > 1 ? 1 : c);
}

void GLFillRound(GLayer& l, const RECT& r, int rad, COLORREF c, BYTE a)
{
    if (!l.bits || a == 0) return;
    int x0 = r.left, y0 = r.top, x1 = r.right, y1 = r.bottom;
    if (x1 <= x0) return;
    int cy0 = y0; if (cy0 < 0) cy0 = 0;
    int cy1 = y1; if (cy1 > l.h) cy1 = l.h;
    if (cy1 <= cy0) return;
    float cx = (x0 + x1 - 1) * 0.5f;
    float cy = (y0 + y1 - 1) * 0.5f;
    float hw = (x1 - x0) * 0.5f;
    float hh = (y1 - y0) * 0.5f;
    float rr = (float)rad;
    if (rr > hw) rr = hw;
    if (rr > hh) rr = hh;
    if (rr < 0) rr = 0;
    int cr = GetRValue(c), cg = GetGValue(c), cb = GetBValue(c);
    int pr = cr * a / 255, pg = cg * a / 255, pb = cb * a / 255;

    for (int y = cy0; y < cy1; y++) {
        float inset = RowInset(y, cy, hh, rr);
        if (inset < 0) continue;
        float xs = cx - hw + inset;
        float xe = cx + hw - inset;
        int is = (int)ceilf(xs);  if (is < 0) is = 0;
        int ie = (int)floorf(xe); if (ie > l.w) ie = l.w;
        BYTE* row = l.bits + ((size_t)y * l.w + is) * 4;
        for (int x = is; x < ie; x++, row += 4)
            BlendPx(row, pr, pg, pb, a);
        // 左右边缘 AA
        for (int k = 0; k < 2; k++) {
            int x = k == 0 ? is - 1 : ie;
            if (x < 0 || x >= l.w) continue;
            float cov;
            if (k == 0) cov = EdgeCov((float)x, xs);
            else {
                float cc = 0.5f + (xe - (float)x - 0.5f);
                cov = cc < 0 ? 0 : (cc > 1 ? 1 : cc);
            }
            if (cov <= 0) continue;
            int sa = (int)(a * cov + 0.5f);
            if (sa <= 0) continue;
            BlendPx(l.bits + ((size_t)y * l.w + x) * 4,
                    cr * sa / 255, cg * sa / 255, cb * sa / 255, sa);
        }
    }
}

void GLFillV(GLayer& l, const RECT& r, int rad,
             COLORREF cTop, BYTE aTop, COLORREF cBot, BYTE aBot)
{
    if (!l.bits) return;
    int x0 = r.left, y0 = r.top, x1 = r.right, y1 = r.bottom;
    if (x1 <= x0) return;
    int cy0 = y0; if (cy0 < 0) cy0 = 0;
    int cy1 = y1; if (cy1 > l.h) cy1 = l.h;
    if (cy1 <= cy0) return;
    float cx = (x0 + x1 - 1) * 0.5f;
    float cy = (y0 + y1 - 1) * 0.5f;
    float hw = (x1 - x0) * 0.5f;
    float hh = (y1 - y0) * 0.5f;
    float rr = (float)rad;
    if (rr > hw) rr = hw;
    if (rr > hh) rr = hh;
    if (rr < 0) rr = 0;
    float span = (float)(y1 - y0 - 1);
    if (span <= 0) span = 1;

    for (int y = cy0; y < cy1; y++) {
        float t = (float)(y - y0) / span;
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        int cr = (int)(GetRValue(cTop) + (GetRValue(cBot) - GetRValue(cTop)) * t);
        int cg = (int)(GetGValue(cTop) + (GetGValue(cBot) - GetGValue(cTop)) * t);
        int cb = (int)(GetBValue(cTop) + (GetBValue(cBot) - GetBValue(cTop)) * t);
        int a = (int)(aTop + (aBot - aTop) * t);
        if (a <= 0) continue;
        float inset = RowInset(y, cy, hh, rr);
        if (inset < 0) continue;
        float xs = cx - hw + inset;
        float xe = cx + hw - inset;
        int is = (int)ceilf(xs);  if (is < 0) is = 0;
        int ie = (int)floorf(xe); if (ie > l.w) ie = l.w;
        BYTE* row = l.bits + ((size_t)y * l.w + is) * 4;
        int pr = cr * a / 255, pg = cg * a / 255, pb = cb * a / 255;
        for (int x = is; x < ie; x++, row += 4)
            BlendPx(row, pr, pg, pb, a);
        for (int k = 0; k < 2; k++) {
            int x = k == 0 ? is - 1 : ie;
            if (x < 0 || x >= l.w) continue;
            float cov;
            if (k == 0) cov = EdgeCov((float)x, xs);
            else {
                float cc = 0.5f + (xe - (float)x - 0.5f);
                cov = cc < 0 ? 0 : (cc > 1 ? 1 : cc);
            }
            if (cov <= 0) continue;
            int sa = (int)(a * cov + 0.5f);
            if (sa <= 0) continue;
            BlendPx(l.bits + ((size_t)y * l.w + x) * 4,
                    cr * sa / 255, cg * sa / 255, cb * sa / 255, sa);
        }
    }
}

void GLRing(GLayer& l, const RECT& r, int rad, int thickness, COLORREF c, BYTE a)
{
    // 只描边带 (外侧覆盖率 减 内侧覆盖率), 绝不触碰带内已有内容。
    // 逐行遍历形状横向跨度, 覆盖四边 (含圆角弧与直边)。
    if (!l.bits || thickness <= 0 || a == 0) return;
    int x0 = r.left, y0 = r.top, x1 = r.right, y1 = r.bottom;
    if (x1 <= x0) return;
    int cy0 = y0; if (cy0 < 0) cy0 = 0;
    int cy1 = y1; if (cy1 > l.h) cy1 = l.h;
    if (cy1 <= cy0) return;
    float cx = (x0 + x1 - 1) * 0.5f;
    float cy = (y0 + y1 - 1) * 0.5f;
    float hw = (x1 - x0) * 0.5f;
    float hh = (y1 - y0) * 0.5f;
    float rr = (float)rad;
    if (rr > hw) rr = hw;
    if (rr > hh) rr = hh;
    if (rr < 0) rr = 0;
    float t = (float)thickness;
    if (t > rr) t = rr;
    if (t >= hw || t >= hh) {
        // 厚度超过半宽: 退化为普通填充
        GLFillRound(l, r, rad, c, a);
        return;
    }
    int cr = GetRValue(c), cg = GetGValue(c), cb = GetBValue(c);

    for (int y = cy0; y < cy1; y++) {
        float insetO = RowInset(y, cy, hh, rr);
        if (insetO < 0) continue;
        float insetI = t + RowInset(y, cy, hh, rr - t);   // 可能为负 = 内圈在该行不存在
        float xsO = cx - hw + insetO;
        float xeO = cx + hw - insetO;
        float xsI = cx - hw + insetI;
        float xeI = cx + hw - insetI;
        if (xsI > xeI - 0.5f) { xsI = xsO; xeI = xeO; }    // 内圈退化: 该行无环
        int xa = (int)ceilf(xsO) - 1; if (xa < 0) xa = 0;
        int xb = (int)floorf(xeO) + 1; if (xb >= l.w) xb = l.w - 1;
        BYTE* row = l.bits + ((size_t)y * l.w + xa) * 4;
        for (int x = xa; x <= xb; x++, row += 4) {
            float covO;
            if (x < xsO)      covO = 0.5f + (float)x - xsO;
            else if (x > xeO) covO = xeO - (float)x;
            else              covO = 1.0f;
            if (covO < 0) covO = 0;
            if (covO > 1) covO = 1;
            float covI;
            if (insetI < 0)   covI = 0;
            else if (x < xsI)      covI = 0.5f + (float)x - xsI;
            else if (x > xeI)      covI = xeI - (float)x;
            else                   covI = 1.0f;
            if (covI < 0) covI = 0;
            if (covI > 1) covI = 1;
            float cov = covO - covI;
            if (cov <= 0) continue;
            int sa = (int)(a * cov + 0.5f);
            if (sa <= 0) continue;
            BlendPx(row, cr * sa / 255, cg * sa / 255, cb * sa / 255, sa);
        }
    }
}

void GLShadow(GLayer& l, const RECT& r, int rad, int depth, COLORREF c, BYTE maxA)
{
    // 同心外扩软阴影: 越靠外越淡 (二次衰减), 与圆角同心
    if (depth <= 0) return;
    for (int i = 1; i <= depth; i++) {
        float t = (float)(depth - i + 1) / (float)(depth + 1);
        BYTE a = (BYTE)(maxA * t * t + 0.5f);
        if (a == 0) continue;
        RECT rr = { r.left - i, r.top - i + 1, r.right + i, r.bottom + i + 1 };
        GLFillRound(l, rr, rad + i, c, a);
    }
}

void GLHLine(GLayer& l, int x0, int x1, int y, COLORREF c, BYTE a)
{
    if (!l.bits || a == 0 || y < 0 || y >= l.h) return;
    if (x0 < 0) x0 = 0;
    if (x1 > l.w) x1 = l.w;
    if (x1 <= x0) return;
    int cr = GetRValue(c), cg = GetGValue(c), cb = GetBValue(c);
    BYTE* row = l.bits + ((size_t)y * l.w + x0) * 4;
    for (int x = x0; x < x1; x++, row += 4)
        BlendPx(row, cr * a / 255, cg * a / 255, cb * a / 255, a);
}

void GLLiftAlpha(GLayer& l)
{
    if (!l.bits) return;
    BYTE* p = l.bits;
    size_t n = (size_t)l.w * l.h;
    for (size_t i = 0; i < n; i++, p += 4) {
        if (p[3] == 0 && (p[0] | p[1] | p[2])) p[3] = 255;
    }
}

void GLLiftAlphaRect(GLayer& l, const RECT& r)
{
    if (!l.bits) return;
    int x0 = r.left; if (x0 < 0) x0 = 0;
    int y0 = r.top;  if (y0 < 0) y0 = 0;
    int x1 = r.right;  if (x1 > l.w) x1 = l.w;
    int y1 = r.bottom; if (y1 > l.h) y1 = l.h;
    for (int y = y0; y < y1; y++) {
        BYTE* p = l.bits + ((size_t)y * l.w + x0) * 4;
        for (int x = x0; x < x1; x++, p += 4) {
            if (p[3] == 0 && (p[0] | p[1] | p[2])) p[3] = 255;
        }
    }
}

void GLBlend(GLayer& dst, const GLayer& src, int x, int y, BYTE opacity)
{
    if (!dst.dc || !src.dc || opacity == 0) return;
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, opacity, AC_SRC_ALPHA };
    AlphaBlend(dst.dc, x, y, src.w, src.h, src.dc, 0, 0, src.w, src.h, bf);
}

void GLPresent(HWND hwnd, const GLayer& surface)
{
    if (!hwnd || !surface.dc) return;
    // UpdateLayeredWindow 的 psize 会把「整个窗口(含边框)」重设为该尺寸 —
    // 因此呈现层必须是窗口外尺寸, 客户区内容按客户区偏移拷入,
    // psize 恒等于窗口当前外尺寸, 绝不触发缩放。
    RECT wr;
    if (!GetWindowRect(hwnd, &wr)) return;
    int ow = wr.right - wr.left;
    int oh = wr.bottom - wr.top;
    if (ow <= 0 || oh <= 0) return;
    POINT cp = { 0, 0 };
    ClientToScreen(hwnd, &cp);
    int ox = cp.x - wr.left;   // 客户区在窗口内的偏移 (左边框)
    int oy = cp.y - wr.top;    // 标题栏高度

    static GLayer s_present;
    if (s_present.w != ow || s_present.h != oh) {
        GLFree(s_present);
        s_present = GLCreate(ow, oh);
        if (!s_present.dc) return;
    }
    GLClear(s_present);
    BitBlt(s_present.dc, ox, oy, surface.w, surface.h, surface.dc, 0, 0, SRCCOPY);

    HDC sd = GetDC(hwnd);
    if (!sd) return;
    POINT pt = { wr.left, wr.top };
    SIZE sz = { ow, oh };
    POINT sp = { 0, 0 };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(hwnd, sd, &pt, &sz, s_present.dc, &sp, 0, &bf, ULW_ALPHA);
    ReleaseDC(hwnd, sd);
}
