#include "tray.h"
#include "../res/resource.h"

SystemTray& SystemTray::Instance() {
    static SystemTray instance;
    return instance;
}

SystemTray::SystemTray() {
    m_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
}

SystemTray::~SystemTray() {
    Remove();
}

static bool IsMainWindowShown() {
    return g_hMainWindow && IsWindowVisible(g_hMainWindow) && !IsIconic(g_hMainWindow);
}

bool SystemTray::Initialize(HWND hWndOwner) {
    m_hWndOwner = hWndOwner;

    ZeroMemory(&m_nid, sizeof(NOTIFYICONDATAW));
    m_nid.cbSize = sizeof(NOTIFYICONDATAW);
    m_nid.hWnd = hWndOwner;
    m_nid.uID = 1;
    m_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    m_nid.uCallbackMessage = WM_TRAYICON;
    HINSTANCE hInst = GetModuleHandleW(NULL);
    m_nid.hIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (!m_nid.hIcon) m_nid.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APP_ICON));
    if (!m_nid.hIcon) m_nid.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION);
    wcscpy_s(m_nid.szTip, L"ytr-music (iALTURKi Native)");

    // m_nid stays fully populated even if this fails (no tray yet at early logon);
    // TaskbarCreated or the next tooltip change adds it later.
    m_isAdded = Shell_NotifyIconW(NIM_ADD, &m_nid) != FALSE;
    m_tipSynced = m_isAdded;
    return m_isAdded;
}

void SystemTray::Remove() {
    if (m_hWndOwner) {
        // Also when m_isAdded is false: an add that timed out may still have created the icon.
        Shell_NotifyIconW(NIM_DELETE, &m_nid);
        m_hWndOwner = nullptr;
    }
    m_isAdded = false;
    m_tipSynced = false;
}

void SystemTray::OnTaskbarCreated() {
    if (!m_hWndOwner) return;
    // Also sent when the taskbar is rebuilt with the icon still present (DPI changes),
    // so delete first or the add fails as a duplicate.
    Shell_NotifyIconW(NIM_DELETE, &m_nid);
    m_isAdded = Shell_NotifyIconW(NIM_ADD, &m_nid) != FALSE;
    m_tipSynced = m_isAdded;
}

void SystemTray::UpdateTooltip(const std::wstring& text) {
    if (!m_hWndOwner) return;
    wchar_t tip[ARRAYSIZE(m_nid.szTip)];
    wcsncpy_s(tip, text.c_str(), _TRUNCATE);
    // Runs on every state tick (~1 Hz while playing); only call into Explorer on a change.
    if (m_tipSynced && wcscmp(tip, m_nid.szTip) == 0) return;
    wcscpy_s(m_nid.szTip, tip);

    // Local copy with NIF_TIP only: the icon is not resent, and m_nid keeps the full flags for re-adds.
    NOTIFYICONDATAW nid = m_nid;
    nid.uFlags = NIF_TIP;
    bool ok = Shell_NotifyIconW(NIM_MODIFY, &nid) != FALSE;
    // Icon missing (initial add failed, Explorer restarted): add it again with the full flags.
    if (!ok) ok = Shell_NotifyIconW(NIM_ADD, &m_nid) != FALSE;
    // Never clear m_isAdded on failure: a timeout can fail while the icon exists. Unsynced retries next tick.
    if (ok) m_isAdded = true;
    m_tipSynced = ok;
}

void SystemTray::HandleTrayMessage(WPARAM, LPARAM lParam) {
    // A double-click arrives as UP, DBLCLK, UP: without this the second UP toggles the window away again.
    if (lParam == WM_LBUTTONDBLCLK) {
        m_swallowUp = true;
        App_ShowMainWindow();
        return;
    }
    if (lParam == WM_LBUTTONUP) {
        if (m_swallowUp) {
            m_swallowUp = false;
            return;
        }
        if (IsMainWindowShown()) {
            App_HideMainWindow();
        } else {
            App_ShowMainWindow();
        }
    } else if (lParam == WM_RBUTTONUP) {
        ShowMenu();
    }
}

void SystemTray::ShowMenu() {
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;
    InsertMenuW(hMenu, 0, MF_BYPOSITION | MF_STRING, 201, g_currentSong.isPaused ? L"Play" : L"Pause");
    InsertMenuW(hMenu, 1, MF_BYPOSITION | MF_STRING, 202, L"Next Track");
    InsertMenuW(hMenu, 2, MF_BYPOSITION | MF_STRING, 203, L"Previous Track");
    InsertMenuW(hMenu, 3, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
    InsertMenuW(hMenu, 4, MF_BYPOSITION | MF_STRING, 204, L"Desktop Miniplayer (PiP)");
    InsertMenuW(hMenu, 5, MF_BYPOSITION | MF_STRING, 205, IsMainWindowShown() ? L"Hide Window" : L"Show Window");
    InsertMenuW(hMenu, 6, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
    InsertMenuW(hMenu, 7, MF_BYPOSITION | MF_STRING, 207, L"Quit");

    POINT pt;
    GetCursorPos(&pt);
    // The owner must be foreground or the menu does not close on an outside click,
    // and the WM_NULL lets a second right-click open it again (KB135788).
    SetForegroundWindow(m_hWndOwner);
    int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, 0, m_hWndOwner, NULL);
    PostMessageW(m_hWndOwner, WM_NULL, 0, 0);
    DestroyMenu(hMenu);

    if (cmd == 201) App_SendControl(L"playPause");
    else if (cmd == 202) App_SendControl(L"next");
    else if (cmd == 203) App_SendControl(L"previous");
    else if (cmd == 204) App_ToggleMiniplayer();
    else if (cmd == 205) {
        if (IsMainWindowShown()) App_HideMainWindow();
        else App_ShowMainWindow();
    }
    else if (cmd == 207) App_Quit();
}
