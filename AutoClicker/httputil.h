#pragma once

#include <Windows.h>
#include <string>

// ============================================================
//  极简 WinHTTP GET 工具（域名 + 端口模式）
// ============================================================
//  - 成功且 HTTP 状态码为 200 → 返回 true
//  - outBody != nullptr 时读入完整响应体（原始 UTF-8 字节）
//  - outBody == nullptr 时只确认收到响应（fire-and-forget，不读 body）
//  - outErr 可选：失败时写入错误码（WinHTTP / Win32）
//  - 全阶段 5s 超时：服务器不可用时最多阻塞调用线程 5s
bool HttpGetText(const wchar_t* host, int port, const wchar_t* path,
                 std::string* outBody, DWORD* outErr = nullptr);
