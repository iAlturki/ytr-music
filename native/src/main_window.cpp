#include "main_window.h"
#include "webview_engine.h"
#include "taskbar_controls.h"
#include "miniplayer.h"
#include "../res/resource.h"
#include <windowsx.h>
#include <dwmapi.h>

// One-shot debounce before a hidden, paused page drops to the low memory level.
#define TIMER_ID_MEMORY_LOW 1
#define MEMORY_LOW_DELAY_MS 120000

static UINT GetWindowDpi(HWND hWnd) {
    // Looked up at runtime: not declared at this toolchain's default _WIN32_WINNT, and
    // missing before Windows 10 1607.
    typedef UINT (WINAPI *GetDpiForWindowFn)(HWND);
    static const GetDpiForWindowFn pGetDpiForWindow = (GetDpiForWindowFn)(void (*)(void))GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    UINT dpi = pGetDpiForWindow ? pGetDpiForWindow(hWnd) : 0;
    if (!dpi) {
        HDC hdc = GetDC(NULL);
        if (hdc) {
            dpi = (UINT)GetDeviceCaps(hdc, LOGPIXELSY);
            ReleaseDC(NULL, hdc);
        }
    }
    return dpi ? dpi : 96;
}

MainWindow& MainWindow::Instance() {
    static MainWindow instance;
    return instance;
}

MainWindow::MainWindow() {}

MainWindow::~MainWindow() {
    Destroy();
}

bool MainWindow::Create(bool showNow, int nCmdShow) {
    HINSTANCE hInst = GetModuleHandleW(NULL);
    HICON hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    if (!hIcon) hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APP_ICON));
    HICON hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (!hIconSm) hIconSm = hIcon;

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"YTRMusicMainWindowClass";
    wc.hIcon = hIcon;
    wc.hIconSm = hIconSm;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassExW(&wc);

    // Centered in the primary monitor's work area (origin included), scaled to its DPI.
    RECT workArea = { 0, 0, 1280, 760 };
    MONITORINFO mi = {};
    mi.cbSize = sizeof(MONITORINFO);
    if (GetMonitorInfoW(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi)) {
        workArea = mi.rcWork;
    } else {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    }

    // WS_CLIPCHILDREN: the parent never paints over the WebView (no resize flicker or overdraw).
    m_hWnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        L"YTRMusicMainWindowClass",
        L"ytr-music",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        workArea.left, workArea.top, 1280, 760,
        NULL, NULL, hInst, this
    );

    if (!m_hWnd) return false;

    g_hMainWindow = m_hWnd;

    // The DPI is only known once the window exists on its monitor.
    const UINT dpi = GetWindowDpi(m_hWnd);
    const int areaWidth = workArea.right - workArea.left;
    const int areaHeight = workArea.bottom - workArea.top;
    int width = MulDiv(1280, dpi, 96);
    int height = MulDiv(760, dpi, 96);
    if (width > areaWidth) width = areaWidth;
    if (height > areaHeight) height = areaHeight;
    SetWindowPos(m_hWnd, NULL,
        workArea.left + (areaWidth - width) / 2, workArea.top + (areaHeight - height) / 2,
        width, height, SWP_NOZORDER | SWP_NOACTIVATE);

    // Enable Windows Immersive Dark Title Bar Mode
    BOOL useDarkMode = TRUE;
    DwmSetWindowAttribute(m_hWnd, 20, &useDarkMode, sizeof(useDarkMode));
    DwmSetWindowAttribute(m_hWnd, 19, &useDarkMode, sizeof(useDarkMode));

    if (hIcon) SendMessageW(m_hWnd, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
    if (hIconSm) SendMessageW(m_hWnd, WM_SETICON, ICON_SMALL, (LPARAM)hIconSm);


    static UINT s_wmShowInstance = RegisterWindowMessageW(L"YTR_MUSIC_SHOW_INSTANCE");
    ChangeWindowMessageFilterEx(m_hWnd, s_wmShowInstance, MSGFLT_ALLOW, NULL);

    // Initialize Taskbar Controls (before any show, so TaskbarButtonCreated is let through)
    TaskbarControls::Instance().Initialize(m_hWnd);

    // Show before the WebView starts so the dark window appears without waiting on it.
    if (showNow) ShowInitial(nCmdShow);

    // Initialize WebView2 inside this window
    WebViewEngine::Instance().Initialize(m_hWnd, [this]() {
        // A minimized window has a 0x0 client area; WM_SIZE sizes the WebView on restore.
        if (!m_hWnd || IsIconic(m_hWnd)) return;
        RECT rc;
        GetClientRect(m_hWnd, &rc);
        WebViewEngine::Instance().Resize(rc.right, rc.bottom);
    });

    return true;
}

void MainWindow::ShowInitial(int nCmdShow) {
    if (nCmdShow == SW_SHOWMINIMIZED || nCmdShow == SW_SHOWMINNOACTIVE || nCmdShow == SW_MINIMIZE) {
        // "Run: Minimized" launch: start on the taskbar without taking focus.
        m_isVisible = true;
        ShowWindow(m_hWnd, SW_SHOWMINNOACTIVE);
        UpdateMemoryPolicy();
        return;
    }
    if (nCmdShow == SW_SHOWMAXIMIZED) ShowWindow(m_hWnd, SW_SHOWMAXIMIZED);
    Show();
}

void MainWindow::Show() {
    if (!m_hWnd && !Create()) return;
    m_isVisible = true;
    // SW_RESTORE only when minimized, so a window hidden while maximized comes back maximized.
    ShowWindow(m_hWnd, IsIconic(m_hWnd) ? SW_RESTORE : SW_SHOW);
    SetWindowPos(m_hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetWindowPos(m_hWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    // Back to NORMAL memory before the page is made visible again.
    UpdateMemoryPolicy();
    SetForegroundWindow(m_hWnd);
    SetFocus(m_hWnd);

    // Re-showing a hidden (not minimized) window sends no WM_SIZE.
    RECT rc;
    GetClientRect(m_hWnd, &rc);
    WebViewEngine::Instance().Resize(rc.right, rc.bottom);
    WebViewEngine::Instance().SetVisible(true);
}

void MainWindow::Hide() {
    m_isVisible = false;
    if (m_hWnd) {
        WebViewEngine::Instance().SetVisible(false);
        if (IsWindowVisible(m_hWnd)) ShowWindow(m_hWnd, SW_HIDE);
    }
    UpdateMemoryPolicy();
}

void MainWindow::Destroy() {
    if (!m_hWnd) return;
    HWND hWnd = m_hWnd;
    // Cleared first so WM_DESTROY knows this is the shutdown path and posts no WM_QUIT.
    m_hWnd = nullptr;
    g_hMainWindow = nullptr;
    m_isVisible = false;
    m_memLowTimerArmed = false;
    DestroyWindow(hWnd);
}

bool MainWindow::IsVisible() const {
    return m_isVisible;
}

void MainWindow::OnPagePlaybackChanged(bool pagePaused) {
    m_pagePaused = pagePaused;
    UpdateMemoryPolicy();
}

void MainWindow::WakeWebView() {
    if (m_memLowTimerArmed) {
        KillTimer(m_hWnd, TIMER_ID_MEMORY_LOW);
        m_memLowTimerArmed = false;
    }
    if (m_memLow) {
        m_memLow = false;
        WebViewEngine::Instance().SetMemoryLow(false);
    }
    // Restarts the debounce if the page is still hidden and paused.
    UpdateMemoryPolicy();
}

bool MainWindow::IsPageIdle() const {
    // Nobody can see the page and it is not playing. LOW is never applied while audio runs.
    return m_pagePaused && (!m_hWnd || !IsWindowVisible(m_hWnd) || IsIconic(m_hWnd));
}

void MainWindow::UpdateMemoryPolicy() {
    if (!IsPageIdle()) {
        if (m_memLowTimerArmed) {
            KillTimer(m_hWnd, TIMER_ID_MEMORY_LOW);
            m_memLowTimerArmed = false;
        }
        if (m_memLow) {
            m_memLow = false;
            WebViewEngine::Instance().SetMemoryLow(false);
        }
    } else if (m_hWnd && !m_memLow && !m_memLowTimerArmed) {
        m_memLowTimerArmed = SetTimer(m_hWnd, TIMER_ID_MEMORY_LOW, MEMORY_LOW_DELAY_MS, NULL) != 0;
    }
}

void MainWindow::OnMemoryLowTimer() {
    KillTimer(m_hWnd, TIMER_ID_MEMORY_LOW);
    m_memLowTimerArmed = false;
    if (!m_memLow && IsPageIdle()) {
        m_memLow = true;
        WebViewEngine::Instance().SetMemoryLow(true);
    }
}

LRESULT CALLBACK MainWindow::WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    MainWindow* self = (MainWindow*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lParam;
        self = (MainWindow*)cs->lpCreateParams;
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)self);
    }

    static UINT s_wmShowInstance = RegisterWindowMessageW(L"YTR_MUSIC_SHOW_INSTANCE");
    if (s_wmShowInstance && msg == s_wmShowInstance) {
        if (wParam == 1) {
            // Second launch with --miniplayer
            if (!MiniplayerWindow::Instance().IsVisible()) MiniplayerWindow::Instance().Show();
        } else if (self) {
            self->Show();
        } else {
            ShowWindow(hWnd, SW_RESTORE);
            ShowWindow(hWnd, SW_SHOW);
            SetWindowPos(hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
            SetWindowPos(hWnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
            SetForegroundWindow(hWnd);
            SetFocus(hWnd);
        }
        // Acknowledges the hand-off to a second launch (see HandOffToRunningInstance).
        return 0x59545231;
    }

    const UINT wmButtonCreated = TaskbarControls::Instance().GetTaskbarButtonCreatedMsg();
    if (wmButtonCreated && msg == wmButtonCreated) {
        TaskbarControls::Instance().OnTaskbarButtonCreated();
        return 0;
    }

    switch (msg) {
        case WM_SETFOCUS: {
            // Keep keyboard focus inside the page so its shortcuts (Space, Ctrl+K, Esc) work after activation.
            if (ICoreWebView2Controller* c = WebViewEngine::Instance().GetController()) {
                c->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
                return 0;
            }
            break;
        }
        case WM_COMMAND: {
            if (TaskbarControls::Instance().HandleCommand(LOWORD(wParam))) {
                return 0;
            }
            break;
        }
        case WM_SIZE: {
            // Memory level first, so a restored page is back at NORMAL before it is made visible.
            if (self) self->UpdateMemoryPolicy();
            if (wParam == SIZE_MINIMIZED) {
                // Hide the page instead of laying it out at 0x0.
                WebViewEngine::Instance().SetVisible(false);
            } else if (wParam == SIZE_RESTORED || wParam == SIZE_MAXIMIZED) {
                WebViewEngine::Instance().Resize(LOWORD(lParam), HIWORD(lParam));
                WebViewEngine::Instance().SetVisible(IsWindowVisible(hWnd) != FALSE);
            }
            return 0;
        }
        case WM_DPICHANGED: {
            const RECT* r = (const RECT*)lParam;
            SetWindowPos(hWnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_TIMER: {
            if (wParam == TIMER_ID_MEMORY_LOW) {
                if (self) self->OnMemoryLowTimer();
                return 0;
            }
            break;
        }
        case WM_CLOSE: {
            // Minimize to system tray on close instead of exiting
            if (self) self->Hide();
            return 0;
        }
        case WM_DESTROY: {
            // Destroy() clears g_hMainWindow first; anything else destroying the window still quits.
            if (hWnd == g_hMainWindow) PostQuitMessage(0);
            return 0;
        }
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}
