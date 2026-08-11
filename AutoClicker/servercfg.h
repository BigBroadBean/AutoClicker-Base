#pragma once

// ============================================================
//  服务器配置（写死，无配置文件）
// ============================================================
// 域名 + 端口模式：report（HWID 上报）与 update（版本检查）模块共用。
// 部署时只改这里，两个功能同时生效。
// 当前为本地测试：http://localhost:3000
constexpr wchar_t kServerHost[] = L"counter.bigbroadbean.top";   // 域名或 IP（不含协议与端口）
constexpr int     kServerPort   = 3000;           // 端口
