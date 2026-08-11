// 极简 WinHTTP GET 工具：report（HWID 上报）与 update（版本检查）共用。
// 系统自带 WinHTTP，无第三方依赖。

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

#include "httputil.h"
#include "types.h"   // APP_VERSION (User-Agent)

#pragma comment(lib, "winhttp.lib")

namespace {
const wchar_t kHttpUserAgent[] = L"AutoClicker/" APP_VERSION;   // 与版本号保持一致
}

bool HttpGetText(const wchar_t* host, int port, const wchar_t* path,
                 std::string* outBody, DWORD* outErr)
{
    auto fail = [outErr](DWORD err) { if (outErr) *outErr = err; return false; };

    HINTERNET hSession = WinHttpOpen(kHttpUserAgent,
                                     WINHTTP_ACCESS_TYPE_NO_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return fail(GetLastError());

    HINTERNET hConn = WinHttpConnect(hSession, host, (INTERNET_PORT)port, 0);
    if (!hConn) {
        DWORD err = GetLastError();
        WinHttpCloseHandle(hSession);
        return fail(err);
    }

    HINTERNET hReq = WinHttpOpenRequest(hConn, L"GET", path, nullptr,
                                        WINHTTP_NO_REFERER,
                                        WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hReq) {
        DWORD err = GetLastError();
        WinHttpCloseHandle(hConn);
        WinHttpCloseHandle(hSession);
        return fail(err);
    }

    // 硬 5s 预算：解析/连接/发送/接收，服务器不可用时不拖住调用线程
    WinHttpSetTimeouts(hReq, 5000, 5000, 5000, 5000);

    DWORD err = 0;
    bool ok = false;
    if (WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hReq, nullptr)) {
        DWORD status = 0, cb = sizeof(status);
        if (WinHttpQueryHeaders(hReq,
                                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &cb,
                                WINHTTP_NO_HEADER_INDEX) &&
            status == 200) {
            if (!outBody) {
                ok = true;   // fire-and-forget：不读响应体
            } else {
                outBody->clear();
                char buf[1024];
                DWORD read = 0;
                for (;;) {
                    read = 0;
                    if (!WinHttpReadData(hReq, buf, sizeof(buf), &read)) break;
                    if (read == 0) break;
                    outBody->append(buf, read);
                }
                ok = true;
            }
        }
    }
    if (!ok) err = GetLastError();

    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConn);
    WinHttpCloseHandle(hSession);
    return ok ? true : fail(err);
}
