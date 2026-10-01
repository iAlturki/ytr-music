#include "app.h"
#include "main_window.h"
#include "miniplayer.h"
#include "webview_engine.h"
#include "tray.h"
#include "taskbar_controls.h"
#include <psapi.h>
#include <tlhelp32.h>
#include <shellapi.h>

#define WM_TRAYICON (WM_USER + 101)
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

void App_SendControl(const std::wstring& action) {
    WebViewEngine::Instance().SendControl(action);
}

void App_SeekTo(double seconds) {
    WebViewEngine::Instance().SeekTo(seconds);
}

void App_SetVolume(int volumePercent) {
    WebViewEngine::Instance().SetVolume(volumePercent);
}

void App_TrimWorkingSet() {
    // 1. Trim current host process
    EmptyWorkingSet(GetCurrentProcess());

    // 2. Find and trim child processes spawned by WebView2
    DWORD currentPid = GetCurrentProcessId();
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe = { sizeof(PROCESSENTRY32W) };
        if (Process32FirstW(hSnapshot, &pe)) {
            do {
                if (pe.th32ParentProcessID == currentPid) {
                    HANDLE hProc = OpenProcess(PROCESS_SET_QUOTA | PROCESS_QUERY_INFORMATION, FALSE, pe.th32ProcessID);
                    if (hProc) {
                        EmptyWorkingSet(hProc);
                        CloseHandle(hProc);
                    }
                }
            } while (Process32NextW(hSnapshot, &pe));
        }
        CloseHandle(hSnapshot);
    }
}

void App_OnSongStateUpdated(const SongInfo& song) {
    MiniplayerWindow::Instance().UpdateSongState(song);
    TaskbarControls::Instance().UpdateState(song);
    std::wstring tip = song.title + L" - " + song.artist;
    if (tip.length() > 120) tip = tip.substr(0, 117) + L"...";
    SystemTray::Instance().UpdateTooltip(tip);
}


void App_ToggleMiniplayer() {
    MiniplayerWindow::Instance().Toggle();
    App_TrimWorkingSet();
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
                App_SendControl(L"playPause");
            }
            return 0;
        }
        case WM_TRAYICON: {
            SystemTray::Instance().HandleTrayMessage(wParam, lParam);
            return 0;
        }
        case WM_TIMER: {
            if (wParam == 999) {
                App_TrimWorkingSet();
            }
            return 0;
        }
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
    LogBridge(L"wWinMain started");
    // Set AppUserModelID for seamless Windows Taskbar pinning and grouping
    SetCurrentProcessExplicitAppUserModelID(L"com.github.iAlturki.ytr-music");

    // 1. Single-Instance Mutex & Process Management
    HANDLE hMutex = CreateMutexW(NULL, TRUE, L"YTRMusicNativeSingleInstanceMutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        LogBridge(L"SingleInstanceMutex already exists! Exiting.");
        UINT msgShow = RegisterWindowMessageW(L"YTR_MUSIC_SHOW_INSTANCE");
        PostMessageW(HWND_BROADCAST, msgShow, 0, 0);
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }
    LogBridge(L"Mutex acquired");

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    // 2. Create message-only helper window
    WNDCLASSEXW mc = { sizeof(WNDCLASSEXW) };
    mc.lpfnWndProc = MsgWndProc;
    mc.hInstance = hInstance;
    mc.lpszClassName = L"YTRMusicMsgHelperClass";
    RegisterClassExW(&mc);

    g_hMsgWnd = CreateWindowExW(0, L"YTRMusicMsgHelperClass", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hInstance, NULL);

    // 3. Register Global Hotkeys
    RegisterHotKey(g_hMsgWnd, HOTKEY_ID_MINIPLAYER, MOD_CONTROL | MOD_ALT, 'M');
    // Global Hardware Media Keys
    RegisterHotKey(g_hMsgWnd, HOTKEY_ID_MEDIA_PLAYPAUSE, 0, VK_MEDIA_PLAY_PAUSE);
    RegisterHotKey(g_hMsgWnd, HOTKEY_ID_MEDIA_NEXT, 0, VK_MEDIA_NEXT_TRACK);
    RegisterHotKey(g_hMsgWnd, HOTKEY_ID_MEDIA_PREV, 0, VK_MEDIA_PREV_TRACK);
    RegisterHotKey(g_hMsgWnd, HOTKEY_ID_MEDIA_STOP, 0, VK_MEDIA_STOP);
    // Universal Shortcuts (Ctrl+Alt+Space, Ctrl+Alt+Right, Ctrl+Alt+Left)
    RegisterHotKey(g_hMsgWnd, HOTKEY_ID_UNIVERSAL_PLAYPAUSE, MOD_CONTROL | MOD_ALT, VK_SPACE);
    RegisterHotKey(g_hMsgWnd, HOTKEY_ID_UNIVERSAL_NEXT, MOD_CONTROL | MOD_ALT, VK_RIGHT);
    RegisterHotKey(g_hMsgWnd, HOTKEY_ID_UNIVERSAL_PREV, MOD_CONTROL | MOD_ALT, VK_LEFT);

    // 4. Initialize System Tray
    SystemTray::Instance().Initialize(g_hMsgWnd);

    // 5. Create Main Window & Desktop Miniplayer
    MainWindow::Instance().Create();
    MiniplayerWindow::Instance().Create();

    // Check command line: if user launched with --miniplayer, only show miniplayer
    if (wcsstr(lpCmdLine, L"--miniplayer") != nullptr) {
        MiniplayerWindow::Instance().Show();
    } else {
        MainWindow::Instance().Show();
    }

    // 6. Periodic active memory compaction (every 60 seconds)
    SetTimer(g_hMsgWnd, 999, 60000, NULL);

    // 7. Standard Win32 Message Loop
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnregisterHotKey(g_hMsgWnd, HOTKEY_ID_MINIPLAYER);
    UnregisterHotKey(g_hMsgWnd, HOTKEY_ID_MEDIA_PLAYPAUSE);
    UnregisterHotKey(g_hMsgWnd, HOTKEY_ID_MEDIA_NEXT);
    UnregisterHotKey(g_hMsgWnd, HOTKEY_ID_MEDIA_PREV);
    UnregisterHotKey(g_hMsgWnd, HOTKEY_ID_MEDIA_STOP);
    UnregisterHotKey(g_hMsgWnd, HOTKEY_ID_UNIVERSAL_PLAYPAUSE);
    UnregisterHotKey(g_hMsgWnd, HOTKEY_ID_UNIVERSAL_NEXT);
    UnregisterHotKey(g_hMsgWnd, HOTKEY_ID_UNIVERSAL_PREV);
    SystemTray::Instance().Remove();

    CoUninitialize();

    if (hMutex) {
        CloseHandle(hMutex);
    }

    return (int)msg.wParam;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    return wWinMain(hInstance, hPrevInstance, GetCommandLineW(), nCmdShow);
}

