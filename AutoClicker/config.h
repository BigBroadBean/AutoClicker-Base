#pragma once

#include <string>

// ============================================================
//  配置系统
// ============================================================
// 主配置按「方案」(profile) 分槽存储：%APPDATA%\AutoClicker\
//   profile_1.txt ... profile_4.txt  每个方案一份完整设置
//   active.txt                       当前激活的方案 (1..4)
//   ui.txt                           4 个方案的显示名称 (UTF-8, 每行一个)
//   window.txt                       窗口位置/尺寸 (4 行整数)
// 旧版单文件 autoclickerSave.txt 首次启动自动迁移为 profile_1.txt。

constexpr int PROFILE_COUNT = 4;

// 激活的方案槽 (1..4)，UI / 热键切换后由 LoadConfig 写入全局设置
extern int g_activeProfile;
// 方案显示名称 (默认 方案1..方案4)
extern std::wstring g_profileNames[PROFILE_COUNT];

void LoadConfig();
void SaveConfig();

// 切换到方案 n (1..4)：先把当前全局设置写入旧槽，再读入新槽。
// 返回是否真的发生了切换 (n 非法或等于当前槽时返回 false)。
// 注意：只改全局设置变量；主题(DWM)/置顶/Toast 等副作用由调用方处理。
bool SwitchProfile(int n);

// ---- 全局 UI 状态 (不属于任何方案) ----
void LoadUiState();                                    // 方案名
void SaveUiState();                                    // 方案名
bool LoadWindowPlacement(int& x, int& y, int& w, int& h);
void SaveWindowPlacement(int x, int y, int w, int h);
