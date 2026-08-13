#include "sound.h"
#include "clicker.h"
#include <Windows.h>
#include <mmsystem.h>
#include <thread>

#pragma comment(lib, "winmm.lib")

static void PlayMediaSound(const wchar_t* file)
{
    std::thread([file]() {
        wchar_t path[MAX_PATH];
        swprintf(path, MAX_PATH, L"C:\\Windows\\Media\\%s", file);
        PlaySoundW(path, NULL, SND_FILENAME | SND_ASYNC);
        Sleep(200);
    }).detach();
}

// 所有提示音统一走这里: 总开关关闭时静音 (开关本身的确认音除外)
static void PlayGated(const wchar_t* file)
{
    if (!soundEnabled) return;
    PlayMediaSound(file);
}

void PlayClickerSound(bool enabled)
{
    PlayGated(enabled ? L"Windows Hardware Insert.wav" : L"Windows Hardware Remove.wav");
}

void PlayMultiClickSound(bool enabled)
{
    PlayGated(enabled ? L"Windows Hardware Insert.wav" : L"Windows Hardware Remove.wav");
}

void PlayScrollClickSound(bool enabled)
{
    PlayGated(enabled ? L"Windows Notify.wav" : L"Windows Notify Calendar.wav");
}

void PlayScrollLRSound()
{
    PlayGated(L"Windows Navigation Start.wav");
}

void PlayCanAttackSound(bool enabled)
{
    PlayGated(enabled ? L"Windows Hardware Insert.wav" : L"Windows Hardware Remove.wav");
}

void PlayCanPlaceSound(bool enabled)
{
    PlayGated(enabled ? L"Windows Hardware Insert.wav" : L"Windows Hardware Remove.wav");
}

void PlayToggleSound(bool enabled)
{
    // 开关确认音不走门控: 关闭提示音时也播放一次, 作为最后反馈
    PlayMediaSound(enabled ? L"Windows Hardware Insert.wav" : L"Windows Hardware Remove.wav");
}
