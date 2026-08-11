// 启动时版本检查：与服务器最新版本对比，有新版本则 MessageBox 提示。
// 后台 detached 线程，全阶段 5s 超时，任何失败都只写日志不打扰用户。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>

#include "update.h"
#include "httputil.h"
#include "servercfg.h"
#include "versionutil.h"
#include "types.h"

#include <string>
#include <thread>
#include <cstdio>
#include <cstdarg>
#include <cctype>

// ---- 本地版本号（与 UI 标题栏一致；发新版只改 types.h 的 APP_VERSION）----
static const char* kLocalVersion = APP_VERSION;

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
//  diagnostics: %APPDATA%\AutoClicker\update.log (append)
// ============================================================
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

        // 4) 弹窗：最新版本号 + 更新内容（版本号显示前规范化，避免双 v）
        std::wstring remoteW = Utf8ToWide(NormalizeVersionDisplay(remote));
        std::wstring localW = Utf8ToWide(NormalizeVersionDisplay(kLocalVersion));
        std::wstring msg = L"发现新版本 v" + remoteW + L"！\n\n";
        msg += L"当前版本：v" + localW + L"\n";
        msg += L"最新版本：v" + remoteW + L"\n";
        if (!content.empty())
            msg += L"\n更新内容：\n" + Utf8ToWide(content);
        else
            msg += L"\n（服务器未提供更新内容）";

        MessageBoxW(nullptr, msg.c_str(), L"AutoClicker 发现新版本",
                    MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
        LogUpdate("已提示新版本 %s", remote.c_str());
    }).detach();
}
