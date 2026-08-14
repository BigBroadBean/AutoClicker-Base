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
#include <algorithm>
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

// ============================================================
//  payload (V67): 内嵌加密 DLL 资源, 直接内存解密后手动映射注入。
//  不再解出到 %TEMP%、不再 LoadLibrary —— 磁盘零痕迹、进程内无模块条目。
//  (网易客户端反作弊会扫描桌面文件与后台进程可执行文件, 见 DEVELOPMENT.md)
// ============================================================
static BYTE* LoadEmbeddedPayload(size_t* outLen)
{
    HRSRC hr = FindResourceA(nullptr, MAKEINTRESOURCEA(IDR_MC_DLL), RT_RCDATA);
    if (!hr) return nullptr;
    HGLOBAL hg = LoadResource(nullptr, hr);
    if (!hg) return nullptr;
    const BYTE* p = (const BYTE*)LockResource(hg);
    DWORD sz = SizeofResource(nullptr, hr);
    if (!p || sz == 0) return nullptr;
    BYTE* buf = (BYTE*)VirtualAlloc(nullptr, sz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!buf) return nullptr;
    for (DWORD i = 0; i < sz; i++) buf[i] = (BYTE)(p[i] ^ 0x5A);   // 构建期 XOR 0x5A
    *outLen = sz;
    return buf;
}

// sidecar 明文 DLL (exe 同目录 / CWD) 优先 —— 便于不重编译更新 DLL;
// 只读入内存用于手动映射, 仍然不落盘、不 LoadLibrary。
static BYTE* LoadSidecarPayload(size_t* outLen)
{
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) *(slash + 1) = '\0';
    strcat_s(path, "MCCombatStatusJni.dll");
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
        GetCurrentDirectoryA(MAX_PATH, path);
        size_t len = strlen(path);
        if (len > 0 && path[len - 1] != '\\') strcat_s(path, "\\");
        strcat_s(path, "MCCombatStatusJni.dll");
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) return nullptr;
    }
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return nullptr;
    DWORD sz = GetFileSize(f, nullptr);
    BYTE* buf = (BYTE*)VirtualAlloc(nullptr, sz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (buf) {
        DWORD rd = 0;
        ReadFile(f, buf, sz, &rd, nullptr);
        if (rd != sz) { VirtualFree(buf, 0, MEM_RELEASE); buf = nullptr; }
    }
    CloseHandle(f);
    if (buf) *outLen = sz;
    return buf;
}

static BYTE* LoadPayload(size_t* outLen)
{
    BYTE* p = LoadSidecarPayload(outLen);
    if (p) return p;
    return LoadEmbeddedPayload(outLen);
}

bool CanAttackDllAvailable()
{
    HRSRC hr = FindResourceA(nullptr, MAKEINTRESOURCEA(IDR_MC_DLL), RT_RCDATA);
    if (hr) return true;
    size_t n = 0;
    BYTE* p = LoadSidecarPayload(&n);
    if (p) { VirtualFree(p, 0, MEM_RELEASE); return true; }
    return false;
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

// ============================================================
//  手动映射注入 (V67, 移植自上游 injector.cpp 的 ManualMap)
//  不调用 LoadLibrary: 无 LoadImage 回调、PEB 模块链表无条目、不落盘。
//  处理: RVA 布局拷贝 / DIR64 重定位 / MinGW 伪重定位 (COFF 符号表) /
//  .rdata 绝对指针启发式补修 / 导入表本地解析 / 入口存根线程。
// ============================================================
struct CoffSym {
    char   name[8];
    DWORD  value;
    SHORT  section;
    WORD   type;
    BYTE   sclass;
    BYTE   naux;
};

static const char* CoffNameOf(const CoffSym* s, const char* strtab)
{
    if (s->name[0] == 0 && s->name[1] == 0 && s->name[2] == 0 && s->name[3] == 0) {
        DWORD off = *(DWORD*)(s->name + 4);
        return strtab + off;
    }
    static char buf[9];
    memcpy(buf, s->name, 8);
    buf[8] = 0;
    return buf;
}

static bool ApplyPseudoRelocs(HANDLE proc, BYTE* base, const BYTE* workImg,
                              const BYTE* imgFile, size_t imgSize)
{
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)imgFile;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return true;
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(imgFile + dos->e_lfanew);
    DWORD symOff = nt->FileHeader.PointerToSymbolTable;
    DWORD symNum = nt->FileHeader.NumberOfSymbols;
    if (!symOff || !symNum) return true;
    if (symOff + (size_t)symNum * 18 + 4 > imgSize) return true;
    const CoffSym* syms = (const CoffSym*)(imgFile + symOff);
    const char* strtab = (const char*)(imgFile + symOff + (size_t)symNum * 18);

    DWORD listRva = 0, endRva = 0;
    for (DWORD i = 0; i < symNum; i++) {
        const CoffSym* s = &syms[i];
        const char* nm = CoffNameOf(s, strtab);
        if (strcmp(nm, "___RUNTIME_PSEUDO_RELOC_LIST__") == 0)
            listRva = s->value;
        else if (strcmp(nm, "___RUNTIME_PSEUDO_RELOC_LIST_END__") == 0)
            endRva = s->value;
        i += s->naux;
    }
    if (!listRva || !endRva || endRva <= listRva) return true;

    for (DWORD rva = listRva; rva + 24 <= endRva; rva += 24) {
        DWORD_PTR sym, target, addend;
        memcpy(&sym,    workImg + rva,      8);
        memcpy(&target, workImg + rva + 8,  8);
        memcpy(&addend, workImg + rva + 16, 8);
        DWORD_PTR val = (DWORD_PTR)base + sym + addend;
        if (!WriteProcessMemory(proc, base + target, &val, 8, nullptr))
            return false;
    }
    return true;
}

static bool ManualMapInject(DWORD pid, const BYTE* imgFile, size_t imgSize, bool verbose,
                            ULONGLONG* outBase = nullptr, ULONGLONG* outEntry = nullptr,
                            bool runEntry = true)
{
    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                              PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                              FALSE, pid);
    if (!proc) {
        if (verbose) LogInject("pid %lu: OpenProcess FAILED err=%lu", pid, GetLastError());
        return false;
    }
    BYTE* base = nullptr;
    BYTE* work = nullptr;
    void* stubAddr = nullptr;
    HANDLE th = nullptr;
    bool ok = false;

    if (imgSize < sizeof(IMAGE_DOS_HEADER)) goto fail;
    if (((IMAGE_DOS_HEADER*)imgFile)->e_magic != IMAGE_DOS_SIGNATURE) goto fail;
    {
        IMAGE_NT_HEADERS64* ntF = (IMAGE_NT_HEADERS64*)(imgFile + ((IMAGE_DOS_HEADER*)imgFile)->e_lfanew);
        if (ntF->Signature != IMAGE_NT_SIGNATURE) goto fail;

        DWORD sizeOfImage = ntF->OptionalHeader.SizeOfImage;
        DWORD sizeOfHeaders = ntF->OptionalHeader.SizeOfHeaders;
        ULONGLONG prefBase = ntF->OptionalHeader.ImageBase;

        // RVA 布局工作缓冲: 节区文件偏移 ≠ RVA, 重定位/导入/伪重定位解析必须
        // 在 RVA 布局上做 (否则 .reloc/.idata 读到错误内容)。
        work = (BYTE*)VirtualAlloc(nullptr, sizeOfImage, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!work) goto fail;
        memset(work, 0, sizeOfImage);
        {
            size_t hdrN = imgSize < sizeOfHeaders ? imgSize : sizeOfHeaders;
            memcpy(work, imgFile, hdrN);
            IMAGE_SECTION_HEADER* secW = IMAGE_FIRST_SECTION(ntF);
            for (int i = 0; i < ntF->FileHeader.NumberOfSections; i++) {
                if (secW[i].SizeOfRawData && secW[i].VirtualAddress < sizeOfImage &&
                    secW[i].PointerToRawData < imgSize) {
                    size_t n = secW[i].SizeOfRawData;
                    if (secW[i].PointerToRawData + n > imgSize) n = imgSize - secW[i].PointerToRawData;
                    if (secW[i].VirtualAddress + n > sizeOfImage) n = sizeOfImage - secW[i].VirtualAddress;
                    memcpy(work + secW[i].VirtualAddress, imgFile + secW[i].PointerToRawData, n);
                }
            }
        }
        const BYTE* img = work;   // RVA 布局视图
        IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)img;
        IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(img + dos->e_lfanew);
        IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);

        // 1. 远端分配 RW
        base = (BYTE*)VirtualAllocEx(proc, nullptr, sizeOfImage,
                                     MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!base) { if (verbose) LogInject("pid %lu: VirtualAllocEx FAILED err=%lu", pid, GetLastError()); goto fail; }

        // 2. 头 + 节区 (从文件缓冲按文件偏移读; RVA 缓冲不能用于此)
        if (!WriteProcessMemory(proc, base, imgFile, sizeOfHeaders, nullptr)) goto fail;
        for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
            if (sec[i].SizeOfRawData &&
                !WriteProcessMemory(proc, base + sec[i].VirtualAddress,
                                    imgFile + sec[i].PointerToRawData,
                                    sec[i].SizeOfRawData, nullptr)) goto fail;
        }

        // 3. DIR64 重定位
        ULONGLONG delta = (ULONGLONG)base - prefBase;
        if (delta) {
            IMAGE_DATA_DIRECTORY& reloc = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
            DWORD off = 0;
            while (off + sizeof(IMAGE_BASE_RELOCATION) <= reloc.Size) {
                DWORD blkFile = reloc.VirtualAddress + off;
                if (blkFile + sizeof(IMAGE_BASE_RELOCATION) > sizeOfImage) break;
                IMAGE_BASE_RELOCATION* blk = (IMAGE_BASE_RELOCATION*)(img + blkFile);
                if (blk->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION)) break;
                if ((ULONGLONG)reloc.VirtualAddress + off + blk->SizeOfBlock > sizeOfImage) break;
                DWORD count = (blk->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / 2;
                WORD* items = (WORD*)((BYTE*)blk + sizeof(IMAGE_BASE_RELOCATION));
                for (DWORD i = 0; i < count; i++) {
                    if ((items[i] >> 12) == IMAGE_REL_BASED_DIR64) {
                        DWORD rva = blk->VirtualAddress + (items[i] & 0xFFF);
                        if (rva + 8 > sizeOfImage) continue;
                        ULONGLONG val = *(ULONGLONG*)(img + rva) + delta;
                        if (!WriteProcessMemory(proc, base + rva, &val, 8, nullptr)) goto fail;
                    }
                }
                off += blk->SizeOfBlock;
            }
        }

        // 3b. MinGW .rdata 绝对指针启发式补修 (无标准 reloc 条目, 期望首选基址)
        {
            std::vector<DWORD> covered;
            covered.reserve(2048);
            {
                IMAGE_DATA_DIRECTORY& reloc2 = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
                DWORD off2 = 0;
                while (off2 + sizeof(IMAGE_BASE_RELOCATION) <= reloc2.Size) {
                    IMAGE_BASE_RELOCATION* blk = (IMAGE_BASE_RELOCATION*)(img + reloc2.VirtualAddress + off2);
                    if (!blk->SizeOfBlock || blk->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION)) break;
                    DWORD count = (blk->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / 2;
                    WORD* items = (WORD*)((BYTE*)blk + sizeof(IMAGE_BASE_RELOCATION));
                    for (DWORD i = 0; i < count; i++)
                        if ((items[i] >> 12) == IMAGE_REL_BASED_DIR64)
                            covered.push_back(blk->VirtualAddress + (items[i] & 0xFFF));
                    off2 += blk->SizeOfBlock;
                }
            }
            std::sort(covered.begin(), covered.end());
            for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
                DWORD ch = sec[i].Characteristics;
                if (!(ch & IMAGE_SCN_MEM_READ) || (ch & IMAGE_SCN_CNT_CODE)) continue;
                DWORD va = sec[i].VirtualAddress;
                DWORD vs = sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : sec[i].SizeOfRawData;
                if (va + vs > sizeOfImage) vs = sizeOfImage - va;
                for (DWORD o = 0; o + 8 <= vs; o++) {
                    if (std::binary_search(covered.begin(), covered.end(), va + o)) continue;
                    ULONGLONG v = *(ULONGLONG*)(img + va + o);
                    if (v >= prefBase && v < prefBase + sizeOfImage) {
                        ULONGLONG nv = v + delta;
                        WriteProcessMemory(proc, base + va + o, &nv, 8, nullptr);
                    }
                }
            }
        }

        // 4. 导入表: 系统 DLL 基址全局一致, 本地解析直接填 IAT
        {
            IMAGE_DATA_DIRECTORY& imp = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
            DWORD off = 0;
            while (off + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= imp.Size) {
                IMAGE_IMPORT_DESCRIPTOR* desc = (IMAGE_IMPORT_DESCRIPTOR*)(img + imp.VirtualAddress + off);
                if (!desc->Name && !desc->FirstThunk) break;
                const char* dllName = (const char*)(img + desc->Name);
                HMODULE mod = GetModuleHandleA(dllName);
                if (!mod) mod = LoadLibraryA(dllName);
                if (!mod) { if (verbose) LogInject("pid %lu: 依赖模块未加载: %s", pid, dllName); goto fail; }
                ULONGLONG* origThunk = (ULONGLONG*)(img + (desc->OriginalFirstThunk
                                                            ? desc->OriginalFirstThunk : desc->FirstThunk));
                ULONGLONG* iat = (ULONGLONG*)(img + desc->FirstThunk);
                for (int i = 0; origThunk[i]; i++) {
                    ULONGLONG val = 0;
                    if (origThunk[i] & 0x8000000000000000ULL) {
                        val = (ULONGLONG)(ULONG_PTR)GetProcAddress(mod, (LPCSTR)(origThunk[i] & 0xFFFF));
                    } else {
                        IMAGE_IMPORT_BY_NAME* ibn = (IMAGE_IMPORT_BY_NAME*)(img + origThunk[i]);
                        val = (ULONGLONG)(ULONG_PTR)GetProcAddress(mod, ibn->Name);
                    }
                    if (!val) { if (verbose) LogInject("pid %lu: 导入解析失败: %s", pid, dllName); goto fail; }
                    if (!WriteProcessMemory(proc, base + desc->FirstThunk + i * 8, &val, 8, nullptr)) goto fail;
                }
                off += sizeof(IMAGE_IMPORT_DESCRIPTOR);
            }
        }

        // 4.5 MinGW 伪重定位
        if (!ApplyPseudoRelocs(proc, base, img, imgFile, imgSize)) goto fail;

        // 5. 入口存根: mov rcx,base; mov edx,1; xor r8,r8; mov rax,entry; jmp rax
        ULONGLONG entry = (ULONGLONG)base + nt->OptionalHeader.AddressOfEntryPoint;
        {
            BYTE stub[64];
            size_t sn = 0;
            stub[sn++] = 0x48; stub[sn++] = 0xB9;
            memcpy(stub + sn, &base, 8);  sn += 8;
            stub[sn++] = 0xBA; stub[sn++] = 0x01; stub[sn++] = 0x00;
            stub[sn++] = 0x00; stub[sn++] = 0x00;
            stub[sn++] = 0x4D; stub[sn++] = 0x31; stub[sn++] = 0xC0;
            stub[sn++] = 0x48; stub[sn++] = 0xB8;
            memcpy(stub + sn, &entry, 8); sn += 8;
            stub[sn++] = 0xFF; stub[sn++] = 0xE0;

            stubAddr = VirtualAllocEx(proc, nullptr, sizeof(stub),
                                      MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
            if (!stubAddr) goto fail;
            if (!WriteProcessMemory(proc, stubAddr, stub, sn, nullptr)) goto fail;
            FlushInstructionCache(proc, stubAddr, (SIZE_T)sn);
        }
        FlushInstructionCache(proc, base, sizeOfImage);

        // 5.5 按节区属性设保护 (代码 RX / 数据 RW), 否则 NX 杀掉线程
        for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
            DWORD ch = sec[i].Characteristics;
            DWORD prot = PAGE_READONLY;
            if (ch & IMAGE_SCN_MEM_EXECUTE) prot = PAGE_EXECUTE_READ;
            if (ch & IMAGE_SCN_MEM_WRITE)   prot = (ch & IMAGE_SCN_MEM_EXECUTE)
                                                   ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
            DWORD old = 0;
            VirtualProtectEx(proc, base + sec[i].VirtualAddress,
                             sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize : 1,
                             prot, &old);
        }

        // 6. 执行 DllMain (runEntry=false 时仅返回基址/入口, 由线程劫持执行)
        if (runEntry) {
            th = CreateRemoteThread(proc, nullptr, 0, (LPTHREAD_START_ROUTINE)stubAddr, nullptr, 0, nullptr);
            if (!th) { if (verbose) LogInject("pid %lu: CreateRemoteThread FAILED err=%lu", pid, GetLastError()); goto fail; }
            DWORD wait = WaitForSingleObject(th, 10000);
            DWORD exitCode = 0;
            GetExitCodeThread(th, &exitCode);
            CloseHandle(th);
            th = nullptr;
            ok = (wait != WAIT_TIMEOUT) && exitCode == 1;
            if (verbose) LogInject("pid %lu: DllMain 返回 %lu (wait=%lu)", pid, exitCode, wait);
            if (!ok && HasHealthyShm(pid)) ok = true;   // DllMain 已就绪但线程信号异常
        } else {
            ok = true;   // 只映射, 不执行
        }
        if (ok) {
            if (outBase)  *outBase  = (ULONGLONG)base;
            if (outEntry) *outEntry = (ULONGLONG)base + nt->OptionalHeader.AddressOfEntryPoint;
        }
    }

fail:
    if (th) CloseHandle(th);
    if (stubAddr) VirtualFreeEx(proc, stubAddr, 0, MEM_RELEASE);
    if (!ok && base) VirtualFreeEx(proc, base, 0, MEM_RELEASE);
    if (work) VirtualFree(work, 0, MEM_RELEASE);
    CloseHandle(proc);
    return ok;
}

// ============================================================
//  V68: 线程劫持执行 DllMain —— 不创建新线程 (无 NtCreateThreadEx 痕迹)。
//  挂起游戏窗口线程, 完整保存现场 (GPR/RFLAGS/XMM), 执行 DllMain,
//  完整恢复现场后跳回原 RIP。壳代码写在映像头页 0x800。
// ============================================================
static size_t StubPut(BYTE* p, const BYTE* b, size_t n) { if (p) memcpy(p, b, n); return n; }
static size_t StubPutImm64(BYTE* p, ULONGLONG v) { if (p) memcpy(p, &v, 8); return 8; }

static size_t BuildHijackStub(BYTE* out, ULONGLONG base, ULONGLONG entry, ULONGLONG origRip)
{
    size_t n = 0;
    static const BYTE save1[] = {
        0x55,
        0x50, 0x51, 0x52, 0x53, 0x56, 0x57,
        0x41,0x50, 0x41,0x51, 0x41,0x52, 0x41,0x53,
        0x41,0x54, 0x41,0x55, 0x41,0x56, 0x41,0x57,
        0x9C,
    };
    n += StubPut(out ? out + n : nullptr, save1, sizeof(save1));
    static const BYTE sub1[] = { 0x48,0x81,0xEC, 0x00,0x01,0x00,0x00 };
    n += StubPut(out ? out + n : nullptr, sub1, sizeof(sub1));
    for (int i = 0; i < 16; i++) {
        BYTE b[8]; size_t k = 0;
        b[k++] = 0xF3;
        if (i >= 8) b[k++] = 0x44;
        b[k++] = 0x0F; b[k++] = 0x7F;
        b[k++] = (BYTE)(0x44 | ((i & 7) << 3));
        b[k++] = 0x24; b[k++] = (BYTE)(i * 16);
        n += StubPut(out ? out + n : nullptr, b, k);
    }
    static const BYTE anchor[] = { 0x48,0x89,0xE3 };
    n += StubPut(out ? out + n : nullptr, anchor, sizeof(anchor));
    static const BYTE align1[] = { 0x48,0x83,0xEC,0x20, 0x48,0x83,0xE4,0xF0 };
    n += StubPut(out ? out + n : nullptr, align1, sizeof(align1));
    n += StubPut(out ? out + n : nullptr, (const BYTE*)"\x48\xB9", 2);
    n += StubPutImm64(out ? out + n : nullptr, base);
    static const BYTE args1[] = { 0xBA,0x01,0x00,0x00,0x00, 0x45,0x31,0xC0, 0x48,0xB8 };
    n += StubPut(out ? out + n : nullptr, args1, sizeof(args1));
    n += StubPutImm64(out ? out + n : nullptr, entry);
    static const BYTE call1[] = { 0xFF,0xD0, 0x48,0x89,0xDC };
    n += StubPut(out ? out + n : nullptr, call1, sizeof(call1));
    for (int i = 0; i < 16; i++) {
        BYTE b[8]; size_t k = 0;
        b[k++] = 0xF3;
        if (i >= 8) b[k++] = 0x44;
        b[k++] = 0x0F; b[k++] = 0x6F;
        b[k++] = (BYTE)(0x44 | ((i & 7) << 3));
        b[k++] = 0x24; b[k++] = (BYTE)(i * 16);
        n += StubPut(out ? out + n : nullptr, b, k);
    }
    static const BYTE add1[] = { 0x48,0x81,0xC4, 0x00,0x01,0x00,0x00 };
    n += StubPut(out ? out + n : nullptr, add1, sizeof(add1));
    static const BYTE restore1[] = {
        0x9D,
        0x41,0x5F, 0x41,0x5E, 0x41,0x5D, 0x41,0x5C,
        0x41,0x5B, 0x41,0x5A, 0x41,0x59, 0x41,0x58,
        0x5F, 0x5E, 0x5B, 0x5A, 0x59, 0x58, 0x5D,
    };
    n += StubPut(out ? out + n : nullptr, restore1, sizeof(restore1));
    static const BYTE ret1[] = { 0xFF,0x35, 0x02,0x00,0x00,0x00, 0xC3, 0x90 };
    n += StubPut(out ? out + n : nullptr, ret1, sizeof(ret1));
    n += StubPutImm64(out ? out + n : nullptr, origRip);
    return n;
}

static DWORD FindWindowThread(DWORD pid)
{
    struct Ctx { DWORD pid; DWORD tid; };
    Ctx c = { pid, 0 };
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        Ctx* x = (Ctx*)lp;
        DWORD p = 0;
        GetWindowThreadProcessId(h, &p);
        if (p == x->pid && IsWindowVisible(h)) {
            x->tid = GetWindowThreadProcessId(h, nullptr);
            return FALSE;
        }
        return TRUE;
    }, (LPARAM)&c);
    return c.tid;
}

static bool HijackRunDll(DWORD pid, ULONGLONG base, ULONGLONG entry, bool verbose)
{
    HANDLE proc = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_QUERY_INFORMATION,
                              FALSE, pid);
    if (!proc) return false;
    DWORD tid = FindWindowThread(pid);
    if (!tid) { CloseHandle(proc); return false; }
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                           THREAD_SET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION,
                           FALSE, tid);
    if (!th) { CloseHandle(proc); return false; }
    CONTEXT ctx = {};
    ctx.ContextFlags = CONTEXT_FULL;
    if (SuspendThread(th) == (DWORD)-1) { CloseHandle(th); CloseHandle(proc); return false; }
    if (!GetThreadContext(th, &ctx)) { ResumeThread(th); CloseHandle(th); CloseHandle(proc); return false; }

    ULONGLONG stubAddr = base + 0x800;
    BYTE stub[600];
    size_t n = BuildHijackStub(stub, base, entry, ctx.Rip);
    if (!WriteProcessMemory(proc, (void*)stubAddr, stub, n, nullptr)) {
        ResumeThread(th); CloseHandle(th); CloseHandle(proc); return false;
    }
    FlushInstructionCache(proc, (void*)stubAddr, n);
    {
        DWORD oldp = 0;
        VirtualProtectEx(proc, (void*)base, 0x1000, PAGE_EXECUTE_READWRITE, &oldp);
    }

    CONTEXT newCtx = ctx;
    newCtx.Rip = stubAddr;
    newCtx.EFlags &= ~0x100;
    if (!SetThreadContext(th, &newCtx)) { ResumeThread(th); CloseHandle(th); CloseHandle(proc); return false; }
    ResumeThread(th);

    bool done = false;
    for (int i = 0; i < 100; i++) {
        Sleep(50);
        if (HasHealthyShm(pid)) { done = true; break; }
    }
    if (!done) {
        SuspendThread(th);
        SetThreadContext(th, &ctx);
        ResumeThread(th);
        CloseHandle(th);
        CloseHandle(proc);
        return false;
    }
    Sleep(150);
    DWORD oldp = 0;
    VirtualProtectEx(proc, (void*)base, 0x1000, PAGE_READONLY, &oldp);
    if (verbose) LogInject("pid %lu: 线程劫持执行 DllMain 成功 (tid %lu)", pid, tid);
    CloseHandle(th);
    CloseHandle(proc);
    return true;
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

        // V67: 载荷只存在于内存 (内嵌加密资源 / 可选 sidecar 明文文件),
        // 不解出到磁盘、不 LoadLibrary —— 手动映射注入。
        size_t payloadLen = 0;
        BYTE* payload = LoadPayload(&payloadLen);
        while (!payload) {
            Sleep(1000);
            payload = LoadPayload(&payloadLen);
        }

        std::set<DWORD> injected;    // PIDs we have already taken care of
        std::map<DWORD, int> skipLog; // per-PID skip log counter (log once, then throttled)
        std::map<DWORD, int> failLog; // per-PID injection failure counter (backoff + throttle)

        HANDLE wake = GateWakeEvent();
        LogInject("=== injector started, payload=%zu bytes, manual map (idle until feature enabled) ===",
                  payloadLen);

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
                    // V68: 先手动映射 (不执行) + 线程劫持执行 DllMain (不创建
                    // 新线程); 劫持不可用时回退传统远程线程执行入口。
                    ULONGLONG mBase = 0, mEntry = 0;
                    bool injectedOk = ManualMapInject(pe.th32ProcessID, payload, payloadLen,
                                                      verbose, &mBase, &mEntry, false);
                    if (injectedOk) {
                        injectedOk = HijackRunDll(pe.th32ProcessID, mBase, mEntry, verbose);
                        if (!injectedOk) {
                            // 映像已就位: 直接远程线程跑入口 (g_attached 幂等防重)
                            BYTE st2[64];
                            size_t sn2 = 0;
                            st2[sn2++] = 0x48; st2[sn2++] = 0xB9;
                            memcpy(st2 + sn2, &mBase, 8); sn2 += 8;
                            st2[sn2++] = 0xBA; st2[sn2++] = 0x01; st2[sn2++] = 0x00;
                            st2[sn2++] = 0x00; st2[sn2++] = 0x00;
                            st2[sn2++] = 0x4D; st2[sn2++] = 0x31; st2[sn2++] = 0xC0;
                            st2[sn2++] = 0x48; st2[sn2++] = 0xB8;
                            memcpy(st2 + sn2, &mEntry, 8); sn2 += 8;
                            st2[sn2++] = 0xFF; st2[sn2++] = 0xE0;
                            HANDLE hp2 = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                                     PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                                                     FALSE, pe.th32ProcessID);
                            if (hp2) {
                                void* stA = VirtualAllocEx(hp2, nullptr, 64,
                                                           MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
                                if (stA) {
                                    WriteProcessMemory(hp2, stA, st2, sn2, nullptr);
                                    FlushInstructionCache(hp2, stA, sn2);
                                    HANDLE ht2 = CreateRemoteThread(hp2, nullptr, 0,
                                                                    (LPTHREAD_START_ROUTINE)stA, nullptr, 0, nullptr);
                                    if (ht2) {
                                        DWORD ec2 = 0;
                                        WaitForSingleObject(ht2, 10000);
                                        GetExitCodeThread(ht2, &ec2);
                                        CloseHandle(ht2);
                                        injectedOk = (ec2 == 1) || HasHealthyShm(pe.th32ProcessID);
                                    }
                                }
                                CloseHandle(hp2);
                            }
                        }
                    }
                    if (injectedOk) {
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
