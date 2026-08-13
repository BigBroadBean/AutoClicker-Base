#include "config.h"
#include "clicker.h"
#include "canattack.h"

#include <Windows.h>
#include <ShlObj.h>
#include <fstream>
#include <string>
#include <cstdio>
#include <mutex>

// UI 线程（控件操作）与连点线程（热键切换/定时停止）都会读写配置，
// 必须串行化，否则两处同时 trunc+写会把文件写花。
static std::mutex g_cfgLock;

int g_activeProfile = 1;
std::wstring g_profileNames[PROFILE_COUNT] = {
    L"\x65b9\x6848\x31", L"\x65b9\x6848\x32", L"\x65b9\x6848\x33", L"\x65b9\x6848\x34"
};   // 方案1..方案4

// ---- 路径 ----
static std::string AppDir()
{
    char appdata[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) {
        std::string dir = std::string(appdata) + "\\AutoClicker";
        CreateDirectoryA(dir.c_str(), nullptr);
        return dir;
    }
    return ".";
}

static std::string GetConfigPath()       { return AppDir() + "\\autoclickerSave.txt"; }   // 旧版单文件 (仅迁移用)
static std::string ProfilePath(int n)    { return AppDir() + "\\profile_" + std::to_string(n) + ".txt"; }
static std::string ActivePath()          { return AppDir() + "\\active.txt"; }
static std::string UiPath()              { return AppDir() + "\\ui.txt"; }
static std::string WindowPath()          { return AppDir() + "\\window.txt"; }

static bool FileExists(const std::string& p)
{
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// ============================================================
//  方案内设置: 行顺序固定 (旧文件兼容: 新增行永远追加在末尾)
// ============================================================
// 行 01 cpsLeft10       (5..1000)
// 行 02 cpsRight10      (5..1000)
// 行 03 cpsMax          (20..500)
// 行 04 randomCpsEnabled
// 行 05 randomCpsRange  (1..5)
// 行 06 vk_key          (0..255)
// 行 07 leftenabled
// 行 08 rightenabled
// 行 09 keepClicke
// 行 10 vk_multi_key    (0..255)
// 行 11 multiMul        (1..5)
// 行 12 multiDelayMs    (1..200)
// 行 13 vk_scroll_key   (0..255)
// 行 14 scrollClickButton
// 行 15 vk_scroll_lr_key(0..255)
// 行 16 theme
// 行 17 autoStopEnabled
// 行 18 autoStopSeconds (1..3600)
// 行 19 topmost
// 行 20 canAttackOnlyClick
// 行 21 vk_canattack_key(0..255)
// 行 22 placeOnlyRightClick
// 行 23 vk_place_key    (0..255)
// 行 24 humanizeMode    (0..3: 0=均匀 1=双击 2=呼吸 3=疲劳)
// 行 25 humanizeLevel   (1..5)
// 行 26 accentIdx       (0..3)
// 行 27 vk_profile_key  (0..255)
// 行 28 soundEnabled    (提示音总开关)

static bool NextInt(std::ifstream& f, int& v)
{
    std::string line;
    if (!std::getline(f, line)) return false;
    return sscanf_s(line.c_str(), "%d", &v) == 1;
}

// 从 path 读入全部全局设置 (调用者已持锁)
static void LoadUnlocked(const std::string& path)
{
    std::ifstream file(path);
    if (!file.is_open()) return;

    int v;
    if (NextInt(file, v) && v >= 5 && v <= 1000)
        cpsLeft10 = v;
    if (NextInt(file, v) && v >= 5 && v <= 1000)
        cpsRight10 = v;
    if (NextInt(file, v) && v >= 20 && v <= 500)
        cpsMax = v;
    if (NextInt(file, v))
        randomCpsEnabled = (v != 0);
    if (NextInt(file, v) && v >= 1 && v <= 5)
        randomCpsRange = v;
    if (NextInt(file, v) && v >= 0 && v <= 255)
        vk_key = v;
    if (NextInt(file, v))
        leftenabled = (v != 0);
    if (NextInt(file, v))
        rightenabled = (v != 0);
    if (NextInt(file, v))
        keepClicke = (v != 0);
    if (NextInt(file, v) && v >= 0 && v <= 255)
        vk_multi_key = v;
    if (NextInt(file, v) && v >= 1 && v <= 5)
        multiMul = v;
    if (NextInt(file, v) && v >= 1 && v <= 200)
        multiDelayMs = v;
    if (NextInt(file, v) && v >= 0 && v <= 255)
        vk_scroll_key = v;
    if (NextInt(file, v))
        scrollClickButton = (v != 0) ? 1 : 0;
    if (NextInt(file, v) && v >= 0 && v <= 255)
        vk_scroll_lr_key = v;
    if (NextInt(file, v))
        g_theme = (v != 0) ? Theme::Light : Theme::Dark;
    if (NextInt(file, v))
        autoStopEnabled = (v != 0);
    if (NextInt(file, v) && v >= 1 && v <= 3600)
        autoStopSeconds = v;
    if (NextInt(file, v))
        topmost = (v != 0);
    if (NextInt(file, v))
        canAttackOnlyClick = (v != 0);
    if (NextInt(file, v) && v >= 0 && v <= 255)
        vk_canattack_key = v;
    if (NextInt(file, v))
        placeOnlyRightClick = (v != 0);
    if (NextInt(file, v) && v >= 0 && v <= 255)
        vk_place_key = v;
    // ---- 2.6+ 新增 ----
    if (NextInt(file, v) && v >= 0 && v <= 3)
        humanizeMode = v;
    if (NextInt(file, v) && v >= 1 && v <= 5)
        humanizeLevel = v;
    if (NextInt(file, v) && v >= 0 && v < ACCENT_COUNT)
        g_accentIdx = v;
    if (NextInt(file, v) && v >= 0 && v <= 255)
        vk_profile_key = v;
    if (NextInt(file, v))
        soundEnabled = (v != 0);

    leftms = cpsToMs(cpsLeft10);
    rightms = cpsToMs(cpsRight10);
}

// 把全部全局设置写入 path (调用者已持锁)
static void SaveUnlocked(const std::string& path)
{
    std::ofstream file(path, std::ios::trunc);
    if (!file.is_open()) return;

    file << cpsLeft10 << "\n"
         << cpsRight10 << "\n"
         << cpsMax << "\n"
         << (randomCpsEnabled ? 1 : 0) << "\n"
         << randomCpsRange << "\n"
         << vk_key << "\n"
         << (leftenabled ? 1 : 0) << "\n"
         << (rightenabled ? 1 : 0) << "\n"
         << (keepClicke ? 1 : 0) << "\n"
         << vk_multi_key << "\n"
         << multiMul << "\n"
         << multiDelayMs << "\n"
         << vk_scroll_key << "\n"
         << scrollClickButton << "\n"
         << vk_scroll_lr_key << "\n"
         << (g_theme == Theme::Light ? 1 : 0) << "\n"
         << (autoStopEnabled ? 1 : 0) << "\n"
         << autoStopSeconds << "\n"
         << (topmost ? 1 : 0) << "\n"
         << (canAttackOnlyClick ? 1 : 0) << "\n"
         << vk_canattack_key << "\n"
         << (placeOnlyRightClick ? 1 : 0) << "\n"
         << vk_place_key << "\n"
         << humanizeMode << "\n"
         << humanizeLevel << "\n"
         << g_accentIdx << "\n"
         << vk_profile_key << "\n"
         << (soundEnabled ? 1 : 0) << "\n";
}

static void ReadActiveUnlocked()
{
    std::ifstream f(ActivePath());
    std::string line;
    if (std::getline(f, line)) {
        int v = atoi(line.c_str());
        if (v >= 1 && v <= PROFILE_COUNT) g_activeProfile = v;
    }
}

static void WriteActiveUnlocked()
{
    std::ofstream f(ActivePath(), std::ios::trunc);
    if (f.is_open()) f << g_activeProfile << "\n";
}

// ---- UTF-8 <-> UTF-16 辅助 ----
static std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return L"";
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return "";
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

void LoadUiState()
{
    std::lock_guard<std::mutex> g(g_cfgLock);
    std::ifstream f(UiPath());
    if (!f.is_open()) return;
    std::string line;
    for (int i = 0; i < PROFILE_COUNT && std::getline(f, line); i++) {
        std::wstring w = Utf8ToWide(line);
        if (!w.empty()) g_profileNames[i] = w;
    }
}

void SaveUiState()
{
    std::lock_guard<std::mutex> g(g_cfgLock);
    std::ofstream f(UiPath(), std::ios::trunc);
    if (!f.is_open()) return;
    for (int i = 0; i < PROFILE_COUNT; i++)
        f << WideToUtf8(g_profileNames[i]) << "\n";
}

bool LoadWindowPlacement(int& x, int& y, int& w, int& h)
{
    std::lock_guard<std::mutex> g(g_cfgLock);
    std::ifstream f(WindowPath());
    if (!f.is_open()) return false;
    std::string line;
    int v[4] = {};
    for (int i = 0; i < 4; i++) {
        if (!std::getline(f, line) || sscanf_s(line.c_str(), "%d", &v[i]) != 1)
            return false;
    }
    x = v[0]; y = v[1]; w = v[2]; h = v[3];
    return true;
}

void SaveWindowPlacement(int x, int y, int w, int h)
{
    std::lock_guard<std::mutex> g(g_cfgLock);
    std::ofstream f(WindowPath(), std::ios::trunc);
    if (!f.is_open()) return;
    f << x << "\n" << y << "\n" << w << "\n" << h << "\n";
}

// 全部设置恢复编译期默认值 (空白方案槽首次切换时使用)
static void ResetDefaultsUnlocked()
{
    cpsLeft10 = 100;
    cpsRight10 = 100;
    cpsMax = 50;
    leftms = cpsToMs(cpsLeft10);
    rightms = cpsToMs(cpsRight10);
    randomCpsEnabled = false;
    randomCpsRange = 2;
    vk_key = 4;
    leftenabled = false;
    rightenabled = false;
    keepClicke = false;
    vk_multi_key = VK_XBUTTON2;
    multiMul = 1;
    multiDelayMs = 20;
    vk_scroll_key = 6;
    scrollClickButton = 0;
    vk_scroll_lr_key = VK_XBUTTON1;
    g_theme = Theme::Light;
    autoStopEnabled = false;
    autoStopSeconds = 30;
    topmost = false;
    canAttackOnlyClick = false;
    vk_canattack_key = VK_F6;
    placeOnlyRightClick = false;
    vk_place_key = VK_F7;
    humanizeMode = 0;
    humanizeLevel = 3;
    g_accentIdx = 0;
    vk_profile_key = 0;
    soundEnabled = true;
}

// ============================================================
//  公开接口
// ============================================================
void LoadConfig()
{
    std::lock_guard<std::mutex> g(g_cfgLock);

    // 一次性迁移: 旧版单文件 -> profile_1.txt (只做一次)
    if (!FileExists(ProfilePath(1))) {
        std::string old = GetConfigPath();
        if (FileExists(old))
            CopyFileA(old.c_str(), ProfilePath(1).c_str(), FALSE);
    }

    ReadActiveUnlocked();
    LoadUnlocked(ProfilePath(g_activeProfile));
}

void SaveConfig()
{
    std::lock_guard<std::mutex> g(g_cfgLock);
    SaveUnlocked(ProfilePath(g_activeProfile));
}

bool SwitchProfile(int n)
{
    if (n < 1 || n > PROFILE_COUNT) return false;
    std::lock_guard<std::mutex> g(g_cfgLock);
    if (n == g_activeProfile) return false;
    SaveUnlocked(ProfilePath(g_activeProfile));   // 旧槽落盘
    g_activeProfile = n;
    WriteActiveUnlocked();
    if (!FileExists(ProfilePath(n)))
        ResetDefaultsUnlocked();                  // 空白槽 -> 默认设置
    LoadUnlocked(ProfilePath(n));                 // 新槽读入全局 (文件缺失则保持默认)
    return true;
}
