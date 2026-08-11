// winsock2 must come before windows.h to avoid winsock.h conflicts;
// WIN32_LEAN_AND_MEAN keeps windows.h from pulling in winsock.h at all.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include "canattack.h"

#include <tlhelp32.h>
#include <psapi.h>
#include <shlobj.h>

#include "resource.h"

#include <thread>
#include <set>
#include <cstring>
#include <cstdio>
#include <cstdarg>

#pragma comment(lib, "ws2_32.lib")

// ============================================================
//  state
// ============================================================
std::atomic<bool> canAttackOnlyClick{ false };   // default: feature OFF
std::atomic<bool> placeOnlyRightClick{ false };  // default: feature OFF
int vk_canattack_key = VK_F6;   // default: F6 (free in Minecraft; F1/F2/F3/F5/F11 are taken)
int vk_place_key     = VK_F7;   // default: F7

std::atomic<int> g_canAttack{ 0 };            // fail-safe: unknown -> cannot attack
std::atomic<int> g_canPlace{ 0 };             // fail-safe: unknown -> not a placeable
std::atomic<long long> g_canAttackLastMs{ 0 };

static constexpr int    kCanAttackPort = 35785;
static constexpr DWORD  kRecvTimeoutMs = 25;      // SO_RCVTIMEO (loop period when idle)
static constexpr long long kStaleMs     = 300;    // no packet -> treat as "cannot attack"
static constexpr long long kConnectedMs = 1000;   // GUI "connected" freshness window

bool CanAttackConnected()
{
    long long last = g_canAttackLastMs.load(std::memory_order_relaxed);
    return last != 0 && (GetTickCount64() - last) < kConnectedMs;
}

// ============================================================
//  UDP monitor thread (async, 5ms loop)
// ============================================================
void StartCanAttackMonitor()
{
    std::thread([]() {
        WSADATA wd;
        if (WSAStartup(MAKEWORD(2, 2), &wd) != 0)
            return;

        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s != INVALID_SOCKET) {
            SOCKADDR_IN a = {};
            a.sin_family = AF_INET;
            a.sin_port = htons(kCanAttackPort);
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // localhost only
            if (bind(s, (SOCKADDR*)&a, sizeof(a)) == 0) {
                DWORD tmo = kRecvTimeoutMs;
                setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));

                char buf[64];
                for (;;) {
                    SOCKADDR_IN from = {};
                    int flen = sizeof(from);
                    int n = recvfrom(s, buf, sizeof(buf), 0, (SOCKADDR*)&from, &flen);
                    if (n >= 1 && from.sin_addr.s_addr == htonl(INADDR_LOOPBACK)) {
                        // 2-byte protocol: [byte0=canAttack][byte1=canPlace].
                        // Old 1-byte datagrams still update canAttack (byte0 is
                        // protocol-compatible); canPlace stays at its last value.
                        char c = buf[0];
                        if (c == '1' || c == 1) {
                            g_canAttack.store(1, std::memory_order_relaxed);
                        } else if (c == '0' || c == 0) {
                            g_canAttack.store(0, std::memory_order_relaxed);
                        } else {
                            continue;   // garbage datagram: ignore, keep last state
                        }
                        if (n >= 2) {
                            char p = buf[1];
                            if (p == '1' || p == 1) {
                                g_canPlace.store(1, std::memory_order_relaxed);
                            } else if (p == '0' || p == 0) {
                                g_canPlace.store(0, std::memory_order_relaxed);
                            }
                        }
                        g_canAttackLastMs.store((long long)GetTickCount64(),
                                                std::memory_order_relaxed);
                    } else {
                        // timeout: mark stale -> fail-safe "cannot attack"
                        long long last = g_canAttackLastMs.load(std::memory_order_relaxed);
                        if (last != 0 && GetTickCount64() - last > kStaleMs) {
                            g_canAttack.store(0, std::memory_order_relaxed);
                            g_canPlace.store(0, std::memory_order_relaxed);
                        }
                    }
                    Sleep(5);   // 5ms loop, precisely adjusts the variable
                }
            }
            closesocket(s);
        }
        WSACleanup();
    }).detach();
}

// ============================================================
//  DLL injection helpers
// ============================================================
static void LogInject(const char* fmt, ...)
{
    // %APPDATA%\AutoClicker\inject.log - diagnostics for blocked injections
    static char path[MAX_PATH] = {};
    if (!path[0]) {
        char appdata[MAX_PATH] = {};
        if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) {
            sprintf_s(path, "%s\\AutoClicker\\inject.log", appdata);
        }
    }
    if (!path[0]) return;
    FILE* f = nullptr;
    if (fopen_s(&f, path, "a") == 0 && f) {
        SYSTEMTIME st = {};
        GetLocalTime(&st);
        fprintf(f, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        va_list ap;
        va_start(ap, fmt);
        vfprintf(f, fmt, ap);
        va_end(ap);
        fprintf(f, "\n");
        fclose(f);
    }
}

static void EnableDebugPrivilege()
{
    HANDLE hToken = nullptr;
    if (OpenProcessToken(GetCurrentProcess(),
                         TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        LUID luid;
        if (LookupPrivilegeValueA(nullptr, "SeDebugPrivilege", &luid)) {
            TOKEN_PRIVILEGES tp = {};
            tp.PrivilegeCount = 1;
            tp.Privileges[0].Luid = luid;
            tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            AdjustTokenPrivileges(hToken, FALSE, &tp, 0, nullptr, nullptr);
        }
        CloseHandle(hToken);
    }
}

// extract MCCombatStatusJni.dll from the embedded RCDATA resource into
// %TEMP%\AutoClicker\ so a single exe is enough to run (no sidecar DLL)
static bool ExtractEmbeddedDll()
{
    HRSRC hr = FindResourceA(nullptr, MAKEINTRESOURCEA(IDR_MC_DLL), RT_RCDATA);
    if (!hr) return false;
    HGLOBAL hg = LoadResource(nullptr, hr);
    if (!hg) return false;
    void* p = LockResource(hg);
    DWORD sz = SizeofResource(nullptr, hr);
    if (!p || sz == 0) return false;

    char path[MAX_PATH] = {};
    if (!GetTempPathA(MAX_PATH, path)) return false;
    strcat_s(path, "AutoClicker");
    CreateDirectoryA(path, nullptr);
    strcat_s(path, "\\MCCombatStatusJni.dll");

    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    WriteFile(f, p, sz, &written, nullptr);
    CloseHandle(f);
    return written == sz;
}

// resolve the DLL path: exe directory -> working directory -> embedded copy
static bool ResolveDllPath(char* out, SIZE_T cap)
{
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) *(slash + 1) = '\0';
    strcat_s(path, "MCCombatStatusJni.dll");
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
        strncpy_s(out, cap, path, _TRUNCATE);
        return true;
    }
    GetCurrentDirectoryA(MAX_PATH, path);
    size_t len = strlen(path);
    if (len > 0 && path[len - 1] != '\\') strcat_s(path, "\\");
    strcat_s(path, "MCCombatStatusJni.dll");
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
        strncpy_s(out, cap, path, _TRUNCATE);
        return true;
    }
    // 3. embedded copy extracted to %TEMP%\AutoClicker
    if (GetTempPathA(MAX_PATH, path)) {
        strcat_s(path, "AutoClicker\\MCCombatStatusJni.dll");
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
            strncpy_s(out, cap, path, _TRUNCATE);
            return true;
        }
    }
    return false;
}

bool CanAttackDllAvailable()
{
    char path[MAX_PATH] = {};
    return ResolveDllPath(path, MAX_PATH);
}

// x64 DLL can only go into 64-bit processes
static bool ProcessIs64Bit(DWORD pid)
{
    HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hp) return false;
    BOOL wow64 = FALSE;
    BOOL ok = IsWow64Process(hp, &wow64);
    CloseHandle(hp);
    return ok && !wow64;
}

// is the DLL already loaded in the target process? (anti double-injection)
static bool ProcessHasModule(DWORD pid, const wchar_t* moduleName)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W me = {};
    me.dwSize = sizeof(me);
    bool found = false;
    if (Module32FirstW(snap, &me)) {
        do {
            if (_wcsicmp(me.szModule, moduleName) == 0) { found = true; break; }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

// log the first few window class names owned by pid (for diagnosing
// launchers that use non-standard window classes)
static void LogProcessWindows(DWORD pid)
{
    struct Ctx { DWORD pid; int n; };
    Ctx ctx = { pid, 0 };
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        Ctx* c = (Ctx*)lp;
        if (c->n >= 3) return FALSE;
        DWORD wpid = 0;
        GetWindowThreadProcessId(hwnd, &wpid);
        if (wpid != c->pid) return TRUE;
        char cls[64] = {};
        if (!GetClassNameA(hwnd, cls, 64)) return TRUE;
        wchar_t title[128] = {};
        GetWindowTextW(hwnd, title, 128);
        LogInject("   window class='%s' title='%ls'", cls, title);
        c->n++;
        return TRUE;
    }, (LPARAM)&ctx);
}

// Minecraft's window classes: GLFW30 (LWJGL3, MC 1.13+) / LWJGL (1.12-).
// Only inject into processes that actually own an MC client window.
static bool HasMinecraftWindow(DWORD pid)
{
    struct Ctx { DWORD pid; bool found; };
    Ctx ctx = { pid, false };
    EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
        Ctx* c = (Ctx*)lp;
        DWORD wpid = 0;
        GetWindowThreadProcessId(hwnd, &wpid);
        if (wpid != c->pid) return TRUE;
        char cls[64] = {};
        if (!GetClassNameA(hwnd, cls, 64)) return TRUE;
        if (strcmp(cls, "GLFW30") == 0 || strcmp(cls, "LWJGL") == 0) {
            c->found = true;
            return FALSE;
        }
        return TRUE;
    }, (LPARAM)&ctx);
    return ctx.found;
}

// classic LoadLibraryA remote-thread injection with full error handling.
// every step is logged so blocked injections can be diagnosed.
static bool InjectDll(DWORD pid, const char* path)
{
    HANDLE hp = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                            PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                            FALSE, pid);
    if (!hp) {
        LogInject("pid %lu: OpenProcess FAILED err=%lu", pid, GetLastError());
        return false;
    }
    LogInject("pid %lu: OpenProcess OK", pid);

    SIZE_T len = strlen(path) + 1;
    void* mem = VirtualAllocEx(hp, nullptr, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem) {
        LogInject("pid %lu: VirtualAllocEx FAILED err=%lu", pid, GetLastError());
        CloseHandle(hp);
        return false;
    }
    if (!WriteProcessMemory(hp, mem, path, len, nullptr)) {
        LogInject("pid %lu: WriteProcessMemory FAILED err=%lu", pid, GetLastError());
        VirtualFreeEx(hp, mem, 0, MEM_RELEASE);
        CloseHandle(hp);
        return false;
    }

    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    auto pLoadLibraryA = (LPTHREAD_START_ROUTINE)GetProcAddress(k32, "LoadLibraryA");
    HANDLE ht = CreateRemoteThread(hp, nullptr, 0, pLoadLibraryA, mem, 0, nullptr);
    if (!ht) {
        LogInject("pid %lu: CreateRemoteThread FAILED err=%lu (anti-injection?)",
                  pid, GetLastError());
        VirtualFreeEx(hp, mem, 0, MEM_RELEASE);
        CloseHandle(hp);
        return false;
    }

    DWORD wait = WaitForSingleObject(ht, 3000);
    DWORD ret = 0;
    GetExitCodeThread(ht, &ret);
    CloseHandle(ht);
    VirtualFreeEx(hp, mem, 0, MEM_RELEASE);
    CloseHandle(hp);

    LogInject("pid %lu: remote LoadLibraryA returned 0x%p (wait=%lu)",
              pid, (void*)(uintptr_t)ret, wait);
    bool ok = (wait != WAIT_TIMEOUT) && ret != 0;
    // timed out? the module may still have loaded - verify to avoid re-injection
    if (!ok && ProcessHasModule(pid, L"MCCombatStatusJni.dll")) ok = true;
    return ok;
}

// ============================================================
//  injector thread: find every not-yet-injected MC Java process
// ============================================================
void StartInjectorThread()
{
    std::thread([]() {
        EnableDebugPrivilege();
        ExtractEmbeddedDll();   // ensure the temp copy exists for injection

        char dllPath[MAX_PATH] = {};
        if (!ResolveDllPath(dllPath, MAX_PATH)) {
            // retry: the user may drop the DLL next to the exe later
            for (;;) {
                Sleep(1000);
                if (ResolveDllPath(dllPath, MAX_PATH)) break;
            }
        }

        std::set<DWORD> injected;   // PIDs we have already taken care of

        LogInject("=== injector started, dll=%s (idle until feature enabled) ===", dllPath);
        for (;;) {
            // Neither feature on: never inject anything (UDP monitor keeps
            // running so the status chips still show live state from
            // already-injected games). Injection starts as soon as EITHER the
            // can-attack (left-button) gate or the can-place (right-button)
            // gate is enabled; both gates share this single injector loop
            // (already-injected PIDs are deduplicated in the set below).
            if (canAttackOnlyClick.load(std::memory_order_relaxed) ||
                placeOnlyRightClick.load(std::memory_order_relaxed)) {
                HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
                if (snap != INVALID_HANDLE_VALUE) {
                    PROCESSENTRY32W pe = {};
                    pe.dwSize = sizeof(pe);
                    for (BOOL ok = Process32FirstW(snap, &pe); ok;
                         ok = Process32NextW(snap, &pe)) {
                        if (pe.th32ProcessID == GetCurrentProcessId()) continue;
                        const wchar_t* nm = pe.szExeFile;
                        if (_wcsicmp(nm, L"javaw.exe") != 0 &&
                            _wcsicmp(nm, L"java.exe") != 0 &&
                            _wcsicmp(nm, L"minecraft.exe") != 0 &&
                            _wcsicmp(nm, L"mc.exe") != 0) continue;
                        if (injected.count(pe.th32ProcessID)) continue;
                        if (!ProcessIs64Bit(pe.th32ProcessID)) {
                            LogInject("pid %lu (%ls): skipped - not x64", pe.th32ProcessID, nm);
                            continue;
                        }
                        if (!HasMinecraftWindow(pe.th32ProcessID)) {
                            LogInject("pid %lu (%ls): skipped - no MC window",
                                      pe.th32ProcessID, nm);
                            LogProcessWindows(pe.th32ProcessID);
                            continue;
                        }
                        if (ProcessHasModule(pe.th32ProcessID, L"MCCombatStatusJni.dll")) {
                            injected.insert(pe.th32ProcessID);   // already loaded
                            LogInject("pid %lu (%ls): already loaded, marked", pe.th32ProcessID, nm);
                            continue;
                        }
                        LogInject("pid %lu (%ls): injecting...", pe.th32ProcessID, nm);
                        if (InjectDll(pe.th32ProcessID, dllPath)) {
                            injected.insert(pe.th32ProcessID);
                            LogInject("pid %lu: injection OK", pe.th32ProcessID);
                        }
                        // failure (elevation etc.): do NOT mark - retry next cycle
                    }
                    CloseHandle(snap);
                }
            }

            // prune dead PIDs so a restarted game gets injected again
            for (auto it = injected.begin(); it != injected.end(); ) {
                HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, *it);
                if (!hp) { it = injected.erase(it); continue; }
                DWORD code = 0;
                GetExitCodeProcess(hp, &code);
                CloseHandle(hp);
                if (code != STILL_ACTIVE) it = injected.erase(it);
                else ++it;
            }

            Sleep(1000);   // 1s injection scan period
        }
    }).detach();
}
