#include "ui.h"
#include "types.h"
#include <dwmapi.h>

void ApplyWin11Style(HWND hwnd)
{
    // 无边框分层玻璃窗: 无系统标题栏 (DWMA_DARK 不再需要);
    // 明确关闭 DWM 圆角 — 玻璃圆角由基底玻璃自绘 (rad 12, 桌面透出)。
    int val = CORNER_DONOTROUND;
    DwmSetWindowAttribute(hwnd, (DWMWINDOWATTRIBUTE)DWMA_CORNER, &val, sizeof(val));
}
