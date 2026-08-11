#include "sound.h"
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

void PlayClickerSound(bool enabled)
{
    PlayMediaSound(enabled ? L"Windows Hardware Insert.wav" : L"Windows Hardware Remove.wav");
}

void PlayMultiClickSound(bool enabled)
{
    PlayMediaSound(enabled ? L"Windows Hardware Insert.wav" : L"Windows Hardware Remove.wav");
}

void PlayScrollClickSound(bool enabled)
{
    PlayMediaSound(enabled ? L"Windows Notify.wav" : L"Windows Notify Calendar.wav");
}

void PlayScrollLRSound()
{
    PlayMediaSound(L"Windows Navigation Start.wav");
}

void PlayCanAttackSound(bool enabled)
{
    PlayMediaSound(enabled ? L"Windows Hardware Insert.wav" : L"Windows Hardware Remove.wav");
}

void PlayCanPlaceSound(bool enabled)
{
    PlayMediaSound(enabled ? L"Windows Hardware Insert.wav" : L"Windows Hardware Remove.wav");
}
