#include "taskbar_controls.h"
#include <objbase.h>
#include <gdiplus.h>

using namespace Gdiplus;

TaskbarControls& TaskbarControls::Instance() {
    static TaskbarControls instance;
    return instance;
}

TaskbarControls::TaskbarControls() {
    m_wmTaskbarButtonCreated = RegisterWindowMessageW(L"TaskbarButtonCreated");
}

TaskbarControls::~TaskbarControls() {
    Shutdown();
}

void TaskbarControls::Shutdown() {
    // Blocks a late TaskbarButtonCreated from re-creating the interface after this point.
    m_shutdown = true;
    if (m_pTaskbar) {
        m_pTaskbar->Release();
        m_pTaskbar = nullptr;
    }
    m_buttonsAdded = false;
    m_hWnd = nullptr;
    if (m_hIconPlay) { DestroyIcon(m_hIconPlay); m_hIconPlay = nullptr; }
    if (m_hIconPause) { DestroyIcon(m_hIconPause); m_hIconPause = nullptr; }
    if (m_hIconNext) { DestroyIcon(m_hIconNext); m_hIconNext = nullptr; }
    if (m_hIconPrev) { DestroyIcon(m_hIconPrev); m_hIconPrev = nullptr; }
}

HICON TaskbarControls::CreateButtonIcon(int type) {
    const int sz = 20;
    Bitmap bmp(sz, sz, PixelFormat32bppARGB);
    Graphics g(&bmp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.Clear(Color(0, 0, 0, 0));

    SolidBrush brush(Color(255, 255, 255, 255));
    Pen pen(Color(255, 255, 255, 255), 1.5f);

    float mid = sz / 2.0f;

    if (type == 0) {
        // Play triangle
        PointF pts[3] = { { mid - 4.0f, mid - 6.0f }, { mid + 6.0f, mid }, { mid - 4.0f, mid + 6.0f } };
        g.FillPolygon(&brush, pts, 3);
    } else if (type == 1) {
        // Pause bars
        g.FillRectangle(&brush, mid - 5.0f, mid - 6.0f, 3.5f, 12.0f);
        g.FillRectangle(&brush, mid + 1.5f, mid - 6.0f, 3.5f, 12.0f);
    } else if (type == 2) {
        // Next Track
        PointF pts[3] = { { mid - 4.0f, mid - 6.0f }, { mid + 4.0f, mid }, { mid - 4.0f, mid + 6.0f } };
        g.FillPolygon(&brush, pts, 3);
        g.DrawLine(&pen, mid + 4.5f, mid - 6.0f, mid + 4.5f, mid + 6.0f);
    } else if (type == 3) {
        // Previous Track
        PointF pts[3] = { { mid + 4.0f, mid - 6.0f }, { mid - 4.0f, mid }, { mid + 4.0f, mid + 6.0f } };
        g.FillPolygon(&brush, pts, 3);
        g.DrawLine(&pen, mid - 4.5f, mid - 6.0f, mid - 4.5f, mid + 6.0f);
    }

    HICON hIcon = nullptr;
    bmp.GetHICON(&hIcon);
    return hIcon;
}

void TaskbarControls::EnsureIcons() {
    if (m_hIconPlay && m_hIconPause && m_hIconNext && m_hIconPrev) return;
    // Own GDI+ session (reference counted): only HICONs outlive it, and the button can be
    // created before anything else has started GDI+.
    GdiplusStartupInput input;
    ULONG_PTR token = 0;
    if (GdiplusStartup(&token, &input, NULL) != Ok) return;
    if (!m_hIconPlay) m_hIconPlay = CreateButtonIcon(0);
    if (!m_hIconPause) m_hIconPause = CreateButtonIcon(1);
    if (!m_hIconNext) m_hIconNext = CreateButtonIcon(2);
    if (!m_hIconPrev) m_hIconPrev = CreateButtonIcon(3);
    GdiplusShutdown(token);
}

void TaskbarControls::Initialize(HWND hWnd) {
    m_hWnd = hWnd;
    ChangeWindowMessageFilterEx(hWnd, m_wmTaskbarButtonCreated, MSGFLT_ALLOW, NULL);
}

void TaskbarControls::OnTaskbarButtonCreated() {
    if (m_shutdown || !m_hWnd) return;

    // Fresh interface per button: the event is rare and this survives an Explorer restart.
    if (m_pTaskbar) {
        m_pTaskbar->Release();
        m_pTaskbar = nullptr;
    }
    HRESULT hr = CoCreateInstance(CLSID_TaskbarList, NULL, CLSCTX_INPROC_SERVER, IID_ITaskbarList3, (void**)&m_pTaskbar);
    if (FAILED(hr) || !m_pTaskbar) {
        m_pTaskbar = nullptr;
        return;
    }
    if (FAILED(m_pTaskbar->HrInit())) {
        m_pTaskbar->Release();
        m_pTaskbar = nullptr;
        return;
    }

    // A new button has no thumbnail toolbar, tooltip or progress yet. While paused the page
    // sends no state ticks, so push the full state now.
    m_buttonsAdded = false;
    InvalidateApplied();
    CreateThumbButtons();
    UpdateState(g_currentSong);
}

void TaskbarControls::CreateThumbButtons() {
    if (!m_pTaskbar || !m_hWnd) return;

    EnsureIcons();

    THUMBBUTTON buttons[3] = {};

    // 0: Previous
    buttons[0].dwMask = THB_ICON | THB_TOOLTIP | THB_FLAGS;
    buttons[0].iId = THUMB_BTN_PREV;
    buttons[0].hIcon = m_hIconPrev;
    wcscpy_s(buttons[0].szTip, L"Previous Track");
    buttons[0].dwFlags = THBF_ENABLED;

    // 1: Play / Pause
    buttons[1].dwMask = THB_ICON | THB_TOOLTIP | THB_FLAGS;
    buttons[1].iId = THUMB_BTN_PLAYPAUSE;
    buttons[1].hIcon = g_currentSong.isPaused ? m_hIconPlay : m_hIconPause;
    wcscpy_s(buttons[1].szTip, g_currentSong.isPaused ? L"Play" : L"Pause");
    buttons[1].dwFlags = THBF_ENABLED;

    // 2: Next
    buttons[2].dwMask = THB_ICON | THB_TOOLTIP | THB_FLAGS;
    buttons[2].iId = THUMB_BTN_NEXT;
    buttons[2].hIcon = m_hIconNext;
    wcscpy_s(buttons[2].szTip, L"Next Track");
    buttons[2].dwFlags = THBF_ENABLED;

    HRESULT hr = m_pTaskbar->ThumbBarAddButtons(m_hWnd, 3, buttons);
    // Add works once per taskbar button; a repeated notification finds the toolbar already there.
    if (FAILED(hr)) hr = m_pTaskbar->ThumbBarUpdateButtons(m_hWnd, 3, buttons);
    m_buttonsAdded = SUCCEEDED(hr);
    if (m_buttonsAdded) m_appliedPaused = g_currentSong.isPaused ? 1 : 0;
}

void TaskbarControls::InvalidateApplied() {
    m_appliedPaused = -1;
    m_tipApplied = false;
    m_appliedProgState = -1;
    m_appliedStep = -1;
}

void TaskbarControls::UpdateState(const SongInfo& song) {
    if (!m_pTaskbar || !m_hWnd) return;
    // Hidden to the tray there is no taskbar button; TaskbarButtonCreated re-pushes on show.
    if (!IsWindowVisible(m_hWnd)) {
        InvalidateApplied();
        return;
    }

    // Play/Pause thumbnail button
    const int paused = song.isPaused ? 1 : 0;
    if (m_buttonsAdded && paused != m_appliedPaused) {
        THUMBBUTTON btn = {};
        btn.dwMask = THB_ICON | THB_TOOLTIP;
        btn.iId = THUMB_BTN_PLAYPAUSE;
        btn.hIcon = song.isPaused ? m_hIconPlay : m_hIconPause;
        wcscpy_s(btn.szTip, song.isPaused ? L"Play" : L"Pause");
        if (SUCCEEDED(m_pTaskbar->ThumbBarUpdateButtons(m_hWnd, 1, &btn))) m_appliedPaused = paused;
    }

    // Hover thumbnail tooltip
    if (!m_tipApplied || song.title != m_appliedTitle || song.artist != m_appliedArtist) {
        std::wstring tip = song.title + L" - " + song.artist;
        if (tip.length() > 250) tip = tip.substr(0, 247) + L"...";
        if (SUCCEEDED(m_pTaskbar->SetThumbnailTooltip(m_hWnd, tip.c_str()))) {
            m_appliedTitle = song.title;
            m_appliedArtist = song.artist;
            m_tipApplied = true;
        }
    }

    // Progress bar on the taskbar icon, in whole percent (finer steps are invisible at that size)
    const bool showProgress = song.duration > 0 && !song.isPaused;
    const int progState = showProgress ? TBPF_NORMAL : TBPF_NOPROGRESS;
    if (progState != m_appliedProgState) {
        if (FAILED(m_pTaskbar->SetProgressState(m_hWnd, (TBPFLAG)progState))) return;
        m_appliedProgState = progState;
        m_appliedStep = -1;
    }
    if (showProgress) {
        double f = song.currentTime / song.duration;
        if (!(f > 0.0)) f = 0.0;
        if (f > 1.0) f = 1.0;
        const int step = (int)(f * 100.0);
        if (step != m_appliedStep && SUCCEEDED(m_pTaskbar->SetProgressValue(m_hWnd, (ULONGLONG)step, 100))) {
            m_appliedStep = step;
        }
    }
}

bool TaskbarControls::HandleCommand(WORD cmdId) {
    if (cmdId == THUMB_BTN_PREV) {
        App_SendControl(L"previous");
        return true;
    }
    if (cmdId == THUMB_BTN_PLAYPAUSE) {
        App_SendControl(L"playPause");
        return true;
    }
    if (cmdId == THUMB_BTN_NEXT) {
        App_SendControl(L"next");
        return true;
    }
    return false;
}
