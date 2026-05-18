#pragma once

#include <windows.h> 
#include "resource.h"
constexpr auto ANIMATION_DURATION_MS = 300;
constexpr auto ANIMATION_TIMER_INTERVAL = 16; 

// Animation state variables;
extern double g_animationProgress;
extern ULONGLONG g_lastAnimationTime;

VOID CALLBACK AnimationTimerProc(HWND hwnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime);