#pragma once

#include "app.h"
#include <memory>

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
    // Destroys the window and frees artwork (some bitmaps hold COM streams). Run before CoUninitialize.
    void Shutdown();

    HWND GetHwnd() const { return m_hWnd; }

private:
    struct RenderCache;

    MiniplayerWindow();
    ~MiniplayerWindow();

    static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void Render(bool force = false);
    void OnTimer(UINT_PTR id);
    void UpdateTimers();
    void HandleMouseMove(float x, float y);
    void HandleMouseLeave();
    void HandleLButtonDown(float x, float y);
    void HandleMouseWheel(short delta);
    void ShowContextMenu(int x, int y);
    void OnHoverChanged();
    void EndDrag(bool commitSeek);
    void SyncClockPaused(bool paused);
    double DisplayTime(ULONGLONG now) const;
    double CurrentProgress() const;
    void SetDpi(UINT dpi);
    int Px(int logical) const { return MulDiv(logical, (int)m_dpi, 96); }
    float Scale() const { return m_dpi / 96.0f; }
    void CalculateBounds();
    void MaybeStartArtFetch();
    void OnArtwork(unsigned gen, LPARAM result);
    void ClearArt();

    HWND m_hWnd = nullptr;
    bool m_isHovered = false;
    bool m_isVisible = false;
    bool m_isDraggingVolume = false;
    bool m_isDraggingSeek = false;
    int m_lastNonZeroVolume = 50;
    int m_eqStep = 0;
    int m_dragVolume = 0;
    double m_seekDragPct = 0.0;

    // Logical (96-DPI) size; Px() converts to device pixels.
    int m_currentWidth = 44;
    int m_currentHeight = 160;
    UINT m_dpi = 96;

    bool m_animTimerOn = false;
    bool m_eqTimerOn = false;
    bool m_progressTimerOn = false;

    // The page reports the position at most once a second while playing and not at all while paused,
    // so the progress knob runs on a local clock based on the last report.
    double m_clockTime = 0.0;
    ULONGLONG m_clockTick = 0;
    bool m_clockPaused = true;
    ULONGLONG m_seekHoldUntil = 0;
    double m_seekHoldDuration = 0.0;

    RECT m_idleRect = {};
    RECT m_expandedRect = {};

    // Artwork state is UI-thread only; fetch workers hand their result over with a posted message.
    Gdiplus::Bitmap* m_artBig = nullptr;
    Gdiplus::Bitmap* m_artSmall = nullptr;
    int m_artPx = 0;
    unsigned m_artVersion = 0;
    std::wstring m_wantedArtUrl;
    std::wstring m_artUrl;
    unsigned m_fetchGen = 0;
    bool m_fetchBusy = false;
    ULONGLONG m_fetchTick = 0;
    ULONGLONG m_fetchFailedAt = 0;
    int m_wheelAccum = 0;

    std::unique_ptr<RenderCache> m_cache;
};
