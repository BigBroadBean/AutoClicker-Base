#pragma once

#include "types.h"
#include <Windows.h>

void ShowToast(const wchar_t* title, const wchar_t* status, COLORREF statusColor);

inline void ShowToggleToast(const wchar_t* title, bool enabled) {
    ShowToast(title, enabled ? L"\x5f00" : L"\x5173",
              enabled ? GREEN() : RED());
}

inline void ShowScrollLRToast(int button) {
    ShowToast(L"\x6eda\x8f6e\x70b9\x51fb",
              button == 0 ? L"\x5de6\x952e" : L"\x53f3\x952e",
              ACCENT());
}

inline void ShowCanAttackToast(bool enabled) {
    ShowToast(L"\x4ec5\x80fd\x653b\x51fb\x65f6\x8fde\x70b9",
              enabled ? L"\x5f00" : L"\x5173",
              enabled ? GREEN() : RED());
}

inline void ShowCanPlaceToast(bool enabled) {
    ShowToast(L"\x4ec5\x6301\x6251\x7f6e\x7269\x65f6\x53f3\x952e\x8fde\x70b9",
              enabled ? L"\x5f00" : L"\x5173",
              enabled ? GREEN() : RED());
}
