#pragma once

#include <Windows.h>

void ApplyWin11Style(HWND hwnd);

// UI 主窗口句柄 (WinMain 创建后赋值; 连点线程热键切换方案后向它发通知)
extern HWND g_uiHwnd;

// 方案热键切换完成 -> WndProc 应用主题/置顶并弹 Toast (wParam = 方案 1..4)
#define WM_APP_PROFILE (WM_APP + 1)