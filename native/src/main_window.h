#pragma once

#include "app.h"

class MainWindow {
public:
    static MainWindow& Instance();

    // showNow shows the window before the WebView starts loading; nCmdShow is honoured
    // for minimized/maximized launches.
    bool Create(bool showNow = false, int nCmdShow = SW_SHOWNORMAL);
    void Show();
    void Hide();
    void Destroy();
    bool IsVisible() const;

    // Page-reported play/pause; drives the WebView low-memory policy.
    void OnPagePlaybackChanged(bool pagePaused);
    // Back to NORMAL memory before a user command reaches the page.
    void WakeWebView();

    HWND GetHwnd() const { return m_hWnd; }

private:
    MainWindow();
    ~MainWindow();

    static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void DrawTitlebar(HDC hdc, int width, int height);
    void ShowInitial(int nCmdShow);
    bool IsPageIdle() const;
    void UpdateMemoryPolicy();
    void OnMemoryLowTimer();

    HWND m_hWnd = nullptr;
    bool m_isVisible = false;
    bool m_pagePaused = true;
    bool m_memLowTimerArmed = false;
    bool m_memLow = false;
};
