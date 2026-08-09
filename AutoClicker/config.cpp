#include "config.h"
#include "clicker.h"
#include "canattack.h"

#include <Windows.h>
#include <ShlObj.h>
#include <fstream>
#include <string>
#include <cstdio>

static std::string GetConfigPath()
{
    char appdata[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) {
        std::string dir = std::string(appdata) + "\\AutoClicker";
        CreateDirectoryA(dir.c_str(), nullptr);
        return dir + "\\autoclickerSave.txt";
    }
    return "autoclickerSave.txt";
}

void LoadConfig()
{
    std::ifstream file(GetConfigPath());
    if (!file.is_open()) return;

    std::string line;
    int v;

    if (std::getline(file, line) && sscanf_s(line.c_str(), "%d", &v) == 1 && v >= 5 && v <= 1000)
        cpsLeft10 = v;
    if (std::getline(file, line) && sscanf_s(line.c_str(), "%d", &v) == 1 && v >= 5 && v <= 1000)
        cpsRight10 = v;
    if (std::getline(file, line) && sscanf_s(line.c_str(), "%d", &v) == 1 && v >= 20 && v <= 500)
        cpsMax = v;
    if (std::getline(file, line) && sscanf_s(line.c_str(), "%d", &v) == 1)
        randomCpsEnabled = (v != 0);
    if (std::getline(file, line) && sscanf_s(line.c_str(), "%d", &v) == 1 && v >= 1 && v <= 5)
        randomCpsRange = v;
    if (std::getline(file, line) && sscanf_s(line.c_str(), "%d", &v) == 1 && v >= 0 && v <= 255)
        vk_key = v;
    if (std::getline(file, line) && sscanf_s(line.c_str(), "%d", &v) == 1)
        leftenabled = (v != 0);
    if (std::getline(file, line) && sscanf_s(line.c_str(), "%d", &v) == 1)
        rightenabled = (v != 0);
    if (std::getline(file, line) && sscanf_s(line.c_str(), "%d", &v) == 1)
        keepClicke = (v != 0);
    if (std::getline(file, line) && sscanf_s(line.c_str(), "%d", &v) == 1 && v >= 0 && v <= 255)
        vk_multi_key = v;
    if (std::getline(file, line) && sscanf_s(line.c_str(), "%d", &v) == 1 && v >= 1 && v <= 5)
        multiMul = v;
    if (std::getline(file, line) && sscanf_s(line.c_str(), "%d", &v) == 1 && v >= 1 && v <= 200)
        multiDelayMs = v;
    if (std::getline(file, line) && sscanf_s(line.c_str(), "%d", &v) == 1 && v >= 0 && v <= 255)
        vk_scroll_key = v;
    if (std::getline(file, line) && sscanf_s(line.c_str(), "%d", &v) == 1)
        scrollClickButton = (v != 0) ? 1 : 0;
    if (std::getline(file, line) && sscanf_s(line.c_str(), "%d", &v) == 1 && v >= 0 && v <= 255)
        vk_scroll_lr_key = v;
    if (std::getline(file, line) && sscanf_s(line.c_str(), "%d", &v) == 1)
        g_theme = (v != 0) ? Theme::Light : Theme::Dark;
    if (std::getline(file, line) && sscanf_s(line.c_str(), "%d", &v) == 1)
        autoStopEnabled = (v != 0);
    if (std::getline(file, line) && sscanf_s(line.c_str(), "%d", &v) == 1 && v >= 1 && v <= 3600)
        autoStopSeconds = v;
    if (std::getline(file, line) && sscanf_s(line.c_str(), "%d", &v) == 1)
        topmost = (v != 0);
    if (std::getline(file, line) && sscanf_s(line.c_str(), "%d", &v) == 1)
        canAttackOnlyClick = (v != 0);
    if (std::getline(file, line) && sscanf_s(line.c_str(), "%d", &v) == 1 && v >= 0 && v <= 255)
        vk_canattack_key = v;

    leftms = cpsToMs(cpsLeft10);
    rightms = cpsToMs(cpsRight10);
}

void SaveConfig()
{
    std::ofstream file(GetConfigPath(), std::ios::trunc);
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
         << vk_canattack_key << "\n";
}
