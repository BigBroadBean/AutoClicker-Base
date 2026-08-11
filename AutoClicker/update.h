#pragma once

// ============================================================
//  启动时版本检查（update check）
// ============================================================
// 启动时在后台线程请求服务器最新版本号（GET /version/latest，域名+端口
// 见 servercfg.h），与本地版本号比较：
//   - 服务器版本 > 本地版本 → MessageBox 弹窗提示（显示最新版本号 + 更新
//     内容，内容来自 GET /content/latest）
//   - 已是最新 / 服务器不可用 / 未设置版本号 → 静默，不打扰用户
//
// 本地版本号 kLocalVersion 定义在 update.cpp，发新版时同步修改（同时改
// httputil.cpp 的 User-Agent 与 UI 标题栏显示的版本号）。

void StartVersionCheck();
