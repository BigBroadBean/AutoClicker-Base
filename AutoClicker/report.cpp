// HWID usage reporting module.
//
// Server address is hardcoded in servercfg.h (domain + port), no config file.
// To deploy, change servercfg.h, e.g.:
//     kServerHost = L"stats.example.com";  kServerPort = 8080;
// Full URL: http://localhost:3000/report?hwid=HW-...
//
// Design notes:
//  - WinHTTP (system component, no third-party dependency), shared helper in
//    httputil.cpp with a 5s total timeout
//  - WINHTTP_ACCESS_TYPE_NO_PROXY: the test server is on localhost, going
//    through a system proxy would break it. When moving to a public domain
//    behind a corporate proxy, switch to WINHTTP_ACCESS_TYPE_DEFAULT_PROXY
//    in httputil.cpp.
//  - Failures only append to report.log; the UI and clicker threads are
//    never touched.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>

#include "report.h"
#include "httputil.h"
#include "servercfg.h"

#include <string>
#include <thread>
#include <cstdio>
#include <cstdarg>
#include <cctype>

// ---- report interface path (host/port in servercfg.h) ----
static const wchar_t kReportPath[] = L"/report";

// ============================================================
//  HWID: stable per-machine id
// ============================================================
static std::string GetHwid()
{
    // 1) MachineGuid: unique per Windows install, stable across reboots.
    //    Strip braces/dashes, keep hex chars, "HW-" + up to 32 hex digits.
    HKEY hk = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Cryptography",
                      0, KEY_READ | KEY_WOW64_64KEY, &hk) == ERROR_SUCCESS) {
        char buf[128] = {};
        DWORD sz = sizeof(buf), type = 0;
        LONG r = RegQueryValueExA(hk, "MachineGuid", nullptr, &type,
                                  (LPBYTE)buf, &sz);
        RegCloseKey(hk);
        if (r == ERROR_SUCCESS && type == REG_SZ && sz > 1 && buf[0]) {
            std::string hw = "HW-";
            for (const char* p = buf; *p && hw.size() < 4 + 32; ++p) {
                char c = (char)toupper((unsigned char)*p);
                if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))
                    hw += c;
            }
            if (hw.size() > 4) return hw;
        }
    }
    // 2) fallback: volume serial number of the system drive
    char root[4] = "C:\\";
    char windir[MAX_PATH] = {};
    if (GetWindowsDirectoryA(windir, MAX_PATH) && windir[0] && windir[1] == ':') {
        root[0] = windir[0];
    }
    DWORD serial = 0;
    if (GetVolumeInformationA(root, nullptr, 0, &serial,
                              nullptr, nullptr, nullptr, 0)) {
        char buf[16];
        sprintf_s(buf, "HW-%08X", serial);
        return buf;
    }
    return "HW-UNKNOWN";
}

// URL-encode a string for the query part (letters/digits/-/_. stay as-is)
static std::wstring UrlEncode(const std::string& s)
{
    std::wstring out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.') {
            out += (wchar_t)c;
        } else {
            wchar_t esc[8];
            swprintf(esc, 8, L"%%%02X", c);
            out += esc;
        }
    }
    return out;
}

// diagnostics: %APPDATA%\AutoClicker\report.log (append)
static void LogReport(const char* fmt, ...)
{
    static char path[MAX_PATH] = {};
    if (!path[0]) {
        char appdata[MAX_PATH] = {};
        if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) {
            char dir[MAX_PATH] = {};
            sprintf_s(dir, "%s\\AutoClicker", appdata);
            CreateDirectoryA(dir, nullptr);
            sprintf_s(path, "%s\\report.log", dir);
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
//  entry: fire-and-forget background report
// ============================================================
void StartHwidReporter()
{
    std::thread([]() {
        std::string hwid = GetHwid();

        std::wstring path = kReportPath;
        path += L"?hwid=";
        path += UrlEncode(hwid);

        LogReport("reporting hwid=%s -> http://%ls:%d%ls",
                  hwid.c_str(), kServerHost, kServerPort, path.c_str());

        DWORD err = 0;
        if (HttpGetText(kServerHost, kServerPort, path.c_str(), nullptr, &err)) {
            LogReport("report OK hwid=%s", hwid.c_str());
        } else {
            LogReport("report FAILED hwid=%s err=%lu", hwid.c_str(), err);
        }
    }).detach();
}
