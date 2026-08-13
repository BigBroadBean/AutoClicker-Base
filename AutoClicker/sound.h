#pragma once

void PlayClickerSound(bool enabled);
void PlayMultiClickSound(bool enabled);
void PlayScrollClickSound(bool enabled);
void PlayScrollLRSound();
void PlayCanAttackSound(bool enabled);
void PlayCanPlaceSound(bool enabled);
// 提示音总开关本身的确认音 (不受总开关限制, 保证关闭时也有最后一次反馈)
void PlayToggleSound(bool enabled);
