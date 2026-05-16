#pragma once

#include <PCH.h>

bool UIInitialize(HWND hwnd);
void UIRenderFrame();
bool UIIsMenuOpen();
bool UIIsWindowActive();
const RECT* UIGetWindowRect();

LRESULT CALLBACK ImGuiWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

void UIDrawShaderSettingsOverlay();
void UIDrawShaderDebugOverlay();
void UIDrawCustomBufferMonitorOverlay();
void UILockShaderList_Internal();
void UIUnlockShaderList_Internal();
