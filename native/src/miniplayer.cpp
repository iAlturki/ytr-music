#include "miniplayer.h"
#include <objbase.h>
#include <gdiplus.h>
#include <windowsx.h>
#include <cmath>
#include <urlmon.h>
#include <thread>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "msimg32.lib")
#pragma comment(lib, "urlmon.lib")

using namespace Gdiplus;

static ULONG_PTR g_gdiplusToken = 0;

MiniplayerWindow& MiniplayerWindow::Instance() {
    static MiniplayerWindow instance;
    return instance;
}

MiniplayerWindow::MiniplayerWindow() {
    InitializeCriticalSection(&m_artCs);
    GdiplusStartupInput gdiplusStartupInput;
    GdiplusStartup(&g_gdiplusToken, &gdiplusStartupInput, NULL);
}

MiniplayerWindow::~MiniplayerWindow() {
    if (m_hWnd) {
        DestroyWindow(m_hWnd);
        m_hWnd = nullptr;
    }
    EnterCriticalSection(&m_artCs);
    if (m_pArtworkBmp) {
        delete m_pArtworkBmp;
        m_pArtworkBmp = nullptr;
    }
    LeaveCriticalSection(&m_artCs);
    DeleteCriticalSection(&m_artCs);

    if (g_gdiplusToken) {
        GdiplusShutdown(g_gdiplusToken);
        g_gdiplusToken = 0;
    }
}

void MiniplayerWindow::CalculateBounds() {
    RECT workArea;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);

    const int IDLE_WIDTH = 44;
    const int IDLE_HEIGHT = 160;
    const int EXPANDED_WIDTH = 340;
    const int EXPANDED_HEIGHT = 224;
    const int MARGIN_RIGHT = 6;
    const int OFFSET_BOTTOM = 44;

    m_idleRect.left = workArea.right - IDLE_WIDTH - MARGIN_RIGHT;
    m_idleRect.top = workArea.bottom - IDLE_HEIGHT - OFFSET_BOTTOM;
    m_idleRect.right = m_idleRect.left + IDLE_WIDTH;
    m_idleRect.bottom = m_idleRect.top + IDLE_HEIGHT;

    m_expandedRect.left = workArea.right - EXPANDED_WIDTH - MARGIN_RIGHT;
    m_expandedRect.top = workArea.bottom - EXPANDED_HEIGHT - OFFSET_BOTTOM;
    m_expandedRect.right = m_expandedRect.left + EXPANDED_WIDTH;
    m_expandedRect.bottom = m_expandedRect.top + EXPANDED_HEIGHT;
}

bool MiniplayerWindow::Create() {
    CalculateBounds();

    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"YTRMusicMiniplayerClass";
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    RegisterClassExW(&wc);

    m_currentWidth = 44;
    m_currentHeight = 160;

    m_hWnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"YTRMusicMiniplayerClass",
        L"ytr-music Miniplayer",
        WS_POPUP,
        m_idleRect.left, m_idleRect.top,
        m_currentWidth, m_currentHeight,
        NULL, NULL, GetModuleHandleW(NULL), this
    );

    if (!m_hWnd) return false;

    g_hMiniplayerWnd = m_hWnd;

    // Timer for smooth hover animation and EQ frequency bars
    SetTimer(m_hWnd, 1, 16, NULL); // ~60fps ticker
    SetTimer(m_hWnd, 2, 120, NULL); // EQ animation ticker

    return true;
}

void MiniplayerWindow::Show() {
    if (!m_hWnd) Create();
    m_isVisible = true;
    CalculateBounds();
    SetWindowPos(m_hWnd, HWND_TOPMOST, m_idleRect.left, m_idleRect.top, m_currentWidth, m_currentHeight, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    Render();
}

void MiniplayerWindow::Hide() {
    m_isVisible = false;
    if (m_hWnd) {
        ShowWindow(m_hWnd, SW_HIDE);
    }
}

void MiniplayerWindow::Toggle() {
    if (m_isVisible) Hide();
    else Show();
}

bool MiniplayerWindow::IsVisible() const {
    return m_isVisible;
}

void MiniplayerWindow::UpdateSongState(const SongInfo& song) {
    if (song.artworkUrl != m_lastArtworkUrl) {
        m_lastArtworkUrl = song.artworkUrl;
        if (song.artworkUrl.empty()) {
            EnterCriticalSection(&m_artCs);
            if (m_pArtworkBmp) {
                delete m_pArtworkBmp;
                m_pArtworkBmp = nullptr;
            }
            LeaveCriticalSection(&m_artCs);
            if (m_isVisible) Render();
        } else {
            std::wstring url = song.artworkUrl;
            std::thread([this, url]() {
                CoInitializeEx(NULL, COINIT_MULTITHREADED);
                IStream* pStream = nullptr;
                HRESULT hr = URLOpenBlockingStreamW(NULL, url.c_str(), &pStream, 0, NULL);
                if (SUCCEEDED(hr) && pStream) {
                    Gdiplus::Bitmap* newBmp = Gdiplus::Bitmap::FromStream(pStream);
                    pStream->Release();
                    if (newBmp && newBmp->GetLastStatus() == Gdiplus::Ok) {
                        EnterCriticalSection(&m_artCs);
                        if (m_lastArtworkUrl == url) {
                            if (m_pArtworkBmp) delete m_pArtworkBmp;
                            m_pArtworkBmp = newBmp;
                            LeaveCriticalSection(&m_artCs);
                            if (m_hWnd) {
                                PostMessageW(m_hWnd, WM_TIMER, 99, 0);
                            }
                        } else {
                            delete newBmp;
                            LeaveCriticalSection(&m_artCs);
                        }
                    } else if (newBmp) {
                        delete newBmp;
                    }
                }
                CoUninitialize();
            }).detach();
        }
    }

    if (m_isVisible) {
        Render();
    }
}

LRESULT CALLBACK MiniplayerWindow::WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    MiniplayerWindow* self = (MiniplayerWindow*)GetWindowLongPtrW(hWnd, GWLP_USERDATA);
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lParam;
        self = (MiniplayerWindow*)cs->lpCreateParams;
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, (LONG_PTR)self);
    }

    if (!self) return DefWindowProcW(hWnd, msg, wParam, lParam);

    switch (msg) {
        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT tme = { sizeof(TRACKMOUSEEVENT), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
            self->HandleMouseMove(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        }
        case WM_MOUSELEAVE: {
            self->HandleMouseLeave();
            return 0;
        }
        case WM_LBUTTONDOWN: {
            self->HandleLButtonDown(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        }
        case WM_LBUTTONUP: {
            if (self->m_isDraggingVolume || self->m_isDraggingSeek) {
                self->m_isDraggingVolume = false;
                self->m_isDraggingSeek = false;
                ReleaseCapture();
            }
            return 0;
        }
        case WM_RBUTTONUP: {
            POINT pt;
            GetCursorPos(&pt);
            self->ShowContextMenu(pt.x, pt.y);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            short delta = GET_WHEEL_DELTA_WPARAM(wParam);
            self->HandleMouseWheel(delta);
            return 0;
        }
        case WM_TIMER: {
            if (wParam == 1) {
                // Smooth interpolation between idle and expanded
                bool changed = false;
                int targetW = self->m_isHovered ? 340 : 44;
                int targetH = self->m_isHovered ? 224 : 160;

                if (self->m_currentWidth != targetW) {
                    int diff = targetW - self->m_currentWidth;
                    int step = diff / 3;
                    if (abs(step) < 2) step = (diff > 0) ? 2 : -2;
                    self->m_currentWidth += step;
                    if ((diff > 0 && self->m_currentWidth > targetW) || (diff < 0 && self->m_currentWidth < targetW)) {
                        self->m_currentWidth = targetW;
                    }
                    changed = true;
                }

                if (self->m_currentHeight != targetH) {
                    int diff = targetH - self->m_currentHeight;
                    int step = diff / 3;
                    if (abs(step) < 2) step = (diff > 0) ? 2 : -2;
                    self->m_currentHeight += step;
                    if ((diff > 0 && self->m_currentHeight > targetH) || (diff < 0 && self->m_currentHeight < targetH)) {
                        self->m_currentHeight = targetH;
                    }
                    changed = true;
                }

                if (changed) {
                    self->CalculateBounds();
                    int newX = self->m_expandedRect.right - self->m_currentWidth;
                    int newY = self->m_expandedRect.bottom - self->m_currentHeight;
                    SetWindowPos(self->m_hWnd, HWND_TOPMOST, newX, newY, self->m_currentWidth, self->m_currentHeight, SWP_NOACTIVATE);
                    self->Render();
                }
            } else if (wParam == 2) {
                if (!g_currentSong.isPaused && !self->m_isHovered) {
                    self->m_eqStep = (self->m_eqStep + 1) % 12;
                    self->Render();
                }
            } else if (wParam == 99) {
                if (self->m_isVisible) {
                    self->Render();
                }
            }
            return 0;
        }
        case WM_DESTROY: {
            KillTimer(hWnd, 1);
            KillTimer(hWnd, 2);
            return 0;
        }
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

void MiniplayerWindow::HandleMouseMove(int x, int y) {
    if (!m_isHovered) {
        m_isHovered = true;
    }
    if (m_isDraggingVolume) {
        int vol = (int)(((float)(x - 36) / 48.0f) * 100.0f + 0.5f);
        if (vol < 0) vol = 0;
        if (vol > 100) vol = 100;
        if (vol != g_currentSong.volume) {
            g_currentSong.volume = vol;
            App_SetVolume(vol);
            Render();
        }
    } else if (m_isDraggingSeek) {
        if (g_currentSong.duration > 0) {
            double pct = (double)(x - 20) / 300.0;
            if (pct < 0.0) pct = 0.0;
            if (pct > 1.0) pct = 1.0;
            g_currentSong.currentTime = pct * g_currentSong.duration;
            App_SeekTo(g_currentSong.currentTime);
            Render();
        }
    }
}

void MiniplayerWindow::HandleMouseLeave() {
    if (m_isHovered) {
        m_isHovered = false;
    }
}

void MiniplayerWindow::HandleLButtonDown(int x, int y) {
    if (m_currentWidth < 220) {
        // In idle dock view or while expanding: clicking anywhere resumes/pauses!
        App_SendControl(L"playPause");
        g_currentSong.isPaused = !g_currentSong.isPaused;
        Render();
        return;
    }

    // In expanded view:
    // 0. Volume Control: (Speaker at 14..32, Slider track at 33..86), Y: [155..189]
    if (y >= 155 && y <= 189 && x >= 14 && x <= 95) {
        if (x <= 32) {
            // Clicked speaker icon -> toggle mute
            if (g_currentSong.volume > 0) {
                m_lastNonZeroVolume = g_currentSong.volume;
                g_currentSong.volume = 0;
            } else {
                g_currentSong.volume = (m_lastNonZeroVolume > 0) ? m_lastNonZeroVolume : 50;
            }
            App_SetVolume(g_currentSong.volume);
            Render();
            return;
        } else {
            // Clicked volume slider track
            int vol = (int)(((float)(x - 36) / 48.0f) * 100.0f + 0.5f);
            if (vol < 0) vol = 0;
            if (vol > 100) vol = 100;
            g_currentSong.volume = vol;
            App_SetVolume(vol);
            Render();
            m_isDraggingVolume = true;
            SetCapture(m_hWnd);
            return;
        }
    }

    // 1. Play/Pause (Resume) button: center at (170, 172)
    // Generous hit box: radius 32px or rect [135..205, 138..206]
    int centerX = 170;
    int centerY = 172;
    int distSq = (x - centerX) * (x - centerX) + (y - centerY) * (y - centerY);
    if (distSq <= 1024 || (x >= 135 && x <= 205 && y >= 138 && y <= 206)) {
        App_SendControl(L"playPause");
        g_currentSong.isPaused = !g_currentSong.isPaused;
        Render();
        return;
    }

    // 2. Prev button: center at (120, 172), generous radius 24px
    distSq = (x - 120) * (x - 120) + (y - centerY) * (y - centerY);
    if (distSq <= 576 || (x >= 95 && x <= 140 && y >= 148 && y <= 196)) {
        App_SendControl(L"previous");
        return;
    }

    // 3. Next button: center at (220, 172), generous radius 24px
    distSq = (x - 220) * (x - 220) + (y - centerY) * (y - centerY);
    if (distSq <= 576 || (x >= 200 && x <= 245 && y >= 148 && y <= 196)) {
        App_SendControl(L"next");
        return;
    }

    // 4. Like button: center at (265, 172), generous radius 24px
    distSq = (x - 265) * (x - 265) + (y - centerY) * (y - centerY);
    if (distSq <= 576 || (x >= 245 && x <= 290 && y >= 148 && y <= 196)) {
        App_SendControl(L"like");
        g_currentSong.isLiked = !g_currentSong.isLiked;
        Render();
        return;
    }

    // 5. Restore Main Window button: (298, 16)
    if (x >= 285 && x <= 335 && y >= 8 && y <= 45) {
        App_ShowMainWindow();
        return;
    }

    // 6. Progress Bar Scrubber: track is drawn at trackY = 116.0f, x from 20 to 320
    if (y >= 104 && y <= 130 && x >= 15 && x <= 325) {
        if (g_currentSong.duration > 0) {
            double pct = (double)(x - 20) / 300.0;
            if (pct < 0.0) pct = 0.0;
            if (pct > 1.0) pct = 1.0;
            g_currentSong.currentTime = pct * g_currentSong.duration;
            App_SeekTo(g_currentSong.currentTime);
            Render();
            m_isDraggingSeek = true;
            SetCapture(m_hWnd);
        }
        return;
    }
}

void MiniplayerWindow::HandleMouseWheel(short delta) {
    int currentVol = g_currentSong.volume;
    int step = (delta > 0) ? 5 : -5;
    int newVol = currentVol + step;
    if (newVol < 0) newVol = 0;
    if (newVol > 100) newVol = 100;
    g_currentSong.volume = newVol;
    App_SetVolume(newVol);
    Render();
}

void MiniplayerWindow::ShowContextMenu(int x, int y) {
    HMENU hMenu = CreatePopupMenu();
    InsertMenuW(hMenu, 0, MF_BYPOSITION | MF_STRING, 101, g_currentSong.isPaused ? L"Play" : L"Pause");
    InsertMenuW(hMenu, 1, MF_BYPOSITION | MF_STRING, 102, L"Next Track");
    InsertMenuW(hMenu, 2, MF_BYPOSITION | MF_STRING, 103, L"Previous Track");
    InsertMenuW(hMenu, 3, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
    InsertMenuW(hMenu, 4, MF_BYPOSITION | MF_STRING, 104, L"Restore ytr-music");
    InsertMenuW(hMenu, 5, MF_BYPOSITION | MF_STRING, 105, L"Close Miniplayer");

    SetForegroundWindow(m_hWnd);
    int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, x, y, 0, m_hWnd, NULL);
    DestroyMenu(hMenu);

    if (cmd == 101) App_SendControl(L"playPause");
    else if (cmd == 102) App_SendControl(L"next");
    else if (cmd == 103) App_SendControl(L"previous");
    else if (cmd == 104) App_ShowMainWindow();
    else if (cmd == 105) Hide();
}

void MiniplayerWindow::Render() {
    if (!m_hWnd || !m_isVisible) return;

    int width = m_currentWidth;
    int height = m_currentHeight;
    if (width <= 0 || height <= 0) return;

    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height; // Top-down DIB
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pBits = nullptr;
    HBITMAP hBmp = CreateDIBSection(hdcScreen, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);
    HGDIOBJ hOldBmp = SelectObject(hdcMem, hBmp);

    // Initialize GDI+ Graphics object on memory DC
    Graphics g(hdcMem);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    // Clear transparent
    g.Clear(Color(0, 0, 0, 0));

    // Dynamic song accent color: warm amber/orange (#ff3d00)
    Color accentColor(255, 255, 61, 0);
    Color secondaryColor(255, 255, 120, 50);

    // 10% translucent glassmorphic card
    // Rounded card bounds
    RectF cardRect(1.0f, 1.0f, (float)(width - 2), (float)(height - 2));
    GraphicsPath path;
    float radius = 14.0f;
    path.AddArc(cardRect.X, cardRect.Y, radius, radius, 180, 90);
    path.AddArc(cardRect.GetRight() - radius, cardRect.Y, radius, radius, 270, 90);
    path.AddArc(cardRect.GetRight() - radius, cardRect.GetBottom() - radius, radius, radius, 0, 90);
    path.AddArc(cardRect.X, cardRect.GetBottom() - radius, radius, radius, 90, 90);
    path.CloseFigure();

    // 10% Translucent Gradient Fill (Alpha: 26 out of 255 = 10% opacity!)
    LinearGradientBrush bgBrush(cardRect, Color(35, 255, 61, 0), Color(24, 15, 18, 26), LinearGradientModeForwardDiagonal);
    g.FillPath(&bgBrush, &path);

    // Subtle luminous glass border
    Pen borderPen(Color(100, 255, 61, 0), 1.2f);
    g.DrawPath(&borderPen, &path);

    if (!m_isHovered) {
        // ==========================================
        // IDLE VIEW: Vertical Top-to-Bottom Dock
        // ==========================================
        // Album Art circle/rounded rect at top
        RectF artRect(6.0f, 10.0f, 32.0f, 32.0f);
        bool drawnIdleArt = false;
        EnterCriticalSection(&m_artCs);
        if (m_pArtworkBmp) {
            GraphicsPath clipPath;
            clipPath.AddEllipse(artRect);
            g.SetClip(&clipPath, CombineModeReplace);
            g.DrawImage(m_pArtworkBmp, artRect);
            g.ResetClip();
            drawnIdleArt = true;
        }
        LeaveCriticalSection(&m_artCs);

        if (!drawnIdleArt) {
            SolidBrush artBg(Color(120, 30, 32, 45));
            g.FillEllipse(&artBg, artRect);

            // Music Note icon in center of art
            Font font(L"Segoe UI Symbol", 12.0f, FontStyleRegular);
            SolidBrush whiteBrush(Color(240, 255, 255, 255));
            StringFormat sf;
            sf.SetAlignment(StringAlignmentCenter);
            sf.SetLineAlignment(StringAlignmentCenter);
            g.DrawString(L"\u266A", -1, &font, artRect, &sf, &whiteBrush);
        }
        Pen artBorder(Color(160, 255, 255, 255), 1.0f);
        g.DrawEllipse(&artBorder, artRect);

        // Outlined Play/Pause icon in middle (Y around 70)
        float cx = 22.0f;
        float cy = 76.0f;
        Pen ctrlOutline(Color(255, 255, 255, 255), 1.6f);
        g.DrawEllipse(&ctrlOutline, cx - 14.0f, cy - 14.0f, 28.0f, 28.0f);

        if (g_currentSong.isPaused) {
            // Play triangle
            PointF pts[3] = { { cx - 3.0f, cy - 6.0f }, { cx + 6.0f, cy }, { cx - 3.0f, cy + 6.0f } };
            SolidBrush playBrush(Color(255, 255, 255, 255));
            g.FillPolygon(&playBrush, pts, 3);
        } else {
            // Pause 2 bars
            SolidBrush pauseBrush(Color(255, 255, 255, 255));
            g.FillRectangle(&pauseBrush, cx - 5.0f, cy - 6.0f, 3.0f, 12.0f);
            g.FillRectangle(&pauseBrush, cx + 2.0f, cy - 6.0f, 3.0f, 12.0f);
        }

        // Animated 3-Bar Audio Equalizer at bottom (Y around 125 to 145)
        float eqHeights[3] = { 6.0f, 14.0f, 9.0f };
        if (!g_currentSong.isPaused) {
            int s = m_eqStep;
            eqHeights[0] = 5.0f + 10.0f * (float)abs(sin(s * 0.5));
            eqHeights[1] = 7.0f + 12.0f * (float)abs(cos(s * 0.6));
            eqHeights[2] = 4.0f + 11.0f * (float)abs(sin(s * 0.4 + 1.0));
        }

        SolidBrush eqBrush(Color(230, 255, 61, 0));
        float barX = 13.0f;
        for (int i = 0; i < 3; ++i) {
            float h = eqHeights[i];
            g.FillRectangle(&eqBrush, barX, 142.0f - h, 4.0f, h);
            barX += 6.0f;
        }
    } else {
        // ==========================================
        // EXPANDED VIEW: Complete High-Performance Player
        // ==========================================
        StringFormat sfCenter;
        sfCenter.SetAlignment(StringAlignmentCenter);
        sfCenter.SetLineAlignment(StringAlignmentCenter);

        // Album art on the left (20, 20, 68x68)
        RectF artRect(20.0f, 20.0f, 68.0f, 68.0f);
        GraphicsPath artPath;
        float artR = 8.0f;
        artPath.AddArc(artRect.X, artRect.Y, artR, artR, 180, 90);
        artPath.AddArc(artRect.GetRight() - artR, artRect.Y, artR, artR, 270, 90);
        artPath.AddArc(artRect.GetRight() - artR, artRect.GetBottom() - artR, artR, artR, 0, 90);
        artPath.AddArc(artRect.X, artRect.GetBottom() - artR, artR, artR, 90, 90);
        artPath.CloseFigure();

        bool drawnExpArt = false;
        EnterCriticalSection(&m_artCs);
        if (m_pArtworkBmp) {
            g.SetClip(&artPath, CombineModeReplace);
            g.DrawImage(m_pArtworkBmp, artRect);
            g.ResetClip();
            drawnExpArt = true;
        }
        LeaveCriticalSection(&m_artCs);

        if (!drawnExpArt) {
            SolidBrush artBg(Color(180, 24, 28, 40));
            g.FillPath(&artBg, &artPath);

            Font bigMusicFont(L"Segoe UI Symbol", 24.0f, FontStyleRegular);
            SolidBrush whiteBrush(Color(255, 255, 255, 255));
            g.DrawString(L"\u266A", -1, &bigMusicFont, artRect, &sfCenter, &whiteBrush);
        }
        Pen artBorder(Color(180, 255, 255, 255), 1.2f);
        g.DrawPath(&artBorder, &artPath);

        // Title and Artist texts
        Font titleFont(L"Segoe UI", 12.0f, FontStyleBold);
        Font artistFont(L"Segoe UI", 10.0f, FontStyleRegular);
        SolidBrush titleBrush(Color(255, 255, 255, 255));
        SolidBrush artistBrush(Color(200, 210, 220, 230));

        RectF titleRect(100.0f, 24.0f, 185.0f, 24.0f);
        RectF artistRect(100.0f, 50.0f, 185.0f, 20.0f);

        StringFormat sfLeft;
        sfLeft.SetTrimming(StringTrimmingEllipsisCharacter);
        sfLeft.SetFormatFlags(StringFormatFlagsNoWrap);

        g.DrawString(g_currentSong.title.c_str(), -1, &titleFont, titleRect, &sfLeft, &titleBrush);
        g.DrawString(g_currentSong.artist.c_str(), -1, &artistFont, artistRect, &sfLeft, &artistBrush);

        // Restore Window / PiP Button (Top Right)
        RectF restoreRect(298.0f, 16.0f, 26.0f, 26.0f);
        Pen restorePen(Color(180, 255, 255, 255), 1.4f);
        g.DrawRectangle(&restorePen, restoreRect.X + 4, restoreRect.Y + 4, 16.0f, 16.0f);
        g.DrawRectangle(&restorePen, restoreRect.X + 8, restoreRect.Y + 8, 8.0f, 8.0f);

        // Track Progress Slider (X: 20 to 320, Y: 116)
        float trackY = 116.0f;
        Pen barBgPen(Color(100, 255, 255, 255), 3.0f);
        g.DrawLine(&barBgPen, 20.0f, trackY, 320.0f, trackY);

        double progress = 0.0;
        if (g_currentSong.duration > 0) {
            progress = g_currentSong.currentTime / g_currentSong.duration;
            if (progress > 1.0) progress = 1.0;
        }

        float filledX = 20.0f + (float)(progress * 300.0);
        Pen barFilledPen(Color(255, 255, 61, 0), 3.0f);
        g.DrawLine(&barFilledPen, 20.0f, trackY, filledX, trackY);

        // Knob handle
        SolidBrush knobBrush(Color(255, 255, 255, 255));
        g.FillEllipse(&knobBrush, filledX - 5.0f, trackY - 5.0f, 10.0f, 10.0f);

        // Control Buttons row:
        // Previous (120, 172)
        float prevX = 120.0f;
        float ctrlY = 172.0f;
        Pen prevPen(Color(220, 255, 255, 255), 1.5f);
        PointF prevPts[3] = { { prevX + 4.0f, ctrlY - 7.0f }, { prevX - 6.0f, ctrlY }, { prevX + 4.0f, ctrlY + 7.0f } };
        g.DrawPolygon(&prevPen, prevPts, 3);
        g.DrawLine(&prevPen, prevX - 6.0f, ctrlY - 7.0f, prevX - 6.0f, ctrlY + 7.0f);

        // Outlined Play/Pause Center Button (170, 172)
        float playX = 170.0f;
        Pen playCircle(Color(255, 255, 255, 255), 2.0f);
        g.DrawEllipse(&playCircle, playX - 20.0f, ctrlY - 20.0f, 40.0f, 40.0f);

        if (g_currentSong.isPaused) {
            PointF playPts[3] = { { playX - 4.0f, ctrlY - 9.0f }, { playX + 8.0f, ctrlY }, { playX - 4.0f, ctrlY + 9.0f } };
            SolidBrush pBrush(Color(255, 255, 255, 255));
            g.FillPolygon(&pBrush, playPts, 3);
        } else {
            SolidBrush pBrush(Color(255, 255, 255, 255));
            g.FillRectangle(&pBrush, playX - 7.0f, ctrlY - 8.0f, 4.0f, 16.0f);
            g.FillRectangle(&pBrush, playX + 3.0f, ctrlY - 8.0f, 4.0f, 16.0f);
        }

        // Next (220, 172)
        float nextX = 220.0f;
        PointF nextPts[3] = { { nextX - 4.0f, ctrlY - 7.0f }, { nextX + 6.0f, ctrlY }, { nextX - 4.0f, ctrlY + 7.0f } };
        g.DrawPolygon(&prevPen, nextPts, 3);
        g.DrawLine(&prevPen, nextX + 6.0f, ctrlY - 7.0f, nextX + 6.0f, ctrlY + 7.0f);

        // Like Heart (265, 172)
        Font heartFont(L"Segoe UI Symbol", 13.0f, FontStyleRegular);
        RectF heartRect(253.0f, ctrlY - 12.0f, 24.0f, 24.0f);
        if (g_currentSong.isLiked) {
            SolidBrush heartBrush(Color(255, 255, 61, 0));
            g.DrawString(L"\u2665", -1, &heartFont, heartRect, &sfCenter, &heartBrush);
        } else {
            SolidBrush heartBrush(Color(180, 220, 225, 235));
            g.DrawString(L"\u2661", -1, &heartFont, heartRect, &sfCenter, &heartBrush);
        }

        // Volume Control on bottom left (ctrlY = 172.0f)
        // 1. Crisp Vector Speaker Icon (X: 16 to 28, Y: ctrlY - 6 to ctrlY + 6)
        Pen speakerPen(Color(200, 255, 255, 255), 1.2f);
        SolidBrush speakerBrush(Color(200, 255, 255, 255));
        // Back rectangle
        g.FillRectangle(&speakerBrush, 16.0f, ctrlY - 3.0f, 3.0f, 6.0f);
        // Cone polygon
        PointF conePts[4] = {
            { 19.0f, ctrlY - 3.0f },
            { 24.0f, ctrlY - 6.5f },
            { 24.0f, ctrlY + 6.5f },
            { 19.0f, ctrlY + 3.0f }
        };
        g.FillPolygon(&speakerBrush, conePts, 4);

        if (g_currentSong.volume > 0) {
            // Sound wave arc
            RectF waveRect(22.0f, ctrlY - 5.0f, 6.0f, 10.0f);
            g.DrawArc(&speakerPen, waveRect, -50, 100);
            if (g_currentSong.volume > 50) {
                RectF waveRect2(24.5f, ctrlY - 7.0f, 7.0f, 14.0f);
                g.DrawArc(&speakerPen, waveRect2, -50, 100);
            }
        } else {
            // Muted "X"
            Pen mutePen(Color(255, 255, 61, 0), 1.5f);
            g.DrawLine(&mutePen, 26.0f, ctrlY - 4.0f, 30.0f, ctrlY + 4.0f);
            g.DrawLine(&mutePen, 30.0f, ctrlY - 4.0f, 26.0f, ctrlY + 4.0f);
        }

        // 2. Volume Slider Track (X: 36 to 84, total 48px width)
        float volTrackX1 = 36.0f;
        float volTrackX2 = 84.0f;
        float volTrackW = volTrackX2 - volTrackX1;
        Pen volTrackBgPen(Color(70, 255, 255, 255), 3.0f);
        g.DrawLine(&volTrackBgPen, volTrackX1, ctrlY, volTrackX2, ctrlY);

        float volPct = (float)g_currentSong.volume / 100.0f;
        if (volPct < 0.0f) volPct = 0.0f;
        if (volPct > 1.0f) volPct = 1.0f;
        float volFilledX = volTrackX1 + volPct * volTrackW;

        Pen volTrackFilledPen(Color(255, 255, 61, 0), 3.0f);
        g.DrawLine(&volTrackFilledPen, volTrackX1, ctrlY, volFilledX, ctrlY);

        // Volume Slider Knob
        SolidBrush volKnobBrush(Color(255, 255, 255, 255));
        g.FillEllipse(&volKnobBrush, volFilledX - 4.0f, ctrlY - 4.0f, 8.0f, 8.0f);

        // 3. Volume % Readout Text (X: 88 to 112)
        Font volFont(L"Segoe UI", 7.5f, FontStyleRegular);
        RectF volRect(88.0f, ctrlY - 7.0f, 28.0f, 14.0f);
        WCHAR volText[16];
        swprintf_s(volText, L"%d%%", g_currentSong.volume);
        SolidBrush volTextBrush(Color(180, 200, 210, 220));
        g.DrawString(volText, -1, &volFont, volRect, &sfLeft, &volTextBrush);
    }

    // Blend to screen via UpdateLayeredWindow
    POINT ptSrc = { 0, 0 };
    SIZE size = { width, height };
    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    POINT ptDst;
    RECT wndRect;
    GetWindowRect(m_hWnd, &wndRect);
    ptDst.x = wndRect.left;
    ptDst.y = wndRect.top;

    UpdateLayeredWindow(m_hWnd, hdcScreen, &ptDst, &size, hdcMem, &ptSrc, 0, &blend, ULW_ALPHA);

    SelectObject(hdcMem, hOldBmp);
    DeleteObject(hBmp);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
}
