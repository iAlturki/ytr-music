#include "miniplayer.h"
#include <objbase.h>
#include <gdiplus.h>
#include <windowsx.h>
#include <cmath>
#include <urlmon.h>
#include <thread>
#include <atomic>
#include <new>
#include <utility>


using namespace Gdiplus;

#define WM_MP_ARTWORK (WM_APP + 1)
#define WM_MP_REFRESH (WM_APP + 2)

static ULONG_PTR g_gdiplusToken = 0;

// Bumped on the UI thread whenever the wanted artwork changes (new URL or new DPI). Workers only read it,
// so a result for an older generation is dropped instead of shown.
static std::atomic<unsigned> s_artGen{0};

// Layout is in 96-DPI logical units; Px() converts to the window's device pixels.
static constexpr int kIdleW = 44;
static constexpr int kIdleH = 160;
static constexpr int kExpandedW = 340;
static constexpr int kExpandedH = 224;
static constexpr int kMarginRight = 6;
static constexpr int kOffsetBottom = 44;
static constexpr int kArtBigSize = 68;
static constexpr int kArtSmallSize = 32;
static constexpr float kVolX1 = 36.0f;
static constexpr float kVolX2 = 84.0f;
static constexpr float kCtrlY = 172.0f;
static constexpr float kSeekX1 = 20.0f;
static constexpr float kSeekW = 300.0f;

enum : UINT_PTR { TIMER_ANIM = 1, TIMER_EQ = 2, TIMER_PROGRESS = 3 };

struct ArtResult {
    Bitmap* bigBmp;
    Bitmap* smallBmp;
    int px;
};

// Everything that changes the pixels; Render skips the frame when it matches the last one.
struct RenderKey {
    int x = 0, y = 0, w = 0, h = 0;
    bool expanded = false;
    bool paused = false;
    bool liked = false;
    int eqStep = 0;
    int volume = 0;
    int progressPx = 0;
    unsigned artVersion = 0;
    std::wstring title;
    std::wstring artist;

    bool operator==(const RenderKey& o) const {
        return x == o.x && y == o.y && w == o.w && h == o.h && expanded == o.expanded &&
               paused == o.paused && liked == o.liked && eqStep == o.eqStep && volume == o.volume &&
               progressPx == o.progressPx && artVersion == o.artVersion && title == o.title && artist == o.artist;
    }
};

static bool EnsureGdiplus() {
    if (g_gdiplusToken) return true;
    GdiplusStartupInput input;
    if (GdiplusStartup(&g_gdiplusToken, &input, NULL) != Ok) g_gdiplusToken = 0;
    return g_gdiplusToken != 0;
}

// GetDpiForWindow is not declared at this toolchain's default WINVER (and is Windows 10 1607+).
static UINT QueryDpi(HWND hWnd) {
    typedef UINT (WINAPI *GetDpiForWindowFn)(HWND);
    static GetDpiForWindowFn getDpiForWindow = reinterpret_cast<GetDpiForWindowFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")));
    UINT dpi = (hWnd && getDpiForWindow) ? getDpiForWindow(hWnd) : 0;
    if (!dpi) {
        HDC hdc = GetDC(NULL);
        if (hdc) {
            dpi = (UINT)GetDeviceCaps(hdc, LOGPIXELSX);
            ReleaseDC(NULL, hdc);
        }
    }
    return dpi ? dpi : 96;
}

static void AddRoundRect(GraphicsPath& path, const RectF& r, float d) {
    path.AddArc(r.X, r.Y, d, d, 180, 90);
    path.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
    path.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
    path.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
    path.CloseFigure();
}

static void FreeBitmap(Bitmap*& bmp) {
    if (bmp) {
        delete bmp;
        bmp = nullptr;
    }
}

static void StepToward(int& value, int target) {
    if (value == target) return;
    int diff = target - value;
    int step = diff / 3;
    if (abs(step) < 2) step = (diff > 0) ? 2 : -2;
    value += step;
    if ((diff > 0 && value > target) || (diff < 0 && value < target)) value = target;
}

static int VolumeAt(float x) {
    float pct = (x - kVolX1) / (kVolX2 - kVolX1);
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 1.0f) pct = 1.0f;
    return (int)(pct * 100.0f + 0.5f);
}

static double SeekPctAt(float x) {
    double pct = (double)(x - kSeekX1) / kSeekW;
    if (pct < 0.0) pct = 0.0;
    if (pct > 1.0) pct = 1.0;
    return pct;
}

// Artwork is pre-scaled to device pixels, so it is drawn untransformed as a 1:1 copy.
static void DrawArtBitmap(Graphics& g, Bitmap* bmp, const RectF& rect, int px, float s) {
    g.ResetTransform();
    g.DrawImage(bmp, (INT)std::lround(rect.X * s), (INT)std::lround(rect.Y * s), px, px);
    g.ScaleTransform(s, s);
}

// lh3.googleusercontent.com / ggpht.com artwork carries its size in a "=w544-h544-..." suffix; ask for
// roughly what the miniplayer draws instead of the largest rendition the page lists.
static std::wstring SizedArtworkUrl(const std::wstring& url, int px) {
    if (url.find(L"googleusercontent.com/") == std::wstring::npos && url.find(L"ggpht.com/") == std::wstring::npos) {
        return url;
    }
    size_t end = url.find_first_of(L"?#");
    if (end == std::wstring::npos) end = url.size();
    if (end == 0) return url;
    const size_t eq = url.rfind(L'=', end - 1);
    const size_t slash = url.rfind(L'/', end - 1);
    if (eq == std::wstring::npos || (slash != std::wstring::npos && eq < slash)) return url;

    std::wstring opts;
    bool changed = false;
    size_t pos = eq + 1;
    for (;;) {
        size_t dash = url.find(L'-', pos);
        if (dash == std::wstring::npos || dash > end) dash = end;
        std::wstring tok = url.substr(pos, dash - pos);
        if (tok.size() > 1 && (tok[0] == L'w' || tok[0] == L'h' || tok[0] == L's') &&
            tok.find_first_not_of(L"0123456789", 1) == std::wstring::npos) {
            tok = tok.substr(0, 1) + std::to_wstring(px);
            changed = true;
        }
        opts += tok;
        if (dash == end) break;
        opts += L'-';
        pos = dash + 1;
    }
    if (!changed) return url;
    return url.substr(0, eq + 1) + opts + url.substr(end);
}

// Scales the source into an n x n device-pixel bitmap with the circle or rounded-rect mask baked in.
static Bitmap* BuildArtBitmap(Bitmap* src, const Rect& crop, int n, bool circle) {
    Bitmap scaled(n, n, PixelFormat32bppPARGB);
    if (scaled.GetLastStatus() != Ok) return nullptr;
    {
        Graphics g(&scaled);
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        // Mirrored edges keep bicubic from pulling transparent pixels into the border.
        ImageAttributes attrs;
        attrs.SetWrapMode(WrapModeTileFlipXY);
        g.DrawImage(src, Rect(0, 0, n, n), crop.X, crop.Y, crop.Width, crop.Height, UnitPixel, &attrs);
    }

    Bitmap* out = new Bitmap(n, n, PixelFormat32bppPARGB);
    if (!out) return nullptr;
    if (out->GetLastStatus() != Ok) {
        delete out;
        return nullptr;
    }
    {
        Graphics g(out);
        g.Clear(Color(0, 0, 0, 0));
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        TextureBrush texture(&scaled, WrapModeClamp);
        if (circle) {
            g.FillEllipse(&texture, 0, 0, n, n);
        } else {
            GraphicsPath path;
            AddRoundRect(path, RectF(0.0f, 0.0f, (REAL)n, (REAL)n), 8.0f * n / kArtBigSize);
            g.FillPath(&texture, &path);
        }
    }
    return out;
}

// Runs detached and never touches the MiniplayerWindow: everything it needs is passed by value.
static void ArtWorker(HWND hWnd, unsigned gen, std::wstring url, int bigPx, int smallPx) {
    HRESULT hrCo = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    ArtResult* result = nullptr;

    std::wstring sized = SizedArtworkUrl(url, bigPx * 2 > 120 ? bigPx * 2 : 120);
    IStream* stream = nullptr;
    HRESULT hr = URLOpenBlockingStreamW(NULL, sized.c_str(), &stream, 0, NULL);
    if ((FAILED(hr) || !stream) && sized != url) {
        if (stream) {
            stream->Release();
            stream = nullptr;
        }
        hr = URLOpenBlockingStreamW(NULL, url.c_str(), &stream, 0, NULL);
    }

    if (SUCCEEDED(hr) && stream && gen == s_artGen.load()) {
        Bitmap* src = Bitmap::FromStream(stream);
        if (src && src->GetLastStatus() == Ok && gen == s_artGen.load()) {
            const int sw = (int)src->GetWidth();
            const int sh = (int)src->GetHeight();
            const int side = sw < sh ? sw : sh;
            if (side > 0) {
                // Centre square: video thumbnails are 16:9 and would otherwise be stretched.
                Rect crop((sw - side) / 2, (sh - side) / 2, side, side);
                Bitmap* bigBmp = BuildArtBitmap(src, crop, bigPx, false);
                Bitmap* smallBmp = BuildArtBitmap(src, crop, smallPx, true);
                if (bigBmp && smallBmp) result = new (std::nothrow) ArtResult{ bigBmp, smallBmp, bigPx };
                if (!result) {
                    FreeBitmap(bigBmp);
                    FreeBitmap(smallBmp);
                }
            }
        }
        // The decoded bitmap holds a reference to the stream; drop it before the stream and COM go away.
        FreeBitmap(src);
    }
    if (stream) stream->Release();
    if (SUCCEEDED(hrCo)) CoUninitialize();

    // Report back even on failure so the UI thread can start the next queued fetch.
    if (!PostMessageW(hWnd, WM_MP_ARTWORK, (WPARAM)gen, (LPARAM)result) && result) {
        FreeBitmap(result->bigBmp);
        FreeBitmap(result->smallBmp);
        delete result;
    }
}

struct MiniplayerWindow::RenderCache {
    StringFormat center;
    StringFormat left;

    // Pixel-unit fonts at the sizes 96 DPI used to give; the world transform scales them with the layout.
    Font fontNoteSmall{ L"Segoe UI Symbol", 16.0f, FontStyleRegular, UnitPixel };
    Font fontNoteBig{ L"Segoe UI Symbol", 32.0f, FontStyleRegular, UnitPixel };
    Font fontTitle{ L"Segoe UI", 16.0f, FontStyleBold, UnitPixel };
    Font fontArtist{ L"Segoe UI", 40.0f / 3.0f, FontStyleRegular, UnitPixel };
    Font fontHeart{ L"Segoe UI Symbol", 52.0f / 3.0f, FontStyleRegular, UnitPixel };
    Font fontVolume{ L"Segoe UI", 10.0f, FontStyleRegular, UnitPixel };

    SolidBrush white{ Color(255, 255, 255, 255) };
    SolidBrush noteIdle{ Color(240, 255, 255, 255) };
    SolidBrush artBgIdle{ Color(120, 30, 32, 45) };
    SolidBrush artBgExpanded{ Color(180, 24, 28, 40) };
    SolidBrush eqBar{ Color(230, 255, 61, 0) };
    SolidBrush artistText{ Color(200, 210, 220, 230) };
    SolidBrush heartOn{ Color(255, 255, 61, 0) };
    SolidBrush heartOff{ Color(180, 220, 225, 235) };
    SolidBrush speaker{ Color(200, 255, 255, 255) };
    SolidBrush volumeText{ Color(180, 200, 210, 220) };

    Pen cardBorder{ Color(100, 255, 61, 0), 1.2f };
    Pen artBorderIdle{ Color(160, 255, 255, 255), 1.0f };
    Pen ctrlOutlineIdle{ Color(255, 255, 255, 255), 1.6f };
    Pen artBorderExpanded{ Color(180, 255, 255, 255), 1.2f };
    Pen restore{ Color(180, 255, 255, 255), 1.4f };
    Pen trackBg{ Color(100, 255, 255, 255), 3.0f };
    Pen accentTrack{ Color(255, 255, 61, 0), 3.0f };
    Pen transport{ Color(220, 255, 255, 255), 1.5f };
    Pen playCircle{ Color(255, 255, 255, 255), 2.0f };
    Pen speakerWave{ Color(200, 255, 255, 255), 1.2f };
    Pen mute{ Color(255, 255, 61, 0), 1.5f };
    Pen volTrackBg{ Color(70, 255, 255, 255), 3.0f };

    GraphicsPath artPath;
    GraphicsPath cardPath;
    std::unique_ptr<LinearGradientBrush> cardFill;
    int cardW = 0;
    int cardH = 0;

    HDC memDC = nullptr;
    HBITMAP dib = nullptr;
    HGDIOBJ oldBmp = nullptr;
    int surfW = 0;
    int surfH = 0;

    bool hasKey = false;
    RenderKey lastKey;

    RenderCache() {
        center.SetAlignment(StringAlignmentCenter);
        center.SetLineAlignment(StringAlignmentCenter);
        left.SetTrimming(StringTrimmingEllipsisCharacter);
        left.SetFormatFlags(StringFormatFlagsNoWrap);
        AddRoundRect(artPath, RectF(20.0f, 20.0f, (REAL)kArtBigSize, (REAL)kArtBigSize), 8.0f);
    }

    ~RenderCache() { ReleaseSurface(); }

    bool EnsureSurface(int w, int h, int capW, int capH) {
        if (memDC && w <= surfW && h <= surfH) return true;
        ReleaseSurface();

        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = capW;
        bmi.bmiHeader.biHeight = -capH; // Top-down DIB
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        HDC dc = CreateCompatibleDC(NULL);
        if (!dc) return false;
        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (!bmp) {
            DeleteDC(dc);
            return false;
        }
        oldBmp = SelectObject(dc, bmp);
        memDC = dc;
        dib = bmp;
        surfW = capW;
        surfH = capH;
        hasKey = false;
        return true;
    }

    // Only called while no Graphics object is bound to memDC.
    void ReleaseSurface() {
        if (memDC) {
            SelectObject(memDC, oldBmp);
            DeleteDC(memDC);
        }
        if (dib) DeleteObject(dib);
        memDC = nullptr;
        dib = nullptr;
        oldBmp = nullptr;
        surfW = 0;
        surfH = 0;
    }
};

MiniplayerWindow& MiniplayerWindow::Instance() {
    static MiniplayerWindow instance;
    return instance;
}

MiniplayerWindow::MiniplayerWindow() = default;

MiniplayerWindow::~MiniplayerWindow() {
    Shutdown();
}

void MiniplayerWindow::Shutdown() {
    // The WM_CAPTURECHANGED that DestroyWindow can send must not seek or render.
    m_isVisible = false;
    m_isDraggingVolume = false;
    m_isDraggingSeek = false;
    if (m_hWnd) {
        DestroyWindow(m_hWnd);
        m_hWnd = nullptr;
        g_hMiniplayerWnd = nullptr;
    }
    ClearArt();
    m_cache.reset();
    // GDI+ is intentionally never shut down: a detached artwork worker can still be decoding
    // during teardown, and process exit reclaims it anyway.
}

void MiniplayerWindow::SetDpi(UINT dpi) {
    if (!dpi) dpi = 96;
    if (dpi == m_dpi) return;
    m_dpi = dpi;
    // The surface is kept (it can be mid-UpdateLayeredWindow); EnsureSurface grows it if needed.
    if (m_cache) m_cache->hasKey = false;
    // Artwork is pre-scaled to device pixels; fetch it again at the new size (the old one is shown meanwhile).
    if (!m_wantedArtUrl.empty()) ++s_artGen;
}

void MiniplayerWindow::CalculateBounds() {
    RECT workArea;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);

    // Both states are anchored to the same bottom-right corner.
    const int right = workArea.right - Px(kMarginRight);
    const int bottom = workArea.bottom - Px(kOffsetBottom);

    m_idleRect.left = right - Px(kIdleW);
    m_idleRect.top = bottom - Px(kIdleH);
    m_idleRect.right = right;
    m_idleRect.bottom = bottom;

    m_expandedRect.left = right - Px(kExpandedW);
    m_expandedRect.top = bottom - Px(kExpandedH);
    m_expandedRect.right = right;
    m_expandedRect.bottom = bottom;
}

bool MiniplayerWindow::Create() {
    if (m_hWnd) return true;

    m_dpi = QueryDpi(NULL);
    CalculateBounds();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"YTRMusicMiniplayerClass";
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    RegisterClassExW(&wc);

    m_currentWidth = kIdleW;
    m_currentHeight = kIdleH;

    m_hWnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"YTRMusicMiniplayerClass",
        L"ytr-music Miniplayer",
        WS_POPUP,
        m_idleRect.left, m_idleRect.top,
        Px(m_currentWidth), Px(m_currentHeight),
        NULL, NULL, GetModuleHandleW(NULL), this
    );

    if (!m_hWnd) return false;

    g_hMiniplayerWnd = m_hWnd;

    // No timers here: they only run while the miniplayer is visible and something is animating.
    SetDpi(QueryDpi(m_hWnd));
    CalculateBounds();
    return true;
}

void MiniplayerWindow::Show() {
    if (!m_hWnd && !Create()) return;
    EnsureGdiplus();
    m_isVisible = true;
    SetDpi(QueryDpi(m_hWnd));
    CalculateBounds();
    // Content first, then show, so the previous frame never flashes.
    Render(true);
    SetWindowPos(m_hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    UpdateTimers();
    MaybeStartArtFetch();
}

void MiniplayerWindow::Hide() {
    m_isVisible = false;
    if (!m_hWnd) return;
    if (m_isDraggingVolume || m_isDraggingSeek) EndDrag(false);
    // Start collapsed next time; nothing animates while hidden.
    m_isHovered = false;
    m_currentWidth = kIdleW;
    m_currentHeight = kIdleH;
    UpdateTimers();
    ShowWindow(m_hWnd, SW_HIDE);
    if (m_cache) {
        m_cache->ReleaseSurface();
        m_cache->hasKey = false;
    }
}

void MiniplayerWindow::Toggle() {
    if (m_isVisible) Hide();
    else Show();
}

bool MiniplayerWindow::IsVisible() const {
    return m_isVisible;
}

void MiniplayerWindow::UpdateTimers() {
    if (!m_hWnd) return;
    const int targetW = m_isHovered ? kExpandedW : kIdleW;
    const int targetH = m_isHovered ? kExpandedH : kIdleH;
    const bool wantAnim = m_isVisible && (m_currentWidth != targetW || m_currentHeight != targetH);
    const bool wantEq = m_isVisible && !m_isHovered && !m_clockPaused;
    const bool wantProgress = m_isVisible && m_isHovered && !m_clockPaused;

    // Arm only on a change: SetTimer on a live id restarts its countdown.
    if (wantAnim != m_animTimerOn) {
        if (wantAnim) {
            CalculateBounds();
            SetWindowPos(m_hWnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            m_animTimerOn = SetTimer(m_hWnd, TIMER_ANIM, 16, NULL) != 0;
        } else {
            KillTimer(m_hWnd, TIMER_ANIM);
            m_animTimerOn = false;
        }
    }
    if (wantEq != m_eqTimerOn) {
        if (wantEq) {
            m_eqTimerOn = SetCoalescableTimer(m_hWnd, TIMER_EQ, 120, NULL, 30) != 0;
        } else {
            KillTimer(m_hWnd, TIMER_EQ);
            m_eqTimerOn = false;
        }
    }
    if (wantProgress != m_progressTimerOn) {
        if (wantProgress) {
            m_progressTimerOn = SetCoalescableTimer(m_hWnd, TIMER_PROGRESS, 500, NULL, 100) != 0;
        } else {
            KillTimer(m_hWnd, TIMER_PROGRESS);
            m_progressTimerOn = false;
        }
    }
}

double MiniplayerWindow::DisplayTime(ULONGLONG now) const {
    double t = m_clockTime;
    if (!m_clockPaused && now > m_clockTick) t += (double)(now - m_clockTick) / 1000.0;
    const double d = g_currentSong.duration;
    if (d > 0 && t > d) t = d;
    if (t < 0) t = 0;
    return t;
}

double MiniplayerWindow::CurrentProgress() const {
    if (m_isDraggingSeek) return m_seekDragPct;
    const double d = g_currentSong.duration;
    if (d <= 0) return 0.0;
    double progress = DisplayTime(GetTickCount64()) / d;
    if (progress > 1.0) progress = 1.0;
    return progress;
}

void MiniplayerWindow::SyncClockPaused(bool paused) {
    if (paused == m_clockPaused) return;
    const ULONGLONG now = GetTickCount64();
    m_clockTime = DisplayTime(now);
    m_clockTick = now;
    m_clockPaused = paused;
    UpdateTimers();
}

void MiniplayerWindow::ClearArt() {
    if (!m_artBig && !m_artSmall) return;
    FreeBitmap(m_artBig);
    FreeBitmap(m_artSmall);
    m_artPx = 0;
    m_artUrl.clear();
    ++m_artVersion;
}

void MiniplayerWindow::MaybeStartArtFetch() {
    if (!m_hWnd || !m_isVisible || m_wantedArtUrl.empty() || !g_gdiplusToken) return;
    const int bigPx = Px(kArtBigSize);
    if (m_artBig && m_artPx == bigPx && m_artUrl == m_wantedArtUrl) return;
    const unsigned gen = s_artGen.load();
    const ULONGLONG now = GetTickCount64();
    // Same track: only retry a failed download, and at most every 10 s.
    if (gen == m_fetchGen && (m_fetchBusy || !m_fetchFailedAt || now - m_fetchFailedAt < 10000)) return;
    // One download at a time (the next one starts when it reports back); a stalled one is given up on.
    if (m_fetchBusy && now - m_fetchTick < 15000) return;
    try {
        std::thread(ArtWorker, m_hWnd, gen, m_wantedArtUrl, bigPx, Px(kArtSmallSize)).detach();
    } catch (...) {
        return;
    }
    m_fetchGen = gen;
    m_fetchBusy = true;
    m_fetchTick = now;
    m_fetchFailedAt = 0;
}

void MiniplayerWindow::OnArtwork(unsigned gen, LPARAM lParam) {
    ArtResult* result = reinterpret_cast<ArtResult*>(lParam);
    if (gen == m_fetchGen) {
        m_fetchBusy = false;
        if (!result && gen == s_artGen.load()) m_fetchFailedAt = GetTickCount64();
    }
    if (result) {
        if (gen == s_artGen.load()) {
            FreeBitmap(m_artBig);
            FreeBitmap(m_artSmall);
            m_artBig = result->bigBmp;
            m_artSmall = result->smallBmp;
            m_artPx = result->px;
            m_artUrl = m_wantedArtUrl;
            ++m_artVersion;
        } else {
            FreeBitmap(result->bigBmp);
            FreeBitmap(result->smallBmp);
        }
        delete result;
    }
    MaybeStartArtFetch();
    Render();
}

void MiniplayerWindow::UpdateSongState(const SongInfo& song) {
    if (song.artworkUrl != m_wantedArtUrl) {
        m_wantedArtUrl = song.artworkUrl;
        ++s_artGen;
        // Show the placeholder rather than the previous song's art until the new one arrives.
        ClearArt();
    }
    MaybeStartArtFetch();

    // `song` is g_currentSong, already overwritten, so pause flips are detected against m_clockPaused.
    const ULONGLONG now = GetTickCount64();
    bool acceptTime = true;
    if (m_seekHoldUntil) {
        // Right after a seek the page can still report the old position; keep the knob where it was dropped.
        if (now >= m_seekHoldUntil || song.duration != m_seekHoldDuration ||
            std::fabs(song.currentTime - DisplayTime(now)) <= 2.0) {
            m_seekHoldUntil = 0;
        } else {
            acceptTime = false;
        }
    }
    if (acceptTime) {
        m_clockTime = song.currentTime;
        m_clockTick = now;
    }
    SyncClockPaused(song.isPaused);

    if (m_isVisible) {
        Render();
    }
}

void MiniplayerWindow::OnTimer(UINT_PTR id) {
    if (!m_isVisible) return;
    if (id == TIMER_ANIM) {
        // Smooth interpolation between idle and expanded
        StepToward(m_currentWidth, m_isHovered ? kExpandedW : kIdleW);
        StepToward(m_currentHeight, m_isHovered ? kExpandedH : kIdleH);
        Render();
        UpdateTimers();
        // Settled in the idle dock: give back the expanded-size surface.
        if (!m_animTimerOn && !m_isHovered && m_cache &&
            (m_cache->surfW > Px(kIdleW) || m_cache->surfH > Px(kIdleH))) {
            m_cache->ReleaseSurface();
        }
    } else if (id == TIMER_EQ) {
        m_eqStep = (m_eqStep + 1) % 12;
        Render();
    } else if (id == TIMER_PROGRESS) {
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
            // Button released without a WM_LBUTTONUP reaching us; EndDrag also settles the hover state.
            if ((self->m_isDraggingVolume || self->m_isDraggingSeek) && !(wParam & MK_LBUTTON)) {
                self->EndDrag(true);
                return 0;
            }
            TRACKMOUSEEVENT tme = { sizeof(TRACKMOUSEEVENT), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
            const float s = self->Scale();
            self->HandleMouseMove(GET_X_LPARAM(lParam) / s, GET_Y_LPARAM(lParam) / s);
            return 0;
        }
        case WM_MOUSELEAVE: {
            self->HandleMouseLeave();
            return 0;
        }
        case WM_LBUTTONDOWN: {
            const float s = self->Scale();
            self->HandleLButtonDown(GET_X_LPARAM(lParam) / s, GET_Y_LPARAM(lParam) / s);
            return 0;
        }
        case WM_LBUTTONUP: {
            if (self->m_isDraggingVolume || self->m_isDraggingSeek) {
                self->EndDrag(true);
            }
            return 0;
        }
        case WM_CAPTURECHANGED: {
            // EndDrag clears the flags before releasing capture, so this only fires on an outside capture loss.
            if ((HWND)lParam != hWnd && (self->m_isDraggingVolume || self->m_isDraggingSeek)) {
                self->EndDrag(true);
            }
            return 0;
        }
        case WM_CANCELMODE: {
            if (self->m_isDraggingVolume || self->m_isDraggingSeek) {
                self->EndDrag(false);
            }
            break;
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
            self->OnTimer(wParam);
            return 0;
        }
        case WM_MP_ARTWORK: {
            self->OnArtwork((unsigned)wParam, lParam);
            return 0;
        }
        case WM_DPICHANGED: {
            // The miniplayer stays anchored to the work-area corner, so the suggested rect is not used.
            // This can arrive from inside UpdateLayeredWindow (it moves the window), so redraw later.
            self->SetDpi(LOWORD(wParam));
            self->CalculateBounds();
            PostMessageW(hWnd, WM_MP_REFRESH, 0, 0);
            return 0;
        }
        case WM_MP_REFRESH: {
            self->Render(true);
            self->MaybeStartArtFetch();
            return 0;
        }
        case WM_DISPLAYCHANGE:
        case WM_SETTINGCHANGE: {
            if (self->m_isVisible && (msg == WM_DISPLAYCHANGE || wParam == SPI_SETWORKAREA)) {
                self->SetDpi(QueryDpi(hWnd));
                self->CalculateBounds();
                // Rare, so a forced frame is free insurance against a stale layered frame after a mode change.
                self->Render(true);
                self->MaybeStartArtFetch();
            }
            break;
        }
        case WM_CLOSE: {
            // Alt+F4 on an activated miniplayer must only hide it; a destroyed window could not be shown again.
            self->Hide();
            return 0;
        }
        case WM_NCDESTROY: {
            // Any destroy path leaves the object ready for Show() to create a fresh window.
            self->m_hWnd = nullptr;
            g_hMiniplayerWnd = nullptr;
            self->m_isVisible = false;
            self->m_isDraggingVolume = false;
            self->m_isDraggingSeek = false;
            break;
        }
        case WM_DESTROY: {
            KillTimer(hWnd, TIMER_ANIM);
            KillTimer(hWnd, TIMER_EQ);
            KillTimer(hWnd, TIMER_PROGRESS);
            self->m_animTimerOn = false;
            self->m_eqTimerOn = false;
            self->m_progressTimerOn = false;
            return 0;
        }
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

void MiniplayerWindow::OnHoverChanged() {
    UpdateTimers();
    Render();
}

void MiniplayerWindow::HandleMouseMove(float x, float y) {
    (void)y;
    if (!m_isHovered) {
        m_isHovered = true;
        OnHoverChanged();
    }
    if (m_isDraggingVolume) {
        int vol = VolumeAt(x);
        if (vol > 0) m_lastNonZeroVolume = vol;
        if (vol != m_dragVolume) {
            m_dragVolume = vol;
            g_currentSong.volume = vol;
            App_SetVolume(vol);
            Render();
        }
    } else if (m_isDraggingSeek) {
        // Preview only; the seek is sent once, when the drag ends.
        double pct = SeekPctAt(x);
        if (pct != m_seekDragPct) {
            m_seekDragPct = pct;
            Render();
        }
    }
}

void MiniplayerWindow::HandleMouseLeave() {
    // While dragging, capture keeps the drag alive outside the window; EndDrag re-checks the cursor.
    if (m_isDraggingVolume || m_isDraggingSeek) return;
    if (m_isHovered) {
        m_isHovered = false;
        OnHoverChanged();
    }
}

void MiniplayerWindow::EndDrag(bool commitSeek) {
    const bool wasSeek = m_isDraggingSeek;
    // Cleared before ReleaseCapture, which sends WM_CAPTURECHANGED synchronously.
    m_isDraggingSeek = false;
    m_isDraggingVolume = false;

    if (wasSeek && commitSeek && g_currentSong.duration > 0) {
        const double t = m_seekDragPct * g_currentSong.duration;
        App_SeekTo(t);
        const ULONGLONG now = GetTickCount64();
        m_clockTime = t;
        m_clockTick = now;
        m_seekHoldUntil = now + 1500;
        m_seekHoldDuration = g_currentSong.duration;
    }

    if (GetCapture() == m_hWnd) ReleaseCapture();

    if (m_isVisible && m_isHovered) {
        POINT pt;
        RECT rc;
        // GetCursorPos fails on the secure desktop (Win+L); collapse then, or nothing would ever collapse it.
        if (GetCursorPos(&pt) && GetWindowRect(m_hWnd, &rc) && PtInRect(&rc, pt)) {
            TRACKMOUSEEVENT tme = { sizeof(TRACKMOUSEEVENT), TME_LEAVE, m_hWnd, 0 };
            TrackMouseEvent(&tme);
        } else {
            m_isHovered = false;
            OnHoverChanged();
        }
    }
    Render();
}

void MiniplayerWindow::HandleLButtonDown(float x, float y) {
    if (m_currentWidth < 220) {
        // In idle dock view or while expanding: clicking anywhere resumes/pauses!
        App_SendControl(L"playPause");
        g_currentSong.isPaused = !g_currentSong.isPaused;
        SyncClockPaused(g_currentSong.isPaused);
        Render();
        return;
    }

    // In expanded view:
    // 0. Volume Control, Y: [155..189]: speaker at 14..33, slider track at kVolX1..kVolX2 plus knob and margin.
    //    The rest of the band (the % readout) is inert so a click there cannot hit Prev or max the volume.
    if (y >= 155 && y <= 189 && x >= 14 && x <= 112) {
        if (x <= 33) {
            // Clicked speaker icon -> toggle mute
            if (g_currentSong.volume > 0) {
                m_lastNonZeroVolume = (g_currentSong.volume > 5) ? g_currentSong.volume : 50;
                g_currentSong.volume = 0;
            } else {
                g_currentSong.volume = (m_lastNonZeroVolume > 5) ? m_lastNonZeroVolume : 50;
            }
            App_SetVolume(g_currentSong.volume);
            Render();
        } else if (x <= kVolX2 + 6.0f) {
            // Clicked volume slider track
            int vol = VolumeAt(x);
            if (vol > 0) m_lastNonZeroVolume = vol;
            g_currentSong.volume = vol;
            m_dragVolume = vol;
            App_SetVolume(vol);
            m_isDraggingVolume = true;
            SetCapture(m_hWnd);
            Render();
        }
        return;
    }

    // 1. Play/Pause (Resume) button: center at (170, 172)
    // Generous hit box: radius 32px or rect [135..205, 138..206]
    const float centerY = kCtrlY;
    float dx = x - 170.0f;
    float dy = y - centerY;
    if (dx * dx + dy * dy <= 1024.0f || (x >= 135 && x <= 205 && y >= 138 && y <= 206)) {
        App_SendControl(L"playPause");
        g_currentSong.isPaused = !g_currentSong.isPaused;
        SyncClockPaused(g_currentSong.isPaused);
        Render();
        return;
    }

    // 2. Prev button: center at (120, 172), generous radius 24px
    dx = x - 120.0f;
    if (dx * dx + dy * dy <= 576.0f || (x >= 95 && x <= 140 && y >= 148 && y <= 196)) {
        App_SendControl(L"previous");
        return;
    }

    // 3. Next button: center at (220, 172), generous radius 24px
    dx = x - 220.0f;
    if (dx * dx + dy * dy <= 576.0f || (x >= 200 && x <= 245 && y >= 148 && y <= 196)) {
        App_SendControl(L"next");
        return;
    }

    // 4. Like button: center at (265, 172), generous radius 24px
    dx = x - 265.0f;
    if (dx * dx + dy * dy <= 576.0f || (x >= 245 && x <= 290 && y >= 148 && y <= 196)) {
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

    // 6. Progress Bar Scrubber: track is drawn at y 116, x from 20 to 320
    if (y >= 104 && y <= 130 && x >= 15 && x <= 325) {
        if (g_currentSong.duration > 0) {
            m_seekDragPct = SeekPctAt(x);
            m_isDraggingSeek = true;
            SetCapture(m_hWnd);
            Render();
        }
        return;
    }
}

void MiniplayerWindow::HandleMouseWheel(short delta) {
    // Touchpads and hi-res wheels send many small deltas; step 5% per full notch only.
    if ((delta > 0) != (m_wheelAccum > 0)) m_wheelAccum = 0;
    m_wheelAccum += delta;
    const int notches = m_wheelAccum / WHEEL_DELTA;
    if (notches == 0) return;
    m_wheelAccum -= notches * WHEEL_DELTA;
    int newVol = g_currentSong.volume + 5 * notches;
    if (newVol < 0) newVol = 0;
    if (newVol > 100) newVol = 100;
    if (newVol > 0) m_lastNonZeroVolume = newVol;
    g_currentSong.volume = newVol;
    m_dragVolume = newVol;
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

void MiniplayerWindow::Render(bool force) {
    if (!m_hWnd || !m_isVisible || !g_gdiplusToken) return;
    if (!m_cache) {
        m_cache.reset(new (std::nothrow) RenderCache());
        if (!m_cache) return;
    }
    RenderCache& c = *m_cache;

    const int width = Px(m_currentWidth);
    const int height = Px(m_currentHeight);
    if (width <= 0 || height <= 0) return;
    const float s = Scale();

    RenderKey key;
    key.x = m_expandedRect.right - width;
    key.y = m_expandedRect.bottom - height;
    key.w = width;
    key.h = height;
    key.expanded = m_isHovered;
    key.paused = g_currentSong.isPaused;
    key.artVersion = m_artVersion;
    double progress = 0.0;
    if (m_isHovered) {
        progress = CurrentProgress();
        key.progressPx = (int)(progress * kSeekW * s + 0.5);
        key.volume = m_isDraggingVolume ? m_dragVolume : g_currentSong.volume;
        key.liked = g_currentSong.isLiked;
        key.title = g_currentSong.title;
        key.artist = g_currentSong.artist;
    } else {
        key.eqStep = m_eqStep;
    }
    if (!force && c.hasKey && key == c.lastKey) return;

    // While expanding, size the surface for the full player so the animation reuses one allocation.
    int capW = width;
    int capH = height;
    if (m_isHovered) {
        if (capW < Px(kExpandedW)) capW = Px(kExpandedW);
        if (capH < Px(kExpandedH)) capH = Px(kExpandedH);
    }
    if (!c.EnsureSurface(width, height, capW, capH)) return;

    // The Graphics object must be gone (and flushed) before the DC is read by UpdateLayeredWindow.
    {
        Graphics g(c.memDC);
        g.SetClip(Rect(0, 0, width, height));
        // Clear transparent
        g.Clear(Color(0, 0, 0, 0));
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        // ClearType needs an opaque background; on a per-pixel-alpha surface it gives colour fringes.
        g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
        g.ScaleTransform(s, s);

        // 10% translucent glassmorphic card, accent #ff3d00
        if (!c.cardFill || c.cardW != m_currentWidth || c.cardH != m_currentHeight) {
            RectF cardRect(1.0f, 1.0f, (float)(m_currentWidth - 2), (float)(m_currentHeight - 2));
            c.cardPath.Reset();
            AddRoundRect(c.cardPath, cardRect, 14.0f);
            c.cardFill.reset(new LinearGradientBrush(cardRect, Color(35, 255, 61, 0), Color(24, 15, 18, 26), LinearGradientModeForwardDiagonal));
            c.cardW = m_currentWidth;
            c.cardH = m_currentHeight;
        }
        if (c.cardFill) g.FillPath(c.cardFill.get(), &c.cardPath);
        // Subtle luminous glass border
        g.DrawPath(&c.cardBorder, &c.cardPath);

        if (!m_isHovered) {
            // ==========================================
            // IDLE VIEW: Vertical Top-to-Bottom Dock
            // ==========================================
            // Album Art circle at top
            RectF artRect(6.0f, 10.0f, (REAL)kArtSmallSize, (REAL)kArtSmallSize);
            if (m_artSmall) {
                DrawArtBitmap(g, m_artSmall, artRect, Px(kArtSmallSize), s);
            } else {
                g.FillEllipse(&c.artBgIdle, artRect);
                // Music Note icon in center of art
                g.DrawString(L"\u266A", -1, &c.fontNoteSmall, artRect, &c.center, &c.noteIdle);
            }
            g.DrawEllipse(&c.artBorderIdle, artRect);

            // Outlined Play/Pause icon in middle (Y around 70)
            float cx = 22.0f;
            float cy = 76.0f;
            g.DrawEllipse(&c.ctrlOutlineIdle, cx - 14.0f, cy - 14.0f, 28.0f, 28.0f);

            if (g_currentSong.isPaused) {
                // Play triangle
                PointF pts[3] = { { cx - 3.0f, cy - 6.0f }, { cx + 6.0f, cy }, { cx - 3.0f, cy + 6.0f } };
                g.FillPolygon(&c.white, pts, 3);
            } else {
                // Pause 2 bars
                g.FillRectangle(&c.white, cx - 5.0f, cy - 6.0f, 3.0f, 12.0f);
                g.FillRectangle(&c.white, cx + 2.0f, cy - 6.0f, 3.0f, 12.0f);
            }

            // Animated 3-Bar Audio Equalizer at bottom (Y around 125 to 145)
            float eqHeights[3] = { 6.0f, 14.0f, 9.0f };
            if (!g_currentSong.isPaused) {
                int step = m_eqStep;
                eqHeights[0] = 5.0f + 10.0f * (float)std::fabs(std::sin(step * 0.5));
                eqHeights[1] = 7.0f + 12.0f * (float)std::fabs(std::cos(step * 0.6));
                eqHeights[2] = 4.0f + 11.0f * (float)std::fabs(std::sin(step * 0.4 + 1.0));
            }

            float barX = 13.0f;
            for (int i = 0; i < 3; ++i) {
                float h = eqHeights[i];
                g.FillRectangle(&c.eqBar, barX, 142.0f - h, 4.0f, h);
                barX += 6.0f;
            }
        } else {
            // ==========================================
            // EXPANDED VIEW: Complete High-Performance Player
            // ==========================================
            // Album art on the left (20, 20, 68x68)
            RectF artRect(20.0f, 20.0f, (REAL)kArtBigSize, (REAL)kArtBigSize);
            if (m_artBig) {
                DrawArtBitmap(g, m_artBig, artRect, Px(kArtBigSize), s);
            } else {
                g.FillPath(&c.artBgExpanded, &c.artPath);
                g.DrawString(L"\u266A", -1, &c.fontNoteBig, artRect, &c.center, &c.white);
            }
            g.DrawPath(&c.artBorderExpanded, &c.artPath);

            // Title and Artist texts
            RectF titleRect(100.0f, 24.0f, 185.0f, 24.0f);
            RectF artistRect(100.0f, 50.0f, 185.0f, 20.0f);
            g.DrawString(g_currentSong.title.c_str(), -1, &c.fontTitle, titleRect, &c.left, &c.white);
            g.DrawString(g_currentSong.artist.c_str(), -1, &c.fontArtist, artistRect, &c.left, &c.artistText);

            // Restore Window / PiP Button (Top Right)
            RectF restoreRect(298.0f, 16.0f, 26.0f, 26.0f);
            g.DrawRectangle(&c.restore, restoreRect.X + 4, restoreRect.Y + 4, 16.0f, 16.0f);
            g.DrawRectangle(&c.restore, restoreRect.X + 8, restoreRect.Y + 8, 8.0f, 8.0f);

            // Track Progress Slider (X: 20 to 320, Y: 116)
            float trackY = 116.0f;
            g.DrawLine(&c.trackBg, kSeekX1, trackY, kSeekX1 + kSeekW, trackY);

            float filledX = kSeekX1 + (float)(progress * kSeekW);
            g.DrawLine(&c.accentTrack, kSeekX1, trackY, filledX, trackY);

            // Knob handle
            g.FillEllipse(&c.white, filledX - 5.0f, trackY - 5.0f, 10.0f, 10.0f);

            // Control Buttons row:
            // Previous (120, 172)
            float prevX = 120.0f;
            float ctrlY = kCtrlY;
            PointF prevPts[3] = { { prevX + 4.0f, ctrlY - 7.0f }, { prevX - 6.0f, ctrlY }, { prevX + 4.0f, ctrlY + 7.0f } };
            g.DrawPolygon(&c.transport, prevPts, 3);
            g.DrawLine(&c.transport, prevX - 6.0f, ctrlY - 7.0f, prevX - 6.0f, ctrlY + 7.0f);

            // Outlined Play/Pause Center Button (170, 172)
            float playX = 170.0f;
            g.DrawEllipse(&c.playCircle, playX - 20.0f, ctrlY - 20.0f, 40.0f, 40.0f);

            if (g_currentSong.isPaused) {
                PointF playPts[3] = { { playX - 4.0f, ctrlY - 9.0f }, { playX + 8.0f, ctrlY }, { playX - 4.0f, ctrlY + 9.0f } };
                g.FillPolygon(&c.white, playPts, 3);
            } else {
                g.FillRectangle(&c.white, playX - 7.0f, ctrlY - 8.0f, 4.0f, 16.0f);
                g.FillRectangle(&c.white, playX + 3.0f, ctrlY - 8.0f, 4.0f, 16.0f);
            }

            // Next (220, 172)
            float nextX = 220.0f;
            PointF nextPts[3] = { { nextX - 4.0f, ctrlY - 7.0f }, { nextX + 6.0f, ctrlY }, { nextX - 4.0f, ctrlY + 7.0f } };
            g.DrawPolygon(&c.transport, nextPts, 3);
            g.DrawLine(&c.transport, nextX + 6.0f, ctrlY - 7.0f, nextX + 6.0f, ctrlY + 7.0f);

            // Like Heart (265, 172)
            RectF heartRect(253.0f, ctrlY - 12.0f, 24.0f, 24.0f);
            if (g_currentSong.isLiked) {
                g.DrawString(L"\u2665", -1, &c.fontHeart, heartRect, &c.center, &c.heartOn);
            } else {
                g.DrawString(L"\u2661", -1, &c.fontHeart, heartRect, &c.center, &c.heartOff);
            }

            // Volume Control on bottom left (ctrlY = 172.0f)
            const int volume = key.volume;
            // 1. Crisp Vector Speaker Icon (X: 16 to 28, Y: ctrlY - 6 to ctrlY + 6)
            // Back rectangle
            g.FillRectangle(&c.speaker, 16.0f, ctrlY - 3.0f, 3.0f, 6.0f);
            // Cone polygon
            PointF conePts[4] = {
                { 19.0f, ctrlY - 3.0f },
                { 24.0f, ctrlY - 6.5f },
                { 24.0f, ctrlY + 6.5f },
                { 19.0f, ctrlY + 3.0f }
            };
            g.FillPolygon(&c.speaker, conePts, 4);

            if (volume > 0) {
                // Sound wave arc
                RectF waveRect(22.0f, ctrlY - 5.0f, 6.0f, 10.0f);
                g.DrawArc(&c.speakerWave, waveRect, -50, 100);
                if (volume > 50) {
                    RectF waveRect2(24.5f, ctrlY - 7.0f, 7.0f, 14.0f);
                    g.DrawArc(&c.speakerWave, waveRect2, -50, 100);
                }
            } else {
                // Muted "X"
                g.DrawLine(&c.mute, 26.0f, ctrlY - 4.0f, 30.0f, ctrlY + 4.0f);
                g.DrawLine(&c.mute, 30.0f, ctrlY - 4.0f, 26.0f, ctrlY + 4.0f);
            }

            // 2. Volume Slider Track (X: 36 to 84, total 48px width)
            float volTrackW = kVolX2 - kVolX1;
            g.DrawLine(&c.volTrackBg, kVolX1, ctrlY, kVolX2, ctrlY);

            float volPct = (float)volume / 100.0f;
            if (volPct < 0.0f) volPct = 0.0f;
            if (volPct > 1.0f) volPct = 1.0f;
            float volFilledX = kVolX1 + volPct * volTrackW;
            g.DrawLine(&c.accentTrack, kVolX1, ctrlY, volFilledX, ctrlY);

            // Volume Slider Knob
            g.FillEllipse(&c.white, volFilledX - 4.0f, ctrlY - 4.0f, 8.0f, 8.0f);

            // 3. Volume % Readout Text (X: 88 to 112)
            RectF volRect(88.0f, ctrlY - 7.0f, 28.0f, 14.0f);
            WCHAR volText[16];
            swprintf_s(volText, L"%d%%", volume);
            g.DrawString(volText, -1, &c.fontVolume, volRect, &c.left, &c.volumeText);
        }

        g.Flush(FlushIntentionSync);
    }

    // Blend to screen; UpdateLayeredWindow moves, resizes and repaints in one step.
    POINT ptDst = { key.x, key.y };
    POINT ptSrc = { 0, 0 };
    SIZE size = { width, height };
    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    if (UpdateLayeredWindow(m_hWnd, NULL, &ptDst, &size, c.memDC, &ptSrc, 0, &blend, ULW_ALPHA)) {
        c.lastKey = std::move(key);
        c.hasKey = true;
    }
}
