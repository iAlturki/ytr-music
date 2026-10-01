#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <string>
#include <functional>
#include <vector>

struct SongInfo {
    std::wstring title = L"Ready to Play";
    std::wstring artist = L"ytr-music";
    std::wstring album = L"";
    std::wstring artworkUrl = L"";
    bool isPaused = true;
    double currentTime = 0.0;
    double duration = 0.0;
    int volume = 100;
    bool isLiked = false;
    bool isDisliked = false;
};

// Global App Actions
void App_SendControl(const std::wstring& action);
void App_SeekTo(double seconds);
void App_SetVolume(int volumePercent);
void App_OnSongStateUpdated(const SongInfo& song);
// Called by the engine only when the paused state reported by the page flips.
void App_OnPagePlaybackChanged(bool pagePaused);
void App_ToggleMiniplayer();
void App_ShowMainWindow();
void App_HideMainWindow();
void App_Quit();
// True when launched with --debug (enables DevTools port + debug.log).
bool App_IsDebugMode();
// No-op unless App_IsDebugMode().
void LogBridge(const std::wstring& text);

extern SongInfo g_currentSong;
extern HWND g_hMainWindow;
extern HWND g_hMiniplayerWnd;
