// 启动时版本检查：与服务器最新版本对比，有新版本则 MessageBox 提示。
// 后台 detached 线程，全阶段 5s 超时，任何失败都只写日志不打扰用户。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>

#include "update.h"
#include "httputil.h"
#include "servercfg.h"

#include <string>
#include <thread>
#include <cstdio>
#include <cstdarg>
#include <cctype>

// ---- 本地版本号（与 UI 标题栏 "v2.5" 一致；发布新版时同步修改）----
static const char kLocalVersion[] = "2.5";

// ---- 服务器接口路径（域名+端口见 servercfg.h）----
static const wchar_t kPathVersionLatest[] = L"/version/latest";
static const wchar_t kPathContentLatest[] = L"/content/latest";

// ============================================================
//  diagnostics: %APPDATA%\AutoClicker\update.log (append)
// ============================================================
static void LogUpdate(const char* fmt, ...)
{
    static char path[MAX_PATH] = {};
    if (!path[0]) {
        char appdata[MAX_PATH] = {};
        if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) {
            char dir[MAX_PATH] = {};
            sprintf_s(dir, "%s\\AutoClicker", appdata);
            CreateDirectoryA(dir, nullptr);
            sprintf_s(path, "%s\\update.log", dir);
        }
    }
    if (!path[0]) return;
    FILE* f = nullptr;
    if (fopen_s(&f, path, "a") == 0 && f) {
        SYSTEMTIME st = {};
        GetLocalTime(&st);
        fprintf(f, "[%02d:%02d:%02d.%03d] ",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fprintf(f, "\n");
        fclose(f);
    }
}

// ============================================================
//  JSON 字符串提取（极简，够用即可）
// ============================================================
// 从服务器返回的 JSON 中提取 "key": "value" 的字符串值。
// 处理 \n \r \t \" \\ 转义；\uXXXX 不处理（Node JSON.stringify 直接输出
// UTF-8 原文，服务器内容受控，够用）。
static bool GetJsonString(const std::string& json, const std::string& key,
                          std::string& out)
{
    std::string pat = "\"" + key + "\"";
    size_t pos = json.find(pat);
    if (pos == std::string::npos) return false;
    pos += pat.size();
    // 跳过空白到 ':'
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
                                 json[pos] == '\r' || json[pos] == '\n')) pos++;
    if (pos >= json.size() || json[pos] != ':') return false;
    pos++;
    // 跳过空白到字符串引号
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
                                 json[pos] == '\r' || json[pos] == '\n')) pos++;
    if (pos >= json.size() || json[pos] != '"') return false;
    pos++;

    out.clear();
    while (pos < json.size()) {
        char c = json[pos++];
        if (c == '"') return true;   // 字符串结束
        if (c == '\\' && pos < json.size()) {
            char e = json[pos++];
            switch (e) {
            case 'n':  out += '\n'; break;
            case 'r':  out += '\r'; break;
            case 't':  out += '\t'; break;
            case '\\': out += '\\'; break;
            case '"':  out += '"';  break;
            default:   out += e;    break;   // 其余转义按原样（够用）
            }
        } else {
            out += c;
        }
    }
    return false;   // 未闭合
}

// ============================================================
//  点分数字版本比较
// ============================================================
// 只比较数字段（忽略 v 前缀、'-'、'.' 等分隔符与后缀）：
//   "2.5"  vs "2.6"   -> -1
//   "2.5"  vs "2.5.1" -> -1
//   "2.10" vs "2.9"   ->  1
//   "v2.5" vs "2.5"   ->  0
static int CompareVersions(const std::string& a, const std::string& b)
{
    size_t ia = 0, ib = 0;
    for (;;) {
        // 跳到下一个数字段的开始（忽略 v 前缀 / 分隔符 / 后缀）
        while (ia < a.size() && !isdigit((unsigned char)a[ia])) ia++;
        while (ib < b.size() && !isdigit((unsigned char)b[ib])) ib++;

        int na = 0, nb = 0;
        while (ia < a.size() && isdigit((unsigned char)a[ia])) na = na * 10 + (a[ia++] - '0');
        while (ib < b.size() && isdigit((unsigned char)b[ib])) nb = nb * 10 + (b[ib++] - '0');

        if (na != nb) return na < nb ? -1 : 1;
        // 本段相等：两边都到字符串末尾才认为完全相等，否则继续下一段
        // （某一方先结束：下一轮该方数字段为 0，自然小于另一方）
        if (ia >= a.size() && ib >= b.size()) return 0;
    }
}

// UTF-8 -> UTF-16（MessageBox 显示中文更新内容）
static std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

// ============================================================
//  entry: fire-and-forget update check
// ============================================================
void StartVersionCheck()
{
    std::thread([]() {
        // 1) 取服务器最新版本号
        std::string json;
        DWORD err = 0;
        if (!HttpGetText(kServerHost, kServerPort, kPathVersionLatest, &json, &err)) {
            LogUpdate("版本检查失败 err=%lu（服务器不可用则静默跳过）", err);
            return;
        }
        std::string remote;
        if (!GetJsonString(json, "version", remote) || remote.empty()) {
            LogUpdate("版本检查：服务器未设置版本号，静默跳过");
            return;
        }

        LogUpdate("版本检查：本地=%s 服务器=%s", kLocalVersion, remote.c_str());

        // 2) 已是最新（或服务器版本不高于本地）-> 不提示
        if (CompareVersions(kLocalVersion, remote) >= 0) {
            LogUpdate("已是最新版本，不提示");
            return;
        }

        // 3) 有新版本：再取更新内容（失败不影响弹窗）
        std::string content;
        std::string cjson;
        if (HttpGetText(kServerHost, kServerPort, kPathContentLatest, &cjson, nullptr))
            GetJsonString(cjson, "update_content", content);

        // 4) 弹窗：最新版本号 + 更新内容
        std::wstring msg = L"发现新版本 v" + Utf8ToWide(remote) + L"！\n\n";
        msg += L"当前版本：v" + Utf8ToWide(kLocalVersion) + L"\n";
        msg += L"最新版本：v" + Utf8ToWide(remote) + L"\n";
        if (!content.empty())
            msg += L"\n更新内容：\n" + Utf8ToWide(content);
        else
            msg += L"\n（服务器未提供更新内容）";

        MessageBoxW(nullptr, msg.c_str(), L"AutoClicker 发现新版本",
                    MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
        LogUpdate("已提示新版本 %s", remote.c_str());
    }).detach();
}
