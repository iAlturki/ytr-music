#include "app.h"
#include "main_window.h"
#include "miniplayer.h"
#include "webview_engine.h"
#include "tray.h"
#include "taskbar_controls.h"
#include <shellapi.h>

#define HOTKEY_ID_MINIPLAYER 1001
#define HOTKEY_ID_MEDIA_PLAYPAUSE 1002
#define HOTKEY_ID_MEDIA_NEXT 1003
#define HOTKEY_ID_MEDIA_PREV 1004
#define HOTKEY_ID_MEDIA_STOP 1005
#define HOTKEY_ID_UNIVERSAL_PLAYPAUSE 1006
#define HOTKEY_ID_UNIVERSAL_NEXT 1007
#define HOTKEY_ID_UNIVERSAL_PREV 1008

SongInfo g_currentSong;
HWND g_hMainWindow = nullptr;
HWND g_hMiniplayerWnd = nullptr;
static HWND g_hMsgWnd = nullptr;
static bool g_debugMode = false;

bool App_IsDebugMode() {
    return g_debugMode;
}

void App_SendControl(const std::wstring& action) {
    MainWindow::Instance().WakeWebView();
    WebViewEngine::Instance().SendControl(action);
}

void App_SeekTo(double seconds) {
    MainWindow::Instance().WakeWebView();
    WebViewEngine::Instance().SeekTo(seconds);
}

void App_SetVolume(int volumePercent) {
    MainWindow::Instance().WakeWebView();
    WebViewEngine::Instance().SetVolume(volumePercent);
}

void App_OnSongStateUpdated(const SongInfo& song) {
    // The miniplayer needs every message (pause flips, metadata, drift correction); tray and taskbar skip unchanged state themselves.
    MiniplayerWindow::Instance().UpdateSongState(song);
    TaskbarControls::Instance().UpdateState(song);
    std::wstring tip = song.title + L" - " + song.artist;
    if (tip.length() > 120) tip = tip.substr(0, 117) + L"...";
    SystemTray::Instance().UpdateTooltip(tip);
}

void App_OnPagePlaybackChanged(bool pagePaused) {
    MainWindow::Instance().OnPagePlaybackChanged(pagePaused);
}

void App_ToggleMiniplayer() {
    MiniplayerWindow::Instance().Toggle();
}

void App_ShowMainWindow() {
    MainWindow::Instance().Show();
}

void App_HideMainWindow() {
    MainWindow::Instance().Hide();
}

void App_Quit() {
    PostQuitMessage(0);
}

static LRESULT CALLBACK MsgWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    const UINT wmTaskbarCreated = SystemTray::Instance().GetTaskbarCreatedMsg();
    if (wmTaskbarCreated && msg == wmTaskbarCreated) {
        SystemTray::Instance().OnTaskbarCreated();
        return 0;
    }

    switch (msg) {
        case WM_HOTKEY: {
            if (wParam == HOTKEY_ID_MINIPLAYER) {
                App_ToggleMiniplayer();
            } else if (wParam == HOTKEY_ID_MEDIA_PLAYPAUSE || wParam == HOTKEY_ID_UNIVERSAL_PLAYPAUSE) {
                App_SendControl(L"playPause");
            } else if (wParam == HOTKEY_ID_MEDIA_NEXT || wParam == HOTKEY_ID_UNIVERSAL_NEXT) {
                App_SendControl(L"next");
            } else if (wParam == HOTKEY_ID_MEDIA_PREV || wParam == HOTKEY_ID_UNIVERSAL_PREV) {
                App_SendControl(L"previous");
            } else if (wParam == HOTKEY_ID_MEDIA_STOP) {
                // Stop must never start playback.
                if (!g_currentSong.isPaused) App_SendControl(L"playPause");
            }
            return 0;
        }
        case WM_TRAYICON: {
            SystemTray::Instance().HandleTrayMessage(wParam, lParam);
            return 0;
        }
        case WM_CLOSE: {
            // This invisible window stays foreground after the tray menu closes; Alt+F4 must
            // not destroy the owner of the hotkeys and the tray icon.
            return 0;
        }
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// Returns false only when the running instance exited meanwhile and this process now owns
// the mutex, so it should start normally.
static bool HandOffToRunningInstance(HANDLE hMutex, bool wantMiniplayer) {
    const DWORD_PTR kShowInstanceAck = 0x59545231;
    UINT msgShow = RegisterWindowMessageW(L"YTR_MUSIC_SHOW_INSTANCE");
    WPARAM wp = wantMiniplayer ? 1 : 0;
    // The first instance may still be starting (mutex taken, window not created yet).
    for (int i = 0; i < 20; ++i) {
        HWND hWnd = FindWindowW(L"YTRMusicMainWindowClass", NULL);
        if (hWnd) {
            DWORD pid = 0;
            GetWindowThreadProcessId(hWnd, &pid);
            // Pass on this launch's foreground right before the hand-off, so Show() can take focus.
            if (pid) AllowSetForegroundWindow(pid);
            // Require an answer: a window whose instance is already shutting down never reads it.
            DWORD_PTR reply = 0;
            if (SendMessageTimeoutW(hWnd, msgShow, wp, 0, SMTO_ABORTIFHUNG, 2000, &reply) && reply == kShowInstanceAck) {
                return true;
            }
            if (!hMutex) return true;
            DWORD wait = WaitForSingleObject(hMutex, 5000);
            return !(wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED);
        }
        if (!hMutex) {
            Sleep(100);
            continue;
        }
        DWORD wait = WaitForSingleObject(hMutex, 100);
        if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) return false;
    }
    PostMessageW(HWND_BROADCAST, msgShow, wp, 0);
    return true;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR lpCmdLine, int nCmdShow) {
    g_debugMode = wcsstr(lpCmdLine, L"--debug") != nullptr;
    const bool startMiniplayer = wcsstr(lpCmdLine, L"--miniplayer") != nullptr;

    LogBridge(L"wWinMain started");
    // Set AppUserModelID for seamless Windows Taskbar pinning and grouping
    SetCurrentProcessExplicitAppUserModelID(L"com.github.iAlturki.ytr-music");

    // 1. Single-Instance Mutex & Process Management
    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"YTRMusicNativeSingleInstanceMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        LogBridge(L"SingleInstanceMutex already exists, handing off.");
        if (HandOffToRunningInstance(hMutex, startMiniplayer)) {
            if (hMutex) CloseHandle(hMutex);
            return 0;
        }
    }
    LogBridge(L"Mutex acquired");

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    // 2. Helper window for hotkeys and the tray icon. Hidden top-level, not message-only:
    // it must receive the TaskbarCreated broadcast and own the tray menu as foreground window.
    WNDCLASSEXW mc = {};
    mc.cbSize = sizeof(WNDCLASSEXW);
    mc.lpfnWndProc = MsgWndProc;
    mc.hInstance = hInstance;
    mc.lpszClassName = L"YTRMusicMsgHelperClass";
    RegisterClassExW(&mc);

    g_hMsgWnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"YTRMusicMsgHelperClass", L"", WS_POPUP, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);
    ChangeWindowMessageFilterEx(g_hMsgWnd, SystemTray::Instance().GetTaskbarCreatedMsg(), MSGFLT_ALLOW, NULL);

    // 3. Register Global Hotkeys
    RegisterHotKey(g_hMsgWnd, HOTKEY_ID_MINIPLAYER, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'M');
    // Global Hardware Media Keys
    RegisterHotKey(g_hMsgWnd, HOTKEY_ID_MEDIA_PLAYPAUSE, MOD_NOREPEAT, VK_MEDIA_PLAY_PAUSE);
    RegisterHotKey(g_hMsgWnd, HOTKEY_ID_MEDIA_NEXT, MOD_NOREPEAT, VK_MEDIA_NEXT_TRACK);
    RegisterHotKey(g_hMsgWnd, HOTKEY_ID_MEDIA_PREV, MOD_NOREPEAT, VK_MEDIA_PREV_TRACK);
    RegisterHotKey(g_hMsgWnd, HOTKEY_ID_MEDIA_STOP, MOD_NOREPEAT, VK_MEDIA_STOP);
    // Universal Shortcuts (Ctrl+Alt+Space, Ctrl+Alt+Right, Ctrl+Alt+Left)
    RegisterHotKey(g_hMsgWnd, HOTKEY_ID_UNIVERSAL_PLAYPAUSE, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_SPACE);
    RegisterHotKey(g_hMsgWnd, HOTKEY_ID_UNIVERSAL_NEXT, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_RIGHT);
    RegisterHotKey(g_hMsgWnd, HOTKEY_ID_UNIVERSAL_PREV, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_LEFT);

    // 4. Main Window, shown before the WebView, miniplayer and tray setup (unless --miniplayer)
    MainWindow::Instance().Create(!startMiniplayer, nCmdShow);

    // 5. Desktop Miniplayer
    MiniplayerWindow::Instance().Create();

    // 6. Initialize System Tray
    SystemTray::Instance().Initialize(g_hMsgWnd);

    if (startMiniplayer) {
        // Main window stays hidden; the engine keeps the page visible until it first plays.
        MainWindow::Instance().Hide();
        MiniplayerWindow::Instance().Show();
    }

    // 7. Standard Win32 Message Loop
    MSG msg = {};
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // Tear down COM objects and windows while COM is initialized and the parent HWND exists;
    // the static destructors that run after wWinMain returns are then no-ops.
    UnregisterHotKey(g_hMsgWnd, HOTKEY_ID_MINIPLAYER);
    UnregisterHotKey(g_hMsgWnd, HOTKEY_ID_MEDIA_PLAYPAUSE);
    UnregisterHotKey(g_hMsgWnd, HOTKEY_ID_MEDIA_NEXT);
    UnregisterHotKey(g_hMsgWnd, HOTKEY_ID_MEDIA_PREV);
    UnregisterHotKey(g_hMsgWnd, HOTKEY_ID_MEDIA_STOP);
    UnregisterHotKey(g_hMsgWnd, HOTKEY_ID_UNIVERSAL_PLAYPAUSE);
    UnregisterHotKey(g_hMsgWnd, HOTKEY_ID_UNIVERSAL_NEXT);
    UnregisterHotKey(g_hMsgWnd, HOTKEY_ID_UNIVERSAL_PREV);
    SystemTray::Instance().Remove();
    WebViewEngine::Instance().Shutdown();
    TaskbarControls::Instance().Shutdown();
    MiniplayerWindow::Instance().Shutdown();
    MainWindow::Instance().Destroy();
    if (g_hMsgWnd) {
        DestroyWindow(g_hMsgWnd);
        g_hMsgWnd = nullptr;
    }

    CoUninitialize();

    if (hMutex) {
        CloseHandle(hMutex);
    }

    return (int)msg.wParam;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR, int nCmdShow) {
    return wWinMain(hInstance, hPrevInstance, GetCommandLineW(), nCmdShow);
}
