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
#include <map>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <mutex>

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

// ---- gate lifecycle: wake event shared by all three threads ----
// The shm poller, UDP monitor and injector all park on this auto-reset
// event while both gates are off (zero wakeups, zero CPU), and get kicked
// instantly when either gate is toggled ("随用随上").
static HANDLE GateWakeEvent()
{
    static HANDLE h = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    return h;
}

// An auto-reset event releases exactly ONE waiter per SetEvent, and a
// signal that has no waiter yet stays latched until someone waits on it.
// Pulsing once per consumer therefore wakes every parked thread: a thread
// that parks after the pulses still finds a leftover signal (with only
// kGateWakeConsumers-1 other threads able to consume it). Keep this count
// >= the number of threads that can park on the event.
static constexpr int kGateWakeConsumers = 3;   // shm poller + UDP monitor + injector

void NotifyGateToggled()
{
    if (HANDLE h = GateWakeEvent()) {
        for (int i = 0; i < kGateWakeConsumers; ++i) SetEvent(h);
    }
}

static bool AnyGateOn()
{
    return canAttackOnlyClick.load(std::memory_order_relaxed) ||
           placeOnlyRightClick.load(std::memory_order_relaxed);
}

bool CanAttackConnected()
{
    long long last = g_canAttackLastMs.load(std::memory_order_relaxed);
    return last != 0 && (GetTickCount64() - last) < kConnectedMs;
}

// ============================================================
//  UDP monitor thread (fallback channel for old DLL builds)
// ============================================================
// Lifecycle-gated: the port is only bound while a gate is on. When both
// gates are off the socket is closed and the thread parks on the wake
// event (zero CPU, and 35785 stays free for other programs). The shared
// memory poller is the primary channel; this only serves DLL builds that
// predate the shared-memory protocol.
void StartCanAttackMonitor()
{
    std::thread([]() {
        WSADATA wd;
        if (WSAStartup(MAKEWORD(2, 2), &wd) != 0)
            return;

        HANDLE wake = GateWakeEvent();

        for (;;) {
            // park while both gates are off
            while (!AnyGateOn())
                WaitForSingleObject(wake, INFINITE);

            SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            bool bound = false;
            if (s != INVALID_SOCKET) {
                SOCKADDR_IN a = {};
                a.sin_family = AF_INET;
                a.sin_port = htons(kCanAttackPort);
                a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // localhost only
                if (bind(s, (SOCKADDR*)&a, sizeof(a)) == 0) {
                    bound = true;
                    DWORD tmo = kRecvTimeoutMs;
                    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tmo, sizeof(tmo));

                    char buf[64];
                    while (AnyGateOn()) {
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
            // gates just turned off (or the port was busy): release it and
            // wait; a toggle wakes us immediately, otherwise retry each 1s
            WaitForSingleObject(wake, bound ? INFINITE : 1000);
        }
        WSACleanup();
    }).detach();
}

// ============================================================
//  shared-memory status poller (PRIMARY channel)
// ============================================================
// The injected DLL publishes a packed CombatStatus struct into
// "Local\MCCombatStatus_<pid>" every ~5ms (see the MCCombatStatusJni
// source; layout is fixed and versioned):
//   offset 0   DWORD magic     = 0x4D435354 ('MCST')
//   offset 4   DWORD version   = 7
//   offset 20  LONG  canAttack
//   offset 24  LONG  canPlace
//   offset 632 LONG  tick      (incremented every 5ms worker loop)
// Reading shared memory has no port conflicts and no socket overhead,
// so this replaces UDP as the primary channel; the UDP monitor above
// only serves old DLL builds.
static void LogInject(const char* fmt, ...);   // defined in the injector section
static bool HasHealthyShm(DWORD pid);          // defined in the injector section

static constexpr SIZE_T kShmOffMagic     = 0;
static constexpr SIZE_T kShmOffVersion   = 4;
static constexpr SIZE_T kShmOffInGame    = 16;
static constexpr SIZE_T kShmOffCanAttack = 20;
static constexpr SIZE_T kShmOffCanPlace  = 24;
static constexpr SIZE_T kShmOffHitType   = 32;
static constexpr SIZE_T kShmOffTargetName= 52;   // char[128]
static constexpr SIZE_T kShmOffTick      = 632;
static constexpr DWORD  kShmMagic        = 0x4D435354;   // 'MCST'
static constexpr DWORD  kShmVersion      = 7;

// ---- HUD 快照 (shm poller 写入, UI 线程读取) ----
static std::atomic<int> g_shmInGame{ 0 };
static std::atomic<int> g_shmHitType{ 0 };
static char        g_shmTargetName[128] = {};
static std::mutex  g_shmTargetMtx;

int GetShmInGame()  { return g_shmInGame.load(std::memory_order_relaxed); }
int GetShmHitType() { return g_shmHitType.load(std::memory_order_relaxed); }
void GetShmTargetName(char* out, size_t cap)
{
    if (!out || cap == 0) return;
    std::lock_guard<std::mutex> g(g_shmTargetMtx);
    strncpy_s(out, cap, g_shmTargetName, _TRUNCATE);
}

struct ShmTarget {
    DWORD      pid;
    HANDLE     hMap;
    const BYTE* view;
    DWORD      lastTick;      // 0 = not resolved yet
};

// owned exclusively by the shm poller thread (no locking needed)
static std::vector<ShmTarget> g_shmTargets;

template <typename T>
static T ShmRead(const BYTE* p, SIZE_T off)
{
    T v;
    memcpy(&v, p + off, sizeof(T));   // memcpy: alignment-safe load
    return v;
}

// is this process a Minecraft Java client that may carry our DLL?
static bool IsMcProcessName(const wchar_t* nm)
{
    return _wcsicmp(nm, L"javaw.exe") == 0 ||
           _wcsicmp(nm, L"java.exe") == 0 ||
           _wcsicmp(nm, L"minecraft.exe") == 0 ||
           _wcsicmp(nm, L"mc.exe") == 0;
}

void StartCanAttackShmPoller()
{
    std::thread([]() {
        HANDLE wake = GateWakeEvent();
        DWORD nextScan = 0;

        for (;;) {
            DWORD now = GetTickCount();

            // 1s rescan: drop dead targets, discover new publishers
            if (now >= nextScan) {
                nextScan = now + 1000;

                for (size_t i = 0; i < g_shmTargets.size(); ) {
                    ShmTarget& t = g_shmTargets[i];
                    bool alive = false;
                    HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, t.pid);
                    if (hp) {
                        DWORD code = 0;
                        GetExitCodeProcess(hp, &code);
                        CloseHandle(hp);
                        alive = (code == STILL_ACTIVE);
                    }
                    if (alive) { ++i; continue; }
                    UnmapViewOfFile(t.view);
                    CloseHandle(t.hMap);
                    g_shmTargets.erase(g_shmTargets.begin() + i);
                }

                HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
                if (snap != INVALID_HANDLE_VALUE) {
                    PROCESSENTRY32W pe = {};
                    pe.dwSize = sizeof(pe);
                    for (BOOL ok = Process32FirstW(snap, &pe); ok;
                         ok = Process32NextW(snap, &pe)) {
                        if (!IsMcProcessName(pe.szExeFile)) continue;
                        bool known = false;
                        for (auto& t : g_shmTargets)
                            if (t.pid == pe.th32ProcessID) { known = true; break; }
                        if (known) continue;

                        char name[64];
                        sprintf_s(name, "Local\\MCCombatStatus_%lu", pe.th32ProcessID);
                        HANDLE hMap = OpenFileMappingA(FILE_MAP_READ, FALSE, name);
                        if (!hMap) continue;   // not injected (yet) -> try again next scan
                        LPVOID view = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
                        if (!view) { CloseHandle(hMap); continue; }
                        // validate magic/version once; mismatched layout -> skip
                        // (a newer DLL with a different struct falls back to UDP)
                        if (ShmRead<DWORD>((const BYTE*)view, kShmOffMagic) != kShmMagic ||
                            ShmRead<DWORD>((const BYTE*)view, kShmOffVersion) != kShmVersion) {
                            UnmapViewOfFile(view);
                            CloseHandle(hMap);
                            continue;
                        }
                        g_shmTargets.push_back({ pe.th32ProcessID, hMap, (const BYTE*)view, 0 });
                        LogInject("shm: pid %lu (%ls) mapped", pe.th32ProcessID, pe.szExeFile);
                    }
                    CloseHandle(snap);
                }
            }

            // 5ms poll of every live mapping. Only feed state while the DLL
            // worker keeps ticking; a frozen tick (crashed worker) is treated
            // as stale and the shared staleness check below drops both to 0.
            // 常驻运行: 除门控状态外还持续刷新 HUD 快照 (inGame/hitType/目标名),
            // 因此无论门控开关与否都持续轮询 (5ms 等待, CPU 可忽略)。
            for (auto& t : g_shmTargets) {
                DWORD tick = ShmRead<DWORD>(t.view, kShmOffTick);
                if (tick == 0) continue;   // worker hasn't resolved JNI yet
                if (tick == t.lastTick) continue;
                t.lastTick = tick;
                LONG atk = ShmRead<LONG>(t.view, kShmOffCanAttack);
                LONG plc = ShmRead<LONG>(t.view, kShmOffCanPlace);
                g_canAttack.store(atk ? 1 : 0, std::memory_order_relaxed);
                g_canPlace.store(plc ? 1 : 0, std::memory_order_relaxed);
                g_canAttackLastMs.store((long long)GetTickCount64(),
                                        std::memory_order_relaxed);
                // ---- HUD 快照 ----
                g_shmInGame.store(ShmRead<LONG>(t.view, kShmOffInGame) ? 1 : 0,
                                  std::memory_order_relaxed);
                g_shmHitType.store((int)ShmRead<LONG>(t.view, kShmOffHitType),
                                   std::memory_order_relaxed);
                {
                    std::lock_guard<std::mutex> g(g_shmTargetMtx);
                    const BYTE* p = t.view + kShmOffTargetName;
                    size_t n = 0;
                    while (n < sizeof(g_shmTargetName) - 1 && p[n]) n++;
                    memcpy(g_shmTargetName, p, n);
                    g_shmTargetName[n] = '\0';
                }
            }

            // fail-safe shared with the UDP path: no fresh data for kStaleMs
            // -> both states fall back to 0 (宁可少点, 不可误点)
            long long last = g_canAttackLastMs.load(std::memory_order_relaxed);
            if (last != 0 && GetTickCount64() - last > kStaleMs) {
                g_canAttack.store(0, std::memory_order_relaxed);
                g_canPlace.store(0, std::memory_order_relaxed);
            }

            // 无任何发布者 (游戏已退出 / 未注入) -> 清空 HUD 快照
            if (g_shmTargets.empty()) {
                g_shmInGame.store(0, std::memory_order_relaxed);
                g_shmHitType.store(0, std::memory_order_relaxed);
                std::lock_guard<std::mutex> g(g_shmTargetMtx);
                g_shmTargetName[0] = '\0';
            }

            // 5ms cadence; a gate toggle wakes us immediately
            WaitForSingleObject(wake, 5);
        }
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
// verbose=false throttles the per-step logs on repeated retries (log spam
// when the game runs elevated and injection keeps failing).
static bool InjectDll(DWORD pid, const char* path, bool verbose)
{
    HANDLE hp = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                            PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                            FALSE, pid);
    if (!hp) {
        if (verbose) LogInject("pid %lu: OpenProcess FAILED err=%lu", pid, GetLastError());
        return false;
    }
    if (verbose) LogInject("pid %lu: OpenProcess OK", pid);

    SIZE_T len = strlen(path) + 1;
    void* mem = VirtualAllocEx(hp, nullptr, len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem) {
        if (verbose) LogInject("pid %lu: VirtualAllocEx FAILED err=%lu", pid, GetLastError());
        CloseHandle(hp);
        return false;
    }
    if (!WriteProcessMemory(hp, mem, path, len, nullptr)) {
        if (verbose) LogInject("pid %lu: WriteProcessMemory FAILED err=%lu", pid, GetLastError());
        VirtualFreeEx(hp, mem, 0, MEM_RELEASE);
        CloseHandle(hp);
        return false;
    }

    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    auto pLoadLibraryA = (LPTHREAD_START_ROUTINE)GetProcAddress(k32, "LoadLibraryA");
    HANDLE ht = CreateRemoteThread(hp, nullptr, 0, pLoadLibraryA, mem, 0, nullptr);
    if (!ht) {
        if (verbose) LogInject("pid %lu: CreateRemoteThread FAILED err=%lu (anti-injection?)",
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

    if (verbose)
        LogInject("pid %lu: remote LoadLibraryA returned 0x%p (wait=%lu)",
                  pid, (void*)(uintptr_t)ret, wait);
    bool ok = (wait != WAIT_TIMEOUT) && ret != 0;
    // timed out? the module may still have loaded - verify to avoid re-injection
    // (module check + shared-memory health: the DLL PEB-unlinks itself, so the
    // module may be invisible while the health channel is alive)
    if (!ok && (ProcessHasModule(pid, L"MCCombatStatusJni.dll") || HasHealthyShm(pid)))
        ok = true;
    return ok;
}

// ============================================================
//  injector thread: find every not-yet-injected MC Java process
// ============================================================

// V66: the DLL PEB-unlinks itself (and injector.exe manual-maps it with no
// module entry at all), so TH32CS_SNAPMODULE can no longer see it. The DLL's
// own health channel (Local\MCCombatStatus_<pid>, magic 'MCST' v7) is the
// reliable "already injected" signal - without this we would LoadLibrary a
// second copy (second SwapBuffers hook, double UDP) on every app restart.
static bool HasHealthyShm(DWORD pid)
{
    char name[64];
    sprintf_s(name, "Local\\MCCombatStatus_%lu", pid);
    HANDLE hMap = OpenFileMappingA(FILE_MAP_READ, FALSE, name);
    if (!hMap) return false;
    bool healthy = false;
    LPVOID view = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
    if (view) {
        healthy = ShmRead<DWORD>((const BYTE*)view, kShmOffMagic) == kShmMagic &&
                  ShmRead<DWORD>((const BYTE*)view, kShmOffVersion) == kShmVersion;
        UnmapViewOfFile(view);
    }
    CloseHandle(hMap);
    return healthy;
}

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

        std::set<DWORD> injected;    // PIDs we have already taken care of
        std::map<DWORD, int> skipLog; // per-PID skip log counter (log once, then throttled)
        std::map<DWORD, int> failLog; // per-PID injection failure counter (backoff + throttle)

        HANDLE wake = GateWakeEvent();
        LogInject("=== injector started, dll=%s (idle until feature enabled) ===", dllPath);

        DWORD backoffMs = 1000;   // 1s -> 2s -> 4s ... capped at 30s on repeated failures
        for (;;) {
            // Neither feature on: nothing to inject, nothing to prune - park
            // on the wake event with zero CPU until a gate is toggled.
            if (!AnyGateOn()) {
                backoffMs = 1000;
                WaitForSingleObject(wake, INFINITE);
                continue;
            }

            bool anyFail = false;
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
            if (snap != INVALID_HANDLE_VALUE) {
                PROCESSENTRY32W pe = {};
                pe.dwSize = sizeof(pe);
                for (BOOL ok = Process32FirstW(snap, &pe); ok;
                     ok = Process32NextW(snap, &pe)) {
                    if (pe.th32ProcessID == GetCurrentProcessId()) continue;
                    const wchar_t* nm = pe.szExeFile;
                    if (!IsMcProcessName(nm)) continue;
                    if (injected.count(pe.th32ProcessID)) continue;

                    int& skipC = skipLog[pe.th32ProcessID];
                    if (!ProcessIs64Bit(pe.th32ProcessID)) {
                        if (skipC++ == 0)
                            LogInject("pid %lu (%ls): skipped - not x64", pe.th32ProcessID, nm);
                        continue;
                    }
                    if (!HasMinecraftWindow(pe.th32ProcessID)) {
                        if (skipC++ == 0) {
                            LogInject("pid %lu (%ls): skipped - no MC window",
                                      pe.th32ProcessID, nm);
                            LogProcessWindows(pe.th32ProcessID);
                        }
                        continue;
                    }
                    if (ProcessHasModule(pe.th32ProcessID, L"MCCombatStatusJni.dll") ||
                        HasHealthyShm(pe.th32ProcessID)) {
                        injected.insert(pe.th32ProcessID);   // already loaded
                        if (skipC++ == 0)
                            LogInject("pid %lu (%ls): already loaded, marked",
                                      pe.th32ProcessID, nm);
                        continue;
                    }

                    // repeated failures: log the first attempt and every 16th
                    // (each with full per-step detail), then back off
                    int& failC = failLog[pe.th32ProcessID];
                    ++failC;
                    bool verbose = (failC == 1) || (failC % 16 == 0);
                    if (verbose)
                        LogInject("pid %lu (%ls): injecting (attempt %d)...",
                                  pe.th32ProcessID, nm, failC);
                    if (InjectDll(pe.th32ProcessID, dllPath, verbose)) {
                        injected.insert(pe.th32ProcessID);
                        skipLog.erase(pe.th32ProcessID);
                        failLog.erase(pe.th32ProcessID);
                        LogInject("pid %lu: injection OK", pe.th32ProcessID);
                    } else {
                        // failure (elevation etc.): do NOT mark - retry later
                        anyFail = true;
                    }
                }
                CloseHandle(snap);
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

            // schedule the next scan: 1s normally; exponential backoff (1s->30s)
            // after failures so a blocked game is not hammered with retries.
            // The wait is interruptible: a gate toggle wakes us immediately.
            if (anyFail) {
                WaitForSingleObject(wake, backoffMs);
                if (backoffMs < 30000) backoffMs *= 2;
            } else {
                backoffMs = 1000;
                WaitForSingleObject(wake, 1000);
            }
        }
    }).detach();
}
