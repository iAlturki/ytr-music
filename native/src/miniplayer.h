#pragma once

#include "app.h"

namespace Gdiplus { class Bitmap; }

class MiniplayerWindow {
public:
    static MiniplayerWindow& Instance();

    bool Create();
    void Show();
    void Hide();
    void Toggle();
    bool IsVisible() const;

    void UpdateSongState(const SongInfo& song);

    HWND GetHwnd() const { return m_hWnd; }

private:
    MiniplayerWindow();
    ~MiniplayerWindow();

    static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void Render();
    void HandleMouseMove(int x, int y);
    void HandleMouseLeave();
    void HandleLButtonDown(int x, int y);
    void HandleMouseWheel(short delta);
    void ShowContextMenu(int x, int y);

    HWND m_hWnd = nullptr;
    bool m_isHovered = false;
    bool m_isVisible = false;
    bool m_isDraggingVolume = false;
    bool m_isDraggingSeek = false;
    int m_lastNonZeroVolume = 50;
    int m_eqStep = 0;

    int m_currentWidth = 44;
    int m_currentHeight = 160;
    int m_targetWidth = 44;
    int m_targetHeight = 160;

    RECT m_idleRect;
    RECT m_expandedRect;

    Gdiplus::Bitmap* m_pArtworkBmp = nullptr;
    std::wstring m_lastArtworkUrl;
    CRITICAL_SECTION m_artCs;

    void CalculateBounds();
};
