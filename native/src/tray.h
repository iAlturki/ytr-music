#pragma once

#include "app.h"
#include <shellapi.h>

#define WM_TRAYICON (WM_USER + 101)

class SystemTray {
public:
    static SystemTray& Instance();

    bool Initialize(HWND hWndOwner);
    void Remove();
    void UpdateTooltip(const std::wstring& text);
    void HandleTrayMessage(WPARAM wParam, LPARAM lParam);
    // Broadcast by Explorer whenever it (re)creates the taskbar; the icon must be added again.
    UINT GetTaskbarCreatedMsg() const { return m_wmTaskbarCreated; }
    void OnTaskbarCreated();

private:
    SystemTray();
    ~SystemTray();

    void ShowMenu();

    HWND m_hWndOwner = nullptr;
    NOTIFYICONDATAW m_nid = {};
    UINT m_wmTaskbarCreated = 0;
    bool m_isAdded = false;
    bool m_tipSynced = false;
    bool m_swallowUp = false;
};
