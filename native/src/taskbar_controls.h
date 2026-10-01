#pragma once

#include "app.h"
#include <shobjidl.h>

#define THUMB_BTN_PREV 1001
#define THUMB_BTN_PLAYPAUSE 1002
#define THUMB_BTN_NEXT 1003

class TaskbarControls {
public:
    static TaskbarControls& Instance();

    void Initialize(HWND hWnd);
    void UpdateState(const SongInfo& song);
    bool HandleCommand(WORD cmdId);
    UINT GetTaskbarButtonCreatedMsg() const { return m_wmTaskbarButtonCreated; }
    // Sent for every new taskbar button: first show, re-show after SW_HIDE, Explorer restart.
    void OnTaskbarButtonCreated();
    // Releases ITaskbarList3 and the icons. Must run before CoUninitialize.
    void Shutdown();

private:
    TaskbarControls();
    ~TaskbarControls();

    void CreateThumbButtons();
    void EnsureIcons();
    void InvalidateApplied();
    HICON CreateButtonIcon(int type); // 0 = play, 1 = pause, 2 = next, 3 = prev

    HWND m_hWnd = nullptr;
    ITaskbarList3* m_pTaskbar = nullptr;
    UINT m_wmTaskbarButtonCreated = 0;
    bool m_buttonsAdded = false;
    bool m_shutdown = false;

    // Last state pushed to the taskbar button; state ticks only call into Explorer on a change.
    int m_appliedPaused = -1;
    bool m_tipApplied = false;
    std::wstring m_appliedTitle;
    std::wstring m_appliedArtist;
    int m_appliedProgState = -1;
    int m_appliedStep = -1;

    HICON m_hIconPlay = nullptr;
    HICON m_hIconPause = nullptr;
    HICON m_hIconNext = nullptr;
    HICON m_hIconPrev = nullptr;
};
