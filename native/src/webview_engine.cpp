#include "webview_engine.h"
#include "com_helper.h"
#include <shlobj.h>
#include <sstream>
#include <iostream>

typedef HRESULT(STDAPICALLTYPE* PFN_CreateCoreWebView2EnvironmentWithOptions)(
    PCWSTR browserExecutableFolder,
    PCWSTR userDataFolder,
    ICoreWebView2EnvironmentOptions* environmentOptions,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* environment_created_handler);

WebViewEngine& WebViewEngine::Instance() {
    static WebViewEngine instance;
    return instance;
}

WebViewEngine::WebViewEngine() {}

WebViewEngine::~WebViewEngine() {
    if (m_controller) {
        m_controller->Close();
        m_controller->Release();
        m_controller = nullptr;
    }
    if (m_webview) {
        m_webview->Release();
        m_webview = nullptr;
    }
    if (m_environment) {
        m_environment->Release();
        m_environment = nullptr;
    }
}

void LogBridge(const std::wstring& text) {
    WCHAR appDataPath[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appDataPath))) {
        std::wstring dirPath = std::wstring(appDataPath) + L"\\ytr-music-native";
        CreateDirectoryW(dirPath.c_str(), NULL);
        std::wstring logPath = dirPath + L"\\debug.log";
        FILE* fp = _wfopen(logPath.c_str(), L"a, ccs=UTF-8");
        if (fp) {
            SYSTEMTIME st;
            GetLocalTime(&st);
            fwprintf(fp, L"[%02d:%02d:%02d.%03d] %ls\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, text.c_str());
            fclose(fp);
        }
    }
}

bool WebViewEngine::Initialize(HWND hWndContainer, std::function<void()> onInitialized) {
    m_hWndContainer = hWndContainer;
    m_onInitialized = onInitialized;

    LogBridge(L"=== WebViewEngine::Initialize called ===");

    // Apply Chromium flags with remote debugging port for validation and remote allow origins
    SetEnvironmentVariableW(
        L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS",
        L"--remote-debugging-port=9222 --remote-allow-origins=* "
        L"--disable-features=CalculateNativeWinOcclusion,SpareRendererForSitePerProcess "
        L"--enable-gpu-rasterization --enable-zero-copy"
    );

    // Get User Data Folder: %APPDATA%\ytr-music-native
    WCHAR appDataPath[MAX_PATH];
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appDataPath))) {
        wcscpy_s(appDataPath, L".");
    }
    std::wstring userDataFolder = std::wstring(appDataPath) + L"\\ytr-music-native";
    CreateDirectoryW(userDataFolder.c_str(), NULL);

    // Load WebView2Loader.dll
    HMODULE hLoader = LoadLibraryW(L"WebView2Loader.dll");
    if (!hLoader) {
        // Try looking next to the module or in native/bin
        WCHAR modulePath[MAX_PATH];
        GetModuleFileNameW(NULL, modulePath, MAX_PATH);
        WCHAR* lastSlash = wcsrchr(modulePath, L'\\');
        if (lastSlash) {
            *lastSlash = L'\0';
            std::wstring loaderPath = std::wstring(modulePath) + L"\\WebView2Loader.dll";
            hLoader = LoadLibraryW(loaderPath.c_str());
        }
    }

    if (!hLoader) {
        // Fallback: extract embedded WebView2Loader from PE resource into user data folder
        std::wstring cachedLoader = userDataFolder + L"\\WebView2Loader.dll";
        hLoader = LoadLibraryW(cachedLoader.c_str());
        if (!hLoader) {
            HRSRC hRes = FindResourceW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(201), MAKEINTRESOURCEW(10));
            if (hRes) {
                HGLOBAL hData = LoadResource(GetModuleHandleW(NULL), hRes);
                DWORD size = SizeofResource(GetModuleHandleW(NULL), hRes);
                void* pData = LockResource(hData);
                if (pData && size > 0) {
                    HANDLE hFile = CreateFileW(cachedLoader.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                    if (hFile != INVALID_HANDLE_VALUE) {
                        DWORD written = 0;
                        WriteFile(hFile, pData, size, &written, NULL);
                        CloseHandle(hFile);
                        hLoader = LoadLibraryW(cachedLoader.c_str());
                    }
                }
            }
        }
    }

    if (!hLoader) {
        MessageBoxW(hWndContainer, L"Could not load WebView2Loader.dll!", L"ytr-music", MB_ICONERROR);
        return false;
    }

    auto pfnCreateEnv = (PFN_CreateCoreWebView2EnvironmentWithOptions)GetProcAddress(
        hLoader, "CreateCoreWebView2EnvironmentWithOptions");
    if (!pfnCreateEnv) {
        MessageBoxW(hWndContainer, L"WebView2 entry point not found!", L"ytr-music Native", MB_ICONERROR);
        return false;
    }

    auto envHandler = new CoreEnvCompletedHandler(
        [this](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
            if (FAILED(hr) || !env) {
                MessageBoxW(m_hWndContainer, L"Failed to create CoreWebView2Environment!", L"ytr-music Native", MB_ICONERROR);
                return hr;
            }
            m_environment = env;
            m_environment->AddRef();

            auto controllerHandler = new CoreControllerCompletedHandler(
                [this](HRESULT hr2, ICoreWebView2Controller* controller) -> HRESULT {
                    if (FAILED(hr2) || !controller) {
                        WCHAR errBuf[256];
                        swprintf_s(errBuf, L"CreateCoreWebView2Controller failed! hr=0x%08X", (UINT)hr2);
                        LogBridge(errBuf);
                        MessageBoxW(m_hWndContainer, L"Failed to create CoreWebView2Controller!", L"ytr-music Native", MB_ICONERROR);
                        return hr2;
                    }
                    m_controller = controller;
                    m_controller->AddRef();

                    m_controller->get_CoreWebView2(&m_webview);

                    // Configure controller settings
                    RECT bounds;
                    GetClientRect(m_hWndContainer, &bounds);
                    m_controller->put_Bounds(bounds);
                    m_controller->put_IsVisible(TRUE);

                    // Setup Settings
                    ICoreWebView2Settings* settings = nullptr;
                    if (SUCCEEDED(m_webview->get_Settings(&settings)) && settings) {
                        settings->put_IsScriptEnabled(TRUE);
                        settings->put_AreDefaultScriptDialogsEnabled(TRUE);
                        settings->put_IsWebMessageEnabled(TRUE);
                        settings->put_AreDevToolsEnabled(TRUE);
                        settings->put_AreDefaultContextMenusEnabled(TRUE);
                        settings->put_IsStatusBarEnabled(FALSE);

                        ICoreWebView2Settings4* settings4 = nullptr;
                        if (SUCCEEDED(settings->QueryInterface(IID_ICoreWebView2Settings4, (void**)&settings4)) && settings4) {
                            settings4->put_IsPasswordAutosaveEnabled(TRUE);
                            settings4->put_IsGeneralAutofillEnabled(TRUE);
                            settings4->Release();
                        }

                        settings->Release();
                    }

                    // Setup message listener
                    EventRegistrationToken msgToken;
                    auto msgHandler = new CoreWebMessageReceivedHandler(
                        [this](ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                            LPWSTR rawJson = nullptr;
                            if (SUCCEEDED(args->get_WebMessageAsJson(&rawJson)) && rawJson) {
                                HandleWebMessage(rawJson);
                                CoTaskMemFree(rawJson);
                            }
                            return S_OK;
                        }
                    );
                    m_webview->add_WebMessageReceived(msgHandler, &msgToken);

                    // Setup Injected Script Bridge
                    SetupInjectedBridge();

                    // Listen for navigation completed to ensure bridge is active
                    EventRegistrationToken navToken;
                    auto navHandler = new CoreNavigationCompletedHandler(
                        [this](ICoreWebView2* sender, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                            LogBridge(L"NavigationCompleted fired! Directly executing bridge script.");
                            if (!m_bridgeScript.empty()) {
                                ExecuteScript(m_bridgeScript);
                            }
                            return S_OK;
                        }
                    );
                    m_webview->add_NavigationCompleted(navHandler, &navToken);

                    // Setup Native Ad-Blocking Network Filters
                    m_webview->AddWebResourceRequestedFilter(L"*://*.doubleclick.net/*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
                    m_webview->AddWebResourceRequestedFilter(L"*://googleads.g.doubleclick.net/*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
                    m_webview->AddWebResourceRequestedFilter(L"*://pagead2.googlesyndication.com/*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
                    m_webview->AddWebResourceRequestedFilter(L"*://*.googlesyndication.com/*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
                    m_webview->AddWebResourceRequestedFilter(L"*://tpc.googlesyndication.com/*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
                    m_webview->AddWebResourceRequestedFilter(L"*://www.youtube.com/pagead/*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
                    m_webview->AddWebResourceRequestedFilter(L"*://music.youtube.com/pagead/*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
                    m_webview->AddWebResourceRequestedFilter(L"*://music.youtube.com/youtubei/v1/player/ad_break*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
                    m_webview->AddWebResourceRequestedFilter(L"*://www.youtube.com/youtubei/v1/player/ad_break*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
                    m_webview->AddWebResourceRequestedFilter(L"*://music.youtube.com/api/stats/ads*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
                    m_webview->AddWebResourceRequestedFilter(L"*://www.youtube.com/api/stats/ads*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
                    m_webview->AddWebResourceRequestedFilter(L"*://adservice.google.*/*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);

                    EventRegistrationToken resToken;
                    auto resHandler = new CoreWebResourceRequestedHandler(
                        [this](ICoreWebView2* sender, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                            ICoreWebView2WebResourceResponse* response = nullptr;
                            m_environment->CreateWebResourceResponse(
                                nullptr, 204, L"No Content",
                                L"Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET, POST, OPTIONS, HEAD\r\nAccess-Control-Allow-Headers: *\r\n",
                                &response
                            );
                            if (response) {
                                args->put_Response(response);
                                response->Release();
                            }
                            return S_OK;
                        }
                    );
                    m_webview->add_WebResourceRequested(resHandler, &resToken);

                    // Navigate to YouTube Music
                    m_webview->Navigate(L"https://music.youtube.com");

                    m_isReady = true;
                    if (m_onInitialized) {
                        m_onInitialized();
                    }
                    return S_OK;
                }
            );

            m_environment->CreateCoreWebView2Controller(m_hWndContainer, controllerHandler);
            return S_OK;
        }
    );

    HRESULT hr = pfnCreateEnv(nullptr, userDataFolder.c_str(), nullptr, envHandler);
    return SUCCEEDED(hr);
}

void WebViewEngine::SetupInjectedBridge() {
    if (!m_webview) return;

    // Comprehensive Bulletproof In-App TopBar UI, Ad-Blocker, Fast-Forwarder, Muter, and State Bridge
    std::wstring script = LR"JS(
(function() {
    console.log('[ytr-native] Injected bridge loading...');

    // 0. High-Performance Innertube / Player Ad Stripper (Intercepts API responses before YouTube parses them)
    function sanitizeAdObject(obj, visited = new WeakSet()) {
        if (!obj || typeof obj !== 'object' || visited.has(obj)) return;
        visited.add(obj);
        delete obj.playerAds;
        delete obj.adPlacements;
        delete obj.adSlots;
        delete obj.adBreakHeartbeatParams;
        delete obj.adBreakHeartbeatRenderer;
        delete obj.adThrottled;
        if (obj.playerResponse && typeof obj.playerResponse === 'object') {
            sanitizeAdObject(obj.playerResponse, visited);
        }
    }

    // Intercept Response.prototype.json to sanitize all incoming /youtubei/v1/player & /next responses
    try {
        const origResponseJson = Response.prototype.json;
        Response.prototype.json = async function() {
            const data = await origResponseJson.apply(this, arguments);
            try {
                sanitizeAdObject(data);
            } catch (_) {}
            return data;
        };
    } catch (_) {}

    // Intercept JSON.parse for embedded responses and initial state
    try {
        const origJsonParse = JSON.parse;
        JSON.parse = function(text, reviver) {
            const data = origJsonParse.apply(this, arguments);
            try {
                sanitizeAdObject(data);
            } catch (_) {}
            return data;
        };
    } catch (_) {}

    try {
        // Trap window.ytInitialPlayerResponse
        let rawInitialPlayerResponse = window.ytInitialPlayerResponse;
        Object.defineProperty(window, 'ytInitialPlayerResponse', {
            configurable: true,
            enumerable: true,
            get() { return rawInitialPlayerResponse; },
            set(val) {
                if (val && typeof val === 'object') sanitizeAdObject(val);
                rawInitialPlayerResponse = val;
            }
        });

    } catch (_) {}

    // 1. Safe CSS Injection: Topbar + Layout Offset + Complete Ad/Promo Blocking
    function ensureStyles() {
        try {
            if (document.getElementById('ytr-adblock-styles')) return;
            const target = document.head || document.documentElement;
            if (!target) return;
            const style = document.createElement('style');
            style.id = 'ytr-adblock-styles';
            style.textContent = `
                /* Complete Ad & Promo Blocker Rules */
                .ytp-ad-overlay-container,
                .ytp-ad-message-container,
                .ytp-ad-action-interstitial,
                #player-ads,
                ytmusic-mealbar-promo-renderer,
                ytd-ad-slot-renderer,
                ytmusic-banner-promo-renderer,
                tp-yt-paper-dialog:has(ytmusic-mealbar-promo-renderer),
                ytmusic-popup-container:has(ytmusic-mealbar-promo-renderer),
                ytmusic-popup-container:has(ytmusic-upsell-dialog-renderer),
                ytmusic-guide-entry-renderer:has(a[href*="/upgrade"]),
                ytmusic-guide-entry-renderer:has(a[title*="Upgrade"]),
                ytmusic-guide-entry-renderer:has(a[aria-label*="Upgrade"]),
                ytmusic-guide-entry-renderer:has([title*="Upgrade" i]),
                ytmusic-guide-entry-renderer:has([aria-label*="Upgrade" i]),
                ytmusic-guide-entry-renderer:has(yt-formatted-string[title*="Upgrade" i]),
                ytmusic-guide-section-renderer:has(a[href*="/upgrade"]),
                a[href*="/music_premium"],
                a[href*="/upgrade"],
                #upgrade-button,
                .ytmusic-nav-bar #upgrade-button,
                ytmusic-pivot-bar-item-renderer:has(a[href*="/upgrade"]),
                ytmusic-upsell-dialog-renderer,
                .ytp-ad-preview-container,
                .ytp-ad-preview-text,
                .ytp-ad-text,
                .ytp-ad-player-overlay,
                .ytp-ad-player-overlay-layout,
                .ytp-ad-image-overlay,
                .ytp-ad-skip-button-container {
                    display: none !important;
                    visibility: hidden !important;
                    pointer-events: none !important;
                    height: 0 !important;
                    opacity: 0 !important;
                }

                /* Topbar Layout Offset: pushes YouTube Music down 36px so it never collides */
                :root {
                    --menu-bar-height: 36px !important;
                }

                body {
                    padding-top: 36px !important;
                    box-sizing: border-box !important;
                }

                ytmusic-app-layout {
                    overflow: auto scroll !important;
                    height: calc(100vh - 36px) !important;
                }

                ytmusic-app-layout > #content {
                    padding-top: 36px !important;
                }

                ytmusic-app-layout > [slot='nav-bar'],
                #nav-bar-background.ytmusic-app-layout,
                ytmusic-nav-bar {
                    top: 36px !important;
                }

                #nav-bar-divider.ytmusic-app-layout {
                    top: calc(var(--ytmusic-nav-bar-height, 64px) + 36px) !important;
                }

                ytmusic-app[is-bauhaus-sidenav-enabled] #guide-spacer.ytmusic-app,
                ytmusic-app[is-bauhaus-sidenav-enabled] #mini-guide-spacer.ytmusic-app {
                    margin-top: calc(var(--ytmusic-nav-bar-height, 64px) + 36px) !important;
                }

                ytmusic-app-layout > [slot='player-page'] {
                    margin-top: 36px !important;
                }

                ytmusic-guide-renderer {
                    height: calc(100vh - 36px - var(--ytmusic-nav-bar-height, 64px)) !important;
                }

                /* In-App Topbar Navigation & Actions Panel */
                #ytmd-title-bar-main-panel {
                    position: fixed !important;
                    top: 0 !important;
                    left: 0 !important;
                    right: 0 !important;
                    width: 100% !important;
                    height: 36px !important;
                    background: #08090c !important;
                    border-bottom: 1px solid rgba(255, 255, 255, 0.08) !important;
                    z-index: 2147483647 !important;
                    display: flex !important;
                    align-items: center !important;
                    justify-content: space-between !important;
                    padding: 0 12px !important;
                    box-sizing: border-box !important;
                    user-select: none !important;
                    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif !important;
                    box-shadow: 0 2px 10px rgba(0,0,0,0.5) !important;
                }

                .ytr-topbar-left {
                    display: flex;
                    align-items: center;
                    gap: 8px;
                    flex-shrink: 0;
                }

                .ytr-topbar-brand {
                    display: flex;
                    align-items: center;
                    gap: 6px;
                    margin-right: 6px;
                }

                .ytr-brand-title {
                    color: #ffffff;
                    font-size: 12px;
                    font-weight: 700;
                    letter-spacing: -0.2px;
                }

                .ytr-nav-btns {
                    display: flex;
                    align-items: center;
                    gap: 4px;
                }

                .ytr-nav-btn {
                    width: 26px;
                    height: 24px;
                    background: rgba(255, 255, 255, 0.06);
                    border: 1px solid rgba(255, 255, 255, 0.1);
                    border-radius: 4px;
                    color: #d1d1d1;
                    font-size: 11px;
                    display: inline-flex;
                    align-items: center;
                    justify-content: center;
                    cursor: pointer;
                    transition: all 0.15s ease;
                }

                .ytr-nav-btn:hover {
                    background: rgba(255, 255, 255, 0.16);
                    color: #ffffff;
                    border-color: rgba(255, 255, 255, 0.25);
                }

                .ytr-menu-items {
                    display: flex;
                    align-items: center;
                    gap: 2px;
                    margin-left: 4px;
                }

                .ytr-menu-dropdown {
                    position: relative;
                }

                .ytr-menu-label {
                    background: transparent;
                    border: none;
                    color: rgba(255, 255, 255, 0.7);
                    font-size: 11px;
                    font-weight: 500;
                    padding: 4px 8px;
                    border-radius: 4px;
                    cursor: pointer;
                    transition: all 0.15s ease;
                }

                .ytr-menu-label:hover, .ytr-menu-dropdown:hover .ytr-menu-label {
                    color: #ffffff;
                    background: rgba(255, 255, 255, 0.1);
                }

                .ytr-dropdown-content {
                    display: none;
                    position: absolute;
                    top: 100%;
                    left: 0;
                    min-width: 170px;
                    background: #12141a;
                    border: 1px solid rgba(255, 255, 255, 0.12);
                    border-radius: 6px;
                    box-shadow: 0 8px 24px rgba(0,0,0,0.7);
                    padding: 4px 0;
                    z-index: 2147483647;
                }

                .ytr-menu-dropdown:hover .ytr-dropdown-content {
                    display: block;
                }

                .ytr-menu-item {
                    padding: 6px 12px;
                    font-size: 11px;
                    color: #d1d5db;
                    cursor: pointer;
                    transition: all 0.1s ease;
                }

                .ytr-menu-item:hover {
                    background: #ff3d00;
                    color: #ffffff;
                }

                .ytr-divider {
                    height: 1px;
                    background: rgba(255, 255, 255, 0.08);
                    margin: 4px 0;
                }

                .ytr-topbar-center {
                    display: flex;
                    align-items: center;
                    justify-content: center;
                    flex: 1;
                    max-width: 450px;
                    overflow: hidden;
                    white-space: nowrap;
                    text-overflow: ellipsis;
                    margin: 0 10px;
                }

                #ytr-topbar-ticker {
                    color: rgba(255, 255, 255, 0.65);
                    font-size: 11px;
                    font-weight: 500;
                    overflow: hidden;
                    text-overflow: ellipsis;
                    white-space: nowrap;
                }

                .ytr-topbar-right {
                    display: flex;
                    align-items: center;
                    gap: 8px;
                    flex-shrink: 0;
                }

                .ytr-badge-link {
                    display: inline-flex;
                    align-items: center;
                    gap: 6px;
                    padding: 3px 10px;
                    background: rgba(255, 61, 0, 0.12);
                    border: 1px solid rgba(255, 61, 0, 0.40);
                    border-radius: 999px;
                    color: #ffffff;
                    font-size: 11px;
                    text-decoration: none;
                    transition: all 0.2s ease;
                }

                .ytr-badge-link:hover {
                    background: rgba(255, 61, 0, 0.25);
                    border-color: #ff3d00;
                    box-shadow: 0 0 10px rgba(255, 61, 0, 0.4);
                }

                .ytr-pip-button {
                    display: inline-flex;
                    align-items: center;
                    gap: 6px;
                    padding: 4px 11px;
                    background: linear-gradient(135deg, rgba(255, 61, 0, 0.22) 0%, rgba(20, 22, 32, 0.70) 100%);
                    border: 1px solid rgba(255, 61, 0, 0.50);
                    border-radius: 6px;
                    color: #ffffff;
                    font-size: 11px;
                    font-weight: 600;
                    cursor: pointer;
                    transition: all 0.2s ease;
                    box-shadow: 0 2px 8px rgba(0,0,0,0.3);
                }

                .ytr-pip-button:hover {
                    background: linear-gradient(135deg, rgba(255, 61, 0, 0.45) 0%, rgba(255, 120, 50, 0.30) 100%);
                    border-color: #ff3d00;
                    box-shadow: 0 0 12px rgba(255, 61, 0, 0.55);
                    transform: translateY(-1px);
                }

                /* Dolby Equalizer Modal & Backdrop */
                #ytr-eq-overlay {
                    position: fixed !important;
                    top: 0 !important;
                    left: 0 !important;
                    right: 0 !important;
                    bottom: 0 !important;
                    background: rgba(0, 0, 0, 0.70) !important;
                    backdrop-filter: blur(8px) !important;
                    z-index: 2147483646 !important;
                    display: none;
                    align-items: center !important;
                    justify-content: center !important;
                }

                #ytr-eq-overlay.ytr-open {
                    display: flex !important;
                }

                #ytr-eq-modal {
                    width: 620px !important;
                    max-width: 94vw !important;
                    background: #11141c !important;
                    border: 1px solid rgba(255, 255, 255, 0.16) !important;
                    border-radius: 14px !important;
                    box-shadow: 0 24px 64px rgba(0, 0, 0, 0.85), 0 0 20px rgba(255, 61, 0, 0.15) !important;
                    padding: 22px 24px !important;
                    color: #ffffff !important;
                    font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif !important;
                    user-select: none !important;
                    display: flex !important;
                    flex-direction: column !important;
                    gap: 16px !important;
                    animation: ytrModalScale 0.18s cubic-bezier(0.16, 1, 0.3, 1) !important;
                }

                @keyframes ytrModalScale {
                    from { opacity: 0; transform: scale(0.95); }
                    to { opacity: 1; transform: scale(1); }
                }

                .ytr-eq-header {
                    display: flex !important;
                    align-items: center !important;
                    justify-content: space-between !important;
                }

                .ytr-eq-title-group {
                    display: flex !important;
                    align-items: center !important;
                    gap: 10px !important;
                }

                .ytr-eq-title {
                    font-size: 15px !important;
                    font-weight: 700 !important;
                    color: #ffffff !important;
                    display: flex !important;
                    align-items: center !important;
                    gap: 8px !important;
                }

                .ytr-eq-badge {
                    font-size: 11px !important;
                    font-weight: 600 !important;
                    padding: 2px 8px !important;
                    border-radius: 12px !important;
                    background: rgba(255, 61, 0, 0.16) !important;
                    color: #ff5722 !important;
                    border: 1px solid rgba(255, 61, 0, 0.40) !important;
                }

                .ytr-eq-header-actions {
                    display: flex !important;
                    align-items: center !important;
                    gap: 10px !important;
                }

                .ytr-eq-toggle-btn {
                    font-size: 11px !important;
                    font-weight: 700 !important;
                    padding: 5px 12px !important;
                    border-radius: 6px !important;
                    border: 1px solid rgba(255, 255, 255, 0.15) !important;
                    background: rgba(255, 255, 255, 0.08) !important;
                    color: #d1d5db !important;
                    cursor: pointer !important;
                    transition: all 0.15s ease !important;
                }

                .ytr-eq-toggle-btn.ytr-active {
                    background: rgba(74, 222, 128, 0.18) !important;
                    border-color: rgba(74, 222, 128, 0.50) !important;
                    color: #4ade80 !important;
                }

                .ytr-eq-close-btn {
                    background: transparent !important;
                    border: none !important;
                    color: rgba(255, 255, 255, 0.55) !important;
                    font-size: 20px !important;
                    cursor: pointer !important;
                    padding: 0 4px !important;
                    line-height: 1 !important;
                    transition: color 0.15s ease !important;
                }

                .ytr-eq-close-btn:hover {
                    color: #ffffff !important;
                }

                .ytr-eq-presets-wrap {
                    display: flex !important;
                    flex-direction: column !important;
                    gap: 6px !important;
                }

                .ytr-eq-presets-label {
                    font-size: 10px !important;
                    font-weight: 700 !important;
                    color: rgba(255, 255, 255, 0.5) !important;
                    text-transform: uppercase !important;
                    letter-spacing: 0.5px !important;
                }

                .ytr-eq-presets-row {
                    display: flex !important;
                    flex-wrap: wrap !important;
                    align-items: center !important;
                    gap: 6px !important;
                }

                .ytr-eq-preset-chip {
                    font-size: 11px !important;
                    font-weight: 500 !important;
                    padding: 4px 10px !important;
                    border-radius: 6px !important;
                    border: 1px solid rgba(255, 255, 255, 0.1) !important;
                    background: rgba(255, 255, 255, 0.05) !important;
                    color: #9ca3af !important;
                    cursor: pointer !important;
                    transition: all 0.15s ease !important;
                }

                .ytr-eq-preset-chip:hover {
                    background: rgba(255, 255, 255, 0.12) !important;
                    color: #ffffff !important;
                }

                .ytr-eq-preset-chip.ytr-selected {
                    background: linear-gradient(135deg, rgba(255, 61, 0, 0.35) 0%, rgba(255, 120, 50, 0.22) 100%) !important;
                    border-color: #ff3d00 !important;
                    color: #ffffff !important;
                    font-weight: 600 !important;
                    box-shadow: 0 0 10px rgba(255, 61, 0, 0.3) !important;
                }

                .ytr-eq-sliders-box {
                    display: flex !important;
                    justify-content: space-between !important;
                    align-items: center !important;
                    background: rgba(0, 0, 0, 0.40) !important;
                    border: 1px solid rgba(255, 255, 255, 0.07) !important;
                    border-radius: 10px !important;
                    padding: 16px 12px !important;
                    gap: 4px !important;
                }

                .ytr-eq-col {
                    display: flex !important;
                    flex-direction: column !important;
                    align-items: center !important;
                    flex: 1 !important;
                    gap: 8px !important;
                }

                .ytr-eq-db {
                    font-size: 10px !important;
                    font-family: ui-monospace, SFMono-Regular, Menlo, monospace !important;
                    color: #38bdf8 !important;
                    min-height: 14px !important;
                    text-align: center !important;
                }

                .ytr-eq-slider {
                    -webkit-appearance: slider-vertical !important;
                    writing-mode: bt-lr !important;
                    width: 20px !important;
                    height: 120px !important;
                    cursor: pointer !important;
                    background: transparent !important;
                    accent-color: #ff3d00 !important;
                    margin: 0 !important;
                }

                .ytr-eq-freq {
                    font-size: 10px !important;
                    font-weight: 600 !important;
                    color: #9ca3af !important;
                    text-align: center !important;
                }

                .ytr-eq-footer {
                    display: flex !important;
                    align-items: center !important;
                    justify-content: space-between !important;
                    font-size: 11px !important;
                    color: rgba(255, 255, 255, 0.45) !important;
                }

                .ytr-eq-reset-btn {
                    font-size: 11px !important;
                    background: rgba(255, 255, 255, 0.06) !important;
                    border: 1px solid rgba(255, 255, 255, 0.15) !important;
                    color: #d1d5db !important;
                    padding: 4px 11px !important;
                    border-radius: 4px !important;
                    cursor: pointer !important;
                    transition: all 0.15s ease !important;
                }

                .ytr-eq-reset-btn:hover {
                    background: rgba(255, 255, 255, 0.14) !important;
                    color: #ffffff !important;
                }
            `;
            target.appendChild(style);
        } catch (_) {}
    }

    // 2. High-Precision Ad Detection & Non-Intrusive Instant Skipper
    let adWasMuted = false;
    let userWasMuted = false;

    function isAdPlaying() {
        try {
            const player = document.querySelector('#movie_player') || document.querySelector('ytmusic-player');
            if (player) {
                if (player.classList && (player.classList.contains('ad-showing') || player.classList.contains('ad-interrupting'))) {
                    return true;
                }
                if (player.hasAttribute && player.hasAttribute('ad-interrupting')) {
                    return true;
                }
                if (typeof player.getAdState === 'function' && player.getAdState() > 0) {
                    return true;
                }
                if (typeof player.isAd === 'function' && player.isAd()) {
                    return true;
                }
            }
            const video = document.querySelector('video');
            if (video && video.classList && video.classList.contains('ad-showing')) {
                return true;
            }
            const skipBtn = document.querySelector('.ytp-ad-skip-button, .ytp-ad-skip-button-modern, .ytp-skip-ad-button, .ytp-ad-text');
            if (skipBtn && (skipBtn.offsetWidth > 0 || skipBtn.offsetHeight > 0)) {
                return true;
            }
        } catch (_) {}
        return false;
    }

    function clickAdSkip() {
        try {
            const skipSelectors = [
                '.ytp-ad-skip-button-modern',
                '.ytp-skip-ad-button',
                '.ytp-ad-skip-button',
                'button.ytp-ad-skip-button-slot',
                'button.videoAdUiSkipButton',
                '.videoAdUiAction',
                '[class*="skip-button" i]',
                '[id*="skip-button" i]',
                'tp-yt-paper-button.ytmusic-mealbar-promo-renderer'
            ];
            for (const sel of skipSelectors) {
                const btns = document.querySelectorAll(sel);
                for (const b of btns) {
                    if (b && (b.offsetWidth > 0 || b.offsetHeight > 0) && !b.hasAttribute('disabled')) {
                        b.click();
                        return true;
                    }
                }
            }
        } catch (_) {}
        return false;
    }

    function neutralizeAds() {
        try {
            ensureStyles();

            const upgradeEntries = document.querySelectorAll('ytmusic-guide-entry-renderer');
            for (const u of upgradeEntries) {
                if (u.textContent && u.textContent.toLowerCase().includes('upgrade')) {
                    u.style.display = 'none';
                }
            }

            const isAd = isAdPlaying();
            const video = document.querySelector('video');
            const player = document.querySelector('#movie_player') || document.querySelector('ytmusic-player');

            if (isAd && video) {
                // Ad is actively interrupting:
                // 1. Visually hide it immediately so user NEVER sees any ad frames
                video.style.opacity = '0';
                const adOverlays = document.querySelectorAll('.ytp-ad-player-overlay, .ytp-ad-player-overlay-layout, .video-ads');
                for (const ov of adOverlays) {
                    ov.style.display = 'none';
                }

                // 2. Mute audio completely
                if (!adWasMuted) {
                    userWasMuted = video.muted;
                    video.muted = true;
                    adWasMuted = true;
                }

                // 3. Try calling player API skipAd() or cancelPlayback()
                try {
                    if (player && typeof player.skipAd === 'function') {
                        player.skipAd();
                    }
                } catch (_) {}

                // 4. Click skip button if present
                clickAdSkip();

                // 5. Jump video time directly to the end of the ad stream to fire the 'ended' event
                try {
                    if (Number.isFinite(video.duration) && video.duration > 0) {
                        video.currentTime = video.duration;
                    }
                    video.playbackRate = 16.0;
                } catch (_) {}
            } else if (video) {
                // Normal song playback: restore full visibility, normal speed, and unmute
                if (video.style.opacity === '0') {
                    video.style.opacity = '1';
                }
                if (video.playbackRate > 2.0) {
                    video.playbackRate = 1.0;
                }
                if (adWasMuted) {
                    if (!userWasMuted) {
                        video.muted = false;
                    }
                    adWasMuted = false;
                }
            }
        } catch (_) {}
    }

    // 4. Safe DOM Construction Helpers (Pure DOM Nodes - 100% immune to Trusted Types CSP restrictions)
    function createEl(tag, attrs, text) {
        const el = document.createElement(tag);
        if (attrs) {
            for (const k in attrs) {
                if (k === 'style') el.style.cssText = attrs[k];
                else if (k === 'className') el.className = attrs[k];
                else el.setAttribute(k, attrs[k]);
            }
        }
        if (text) el.textContent = text;
        return el;
    }

    function createSvg(width, height, viewBox, innerShapes) {
        const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
        svg.setAttribute('viewBox', viewBox);
        svg.setAttribute('width', String(width));
        svg.setAttribute('height', String(height));
        svg.style.width = width + 'px';
        svg.style.height = height + 'px';
        svg.style.flexShrink = '0';
        if (innerShapes) {
            for (let i = 0; i < innerShapes.length; i++) {
                const s = document.createElementNS('http://www.w3.org/2000/svg', innerShapes[i].tag);
                const a = innerShapes[i].attrs;
                if (a) {
                    for (const k in a) {
                        s.setAttribute(k, a[k]);
                    }
                }
                svg.appendChild(s);
            }
        }
        return svg;
    }

    function focusSearchBox() {
        try {
            const searchBtn = document.querySelector('ytmusic-search-box tp-yt-paper-icon-button, ytmusic-search-box [aria-label*="Search" i], tp-yt-paper-icon-button#search-button');
            if (searchBtn) searchBtn.click();
            setTimeout(() => {
                const input = document.querySelector('ytmusic-search-box input, input#input, input[type="search"]');
                if (input) {
                    input.focus();
                    input.select();
                }
            }, 60);
        } catch (_) {}
    }

    // --- Studio-Grade Web Audio Equalizer (10-Band Biquad Filters & Dolby Presets) ---
    const EQ_FREQUENCIES = [32, 64, 125, 250, 500, 1000, 2000, 4000, 8000, 16000];
    const EQ_FREQ_LABELS = ['32', '64', '125', '250', '500', '1k', '2k', '4k', '8k', '16k'];
    const DOLBY_PRESETS = {
        'Dolby Atmos': [4, 3, 1, 0, -1, 1, 2, 3, 4, 5],
        'Dolby Music': [3, 2, 1, 0, 1, 2, 2, 2, 3, 3],
        'Dolby Movie': [5, 4, 2, -1, 0, 1, 2, 3, 4, 3],
        'Bass Boost':  [6, 5, 4, 2, 0, 0, 0, 1, 1, 1],
        'Vocal':       [-2, -1, 0, 1, 3, 3, 2, 1, 0, 0],
        'Rock':        [4, 3, 1, -1, -1, 1, 2, 3, 4, 4],
        'Gaming':      [3, 2, 0, -2, 1, 2, 4, 3, 2, 1],
        'Flat':        [0, 0, 0, 0, 0, 0, 0, 0, 0, 0]
    };

    let eqMasterEnabled = localStorage.getItem('ytr_eq_enabled') !== 'false';
    let currentPresetName = localStorage.getItem('ytr_eq_preset') || 'Dolby Atmos';
    let currentGains = null;
    try {
        const saved = localStorage.getItem('ytr_eq_gains');
        if (saved) currentGains = JSON.parse(saved);
    } catch (_) {}
    if (!Array.isArray(currentGains) || currentGains.length !== 10) {
        currentGains = DOLBY_PRESETS[currentPresetName] ? [...DOLBY_PRESETS[currentPresetName]] : [...DOLBY_PRESETS['Dolby Atmos']];
    }

    let audioCtx = null;
    let audioSourceNode = null;
    let eqFilterNodes = [];
    let fadeGainNode = null;
    let lastConnectedVideo = null;

    function initAudioGraph(video) {
        if (!video) return;
        if (lastConnectedVideo === video && audioCtx) {
            if (audioCtx.state === 'suspended') {
                audioCtx.resume().catch(() => {});
            }
            return;
        }

        try {
            const AudioContextClass = window.AudioContext || window.webkitAudioContext;
            if (!AudioContextClass) return;

            if (!audioCtx) {
                audioCtx = new AudioContextClass();
            }

            if (lastConnectedVideo !== video) {
                audioSourceNode = audioCtx.createMediaElementSource(video);
                lastConnectedVideo = video;

                // Build 10-Band Biquad Filter chain
                eqFilterNodes = [];
                for (let i = 0; i < EQ_FREQUENCIES.length; i++) {
                    const freq = EQ_FREQUENCIES[i];
                    const filter = audioCtx.createBiquadFilter();
                    if (i === 0) {
                        filter.type = 'lowshelf';
                    } else if (i === EQ_FREQUENCIES.length - 1) {
                        filter.type = 'highshelf';
                    } else {
                        filter.type = 'peaking';
                        filter.Q.value = 1.4;
                    }
                    filter.frequency.value = freq;
                    filter.gain.value = eqMasterEnabled ? currentGains[i] : 0.0;
                    eqFilterNodes.push(filter);
                }

                // Dedicated GainNode for Smooth Audio Fading (never touches video.volume)
                fadeGainNode = audioCtx.createGain();
                fadeGainNode.gain.value = 1.0;

                // Chain: Source -> Filter[0] -> ... -> Filter[9] -> FadeGain -> Destination
                let prev = audioSourceNode;
                for (let i = 0; i < eqFilterNodes.length; i++) {
                    prev.connect(eqFilterNodes[i]);
                    prev = eqFilterNodes[i];
                }
                prev.connect(fadeGainNode);
                fadeGainNode.connect(audioCtx.destination);

                window.__ytr_audioCtx = audioCtx;
                window.__ytr_eqFilters = eqFilterNodes;
                window.__ytr_fadeGain = fadeGainNode;
            }

            if (audioCtx.state === 'suspended') {
                audioCtx.resume().catch(() => {});
            }
        } catch (e) {
            console.warn('[ytr-audio] initAudioGraph error:', e);
        }
    }

    function applyEqGains(gains, presetName) {
        currentGains = [...gains];
        if (presetName) currentPresetName = presetName;
        localStorage.setItem('ytr_eq_gains', JSON.stringify(currentGains));
        if (presetName) localStorage.setItem('ytr_eq_preset', currentPresetName);

        if (audioCtx && eqFilterNodes.length === 10) {
            const now = audioCtx.currentTime;
            for (let i = 0; i < 10; i++) {
                const target = eqMasterEnabled ? currentGains[i] : 0.0;
                try {
                    eqFilterNodes[i].gain.cancelScheduledValues(now);
                    eqFilterNodes[i].gain.setValueAtTime(eqFilterNodes[i].gain.value, now);
                    eqFilterNodes[i].gain.linearRampToValueAtTime(target, now + 0.05);
                } catch (_) {
                    eqFilterNodes[i].gain.value = target;
                }
            }
        }
        updateEqualizerModalUI();
        updateEqButtonUI();
    }

    function setEqMasterEnabled(enabled) {
        eqMasterEnabled = enabled;
        localStorage.setItem('ytr_eq_enabled', enabled ? 'true' : 'false');
        applyEqGains(currentGains, currentPresetName);
        updateEqualizerModalUI();
        updateEqButtonUI();
    }

    function updateEqButtonUI() {
        const btn = document.getElementById('ytr-btn-eq');
        const text = document.getElementById('ytr-btn-eq-text');
        if (!btn) return;
        const targetBg = eqMasterEnabled ? 'rgba(255, 61, 0, 0.18)' : 'rgba(255, 255, 255, 0.06)';
        btn.style.background = targetBg;
        btn.style.borderColor = eqMasterEnabled ? 'rgba(255, 61, 0, 0.45)' : 'rgba(255, 255, 255, 0.1)';
        btn.style.color = eqMasterEnabled ? '#ff5722' : '#d1d1d1';
        if (text) {
            text.textContent = eqMasterEnabled ? `EQ (${currentPresetName})` : 'EQ: OFF';
        }
    }

    function updateEqualizerModalUI() {
        const badge = document.getElementById('ytr-eq-preset-badge');
        if (badge) badge.textContent = currentPresetName;

        const toggleBtn = document.getElementById('ytr-eq-master-toggle');
        if (toggleBtn) {
            toggleBtn.textContent = eqMasterEnabled ? '\u2713 EQ: ON' : '\u25CB EQ: OFF';
            if (eqMasterEnabled) toggleBtn.classList.add('ytr-active');
            else toggleBtn.classList.remove('ytr-active');
        }

        const chips = document.querySelectorAll('.ytr-eq-preset-chip');
        chips.forEach(chip => {
            const pName = chip.getAttribute('data-preset');
            if (pName === currentPresetName) chip.classList.add('ytr-selected');
            else chip.classList.remove('ytr-selected');
        });

        for (let i = 0; i < 10; i++) {
            const slider = document.getElementById(`ytr-eq-slider-${i}`);
            const dbText = document.getElementById(`ytr-eq-db-${i}`);
            const val = currentGains[i];
            if (slider && parseFloat(slider.value) !== val) {
                slider.value = String(val);
            }
            if (dbText) {
                dbText.textContent = (val > 0 ? '+' : '') + val + ' dB';
            }
        }
    }

    function toggleEqualizerModal() {
        let overlay = document.getElementById('ytr-eq-overlay');
        if (!overlay) {
            overlay = buildEqualizerModal();
        }
        if (overlay) {
            const isOpen = overlay.classList.toggle('ytr-open');
            if (isOpen) {
                updateEqualizerModalUI();
            }
        }
    }

    function buildEqualizerModal() {
        let overlay = document.getElementById('ytr-eq-overlay');
        if (overlay) return overlay;

        overlay = createEl('div', { id: 'ytr-eq-overlay' });
        const modal = createEl('div', { id: 'ytr-eq-modal' });

        // 1. Header
        const header = createEl('div', { className: 'ytr-eq-header' });
        const titleGrp = createEl('div', { className: 'ytr-eq-title-group' });
        const titleSvg = createSvg(16, 16, '0 0 24 24', [
            { tag: 'path', attrs: { d: 'M10 20h4V4h-4v16zm-6 0h4v-8H4v8zM16 9v11h4V9h-4z', fill: '#ff3d00' } }
        ]);
        const titleSpan = createEl('span', { className: 'ytr-eq-title' });
        titleSpan.appendChild(titleSvg);
        titleSpan.appendChild(document.createTextNode('Dolby Audio Equalizer'));
        const badge = createEl('span', { id: 'ytr-eq-preset-badge', className: 'ytr-eq-badge' }, currentPresetName);
        titleGrp.appendChild(titleSpan);
        titleGrp.appendChild(badge);

        const actions = createEl('div', { className: 'ytr-eq-header-actions' });
        const toggleBtn = createEl('button', {
            id: 'ytr-eq-master-toggle',
            className: 'ytr-eq-toggle-btn' + (eqMasterEnabled ? ' ytr-active' : ''),
            title: 'Master Equalizer Toggle'
        }, eqMasterEnabled ? '\u2713 EQ: ON' : '\u25CB EQ: OFF');
        toggleBtn.addEventListener('click', () => {
            setEqMasterEnabled(!eqMasterEnabled);
        });

        const closeBtn = createEl('button', { className: 'ytr-eq-close-btn', title: 'Close (Esc)' }, '\u2715');
        closeBtn.addEventListener('click', () => overlay.classList.remove('ytr-open'));

        actions.appendChild(toggleBtn);
        actions.appendChild(closeBtn);

        header.appendChild(titleGrp);
        header.appendChild(actions);

        // 2. Presets Row
        const presetsWrap = createEl('div', { className: 'ytr-eq-presets-wrap' });
        const presetsLabel = createEl('div', { className: 'ytr-eq-presets-label' }, 'Dolby Studio Presets');
        const presetsRow = createEl('div', { className: 'ytr-eq-presets-row' });

        for (const pName in DOLBY_PRESETS) {
            const chip = createEl('button', {
                className: 'ytr-eq-preset-chip' + (pName === currentPresetName ? ' ytr-selected' : ''),
                'data-preset': pName
            }, pName);
            chip.addEventListener('click', () => {
                applyEqGains(DOLBY_PRESETS[pName], pName);
            });
            presetsRow.appendChild(chip);
        }
        presetsWrap.appendChild(presetsLabel);
        presetsWrap.appendChild(presetsRow);

        // 3. 10 Vertical Sliders Box
        const slidersBox = createEl('div', { className: 'ytr-eq-sliders-box' });
        for (let i = 0; i < 10; i++) {
            const col = createEl('div', { className: 'ytr-eq-col' });
            const val = currentGains[i];
            const dbText = createEl('span', { id: `ytr-eq-db-${i}`, className: 'ytr-eq-db' }, (val > 0 ? '+' : '') + val + ' dB');
            const slider = createEl('input', {
                id: `ytr-eq-slider-${i}`,
                type: 'range',
                min: '-12',
                max: '12',
                step: '0.5',
                value: String(val),
                orient: 'vertical',
                className: 'ytr-eq-slider',
                title: `${EQ_FREQ_LABELS[i]}Hz (${val} dB)`
            });
            slider.addEventListener('input', (e) => {
                const newGains = [...currentGains];
                newGains[i] = parseFloat(e.target.value);
                applyEqGains(newGains, 'Custom');
            });
            const freqText = createEl('span', { className: 'ytr-eq-freq' }, EQ_FREQ_LABELS[i]);

            col.appendChild(dbText);
            col.appendChild(slider);
            col.appendChild(freqText);
            slidersBox.appendChild(col);
        }

        // 4. Footer
        const footer = createEl('div', { className: 'ytr-eq-footer' });
        const hint = createEl('span', {}, '10-Band Biquad Studio Equalizer \u2022 32Hz - 16kHz \u2022 64-bit Audio DSP');
        const resetBtn = createEl('button', { className: 'ytr-eq-reset-btn' }, 'Reset to Flat (0 dB)');
        resetBtn.addEventListener('click', () => {
            applyEqGains(DOLBY_PRESETS['Flat'], 'Flat');
        });
        footer.appendChild(hint);
        footer.appendChild(resetBtn);

        modal.appendChild(header);
        modal.appendChild(presetsWrap);
        modal.appendChild(slidersBox);
        modal.appendChild(footer);

        overlay.appendChild(modal);

        overlay.addEventListener('click', (e) => {
            if (e.target === overlay) overlay.classList.remove('ytr-open');
        });

        document.body.appendChild(overlay);
        return overlay;
    }

    // --- Studio-Grade Smooth Audio Fading Engine ---
    let smoothAudioEnabled = localStorage.getItem('ytr_smooth_audio') === 'true';
    let isActionBypassed = false;
    let shouldFadeInNextTrack = false;

    function updateSmoothAudioUI() {
        try {
            const item = document.getElementById('ytr-menu-fade');
            if (item) {
                const targetText = smoothAudioEnabled ? '\u2713 Smooth Audio (Fade): ON' : '\u25CB Smooth Audio (Fade): OFF';
                if (item.textContent !== targetText) {
                    item.textContent = targetText;
                    item.style.color = smoothAudioEnabled ? '#4ade80' : '#9ca3af';
                    item.style.fontWeight = smoothAudioEnabled ? '600' : 'normal';
                }
            }

            const pill = document.getElementById('ytr-btn-smooth');
            const pillText = document.getElementById('ytr-btn-smooth-text');
            if (pill) {
                const targetBg = smoothAudioEnabled ? 'rgba(74, 222, 128, 0.16)' : 'rgba(255, 255, 255, 0.06)';
                if (pill.style.background !== targetBg) {
                    pill.style.background = targetBg;
                    pill.style.borderColor = smoothAudioEnabled ? 'rgba(74, 222, 128, 0.45)' : 'rgba(255, 255, 255, 0.1)';
                    pill.style.color = smoothAudioEnabled ? '#4ade80' : '#d1d1d1';
                }
            }
            if (pillText) {
                const targetPillText = smoothAudioEnabled ? 'Smooth Audio: ON' : 'Smooth Audio: OFF';
                if (pillText.textContent !== targetPillText) {
                    pillText.textContent = targetPillText;
                }
            }
        } catch (_) {}
    }

    function smoothTogglePlayPause() {
        const video = document.querySelector('video');
        const playBtn = document.querySelector('#play-pause-button, .play-pause-button');
        const player = document.querySelector('#movie_player');

        function triggerNativePlayPause() {
            isActionBypassed = true;
            try {
                if (playBtn) {
                    playBtn.click();
                } else if (player && typeof player.getPlayerState === 'function') {
                    if (player.getPlayerState() === 1) player.pauseVideo();
                    else player.playVideo();
                } else if (video) {
                    video.paused ? video.play() : video.pause();
                }
            } finally {
                setTimeout(() => { isActionBypassed = false; }, 300);
            }
        }

        if (!video || !smoothAudioEnabled || !audioCtx || !fadeGainNode) {
            triggerNativePlayPause();
            return;
        }

        if (audioCtx.state === 'suspended') {
            audioCtx.resume().catch(() => {});
        }

        if (video.paused) {
            // Smooth Resume: set gain to 0, start playing, ramp gain up to 1.0 in 220ms
            const now = audioCtx.currentTime;
            fadeGainNode.gain.cancelScheduledValues(now);
            fadeGainNode.gain.setValueAtTime(0.001, now);
            triggerNativePlayPause();
            fadeGainNode.gain.linearRampToValueAtTime(1.0, now + 0.22);
        } else {
            // Smooth Pause: ramp gain down from 1.0 to 0.001 in 180ms, then pause and reset gain to 1.0
            const now = audioCtx.currentTime;
            fadeGainNode.gain.cancelScheduledValues(now);
            fadeGainNode.gain.setValueAtTime(fadeGainNode.gain.value, now);
            fadeGainNode.gain.linearRampToValueAtTime(0.001, now + 0.18);
            setTimeout(() => {
                triggerNativePlayPause();
                if (audioCtx && fadeGainNode) {
                    const t = audioCtx.currentTime;
                    fadeGainNode.gain.setValueAtTime(1.0, t);
                }
            }, 190);
        }
    }

    function smoothNextTrack() {
        const video = document.querySelector('video');
        const nextBtn = document.querySelector('.next-button.ytmusic-player-bar, #next-button, button.next-button');
        const player = document.querySelector('#movie_player');

        function triggerNativeNext() {
            isActionBypassed = true;
            try {
                if (player && typeof player.nextVideo === 'function') {
                    player.nextVideo();
                } else if (nextBtn) {
                    nextBtn.click();
                }
            } finally {
                setTimeout(() => { isActionBypassed = false; }, 300);
            }
        }

        if (!smoothAudioEnabled || !video || video.paused || !audioCtx || !fadeGainNode) {
            triggerNativeNext();
            return;
        }

        shouldFadeInNextTrack = true;
        const now = audioCtx.currentTime;
        fadeGainNode.gain.cancelScheduledValues(now);
        fadeGainNode.gain.setValueAtTime(fadeGainNode.gain.value, now);
        fadeGainNode.gain.linearRampToValueAtTime(0.001, now + 0.16);
        setTimeout(() => {
            triggerNativeNext();
            if (audioCtx && fadeGainNode) {
                fadeGainNode.gain.setValueAtTime(1.0, audioCtx.currentTime);
            }
        }, 170);
    }

    function smoothPrevTrack() {
        const video = document.querySelector('video');
        const prevBtn = document.querySelector('.previous-button.ytmusic-player-bar, #previous-button, button.previous-button');
        const player = document.querySelector('#movie_player');

        function triggerNativePrev() {
            isActionBypassed = true;
            try {
                if (player && typeof player.previousVideo === 'function') {
                    player.previousVideo();
                } else if (prevBtn) {
                    prevBtn.click();
                }
            } finally {
                setTimeout(() => { isActionBypassed = false; }, 300);
            }
        }

        if (!smoothAudioEnabled || !video || video.paused || !audioCtx || !fadeGainNode) {
            triggerNativePrev();
            return;
        }

        shouldFadeInNextTrack = true;
        const now = audioCtx.currentTime;
        fadeGainNode.gain.cancelScheduledValues(now);
        fadeGainNode.gain.setValueAtTime(fadeGainNode.gain.value, now);
        fadeGainNode.gain.linearRampToValueAtTime(0.001, now + 0.16);
        setTimeout(() => {
            triggerNativePrev();
            if (audioCtx && fadeGainNode) {
                fadeGainNode.gain.setValueAtTime(1.0, audioCtx.currentTime);
            }
        }, 170);
    }

    window.__ytr_smoothToggle = smoothTogglePlayPause;
    window.__ytr_smoothNext = smoothNextTrack;
    window.__ytr_smoothPrev = smoothPrevTrack;
    window.__ytr_toggleEqualizer = toggleEqualizerModal;
    window.__ytr_setEqMaster = setEqMasterEnabled;
    window.__ytr_applyPreset = (p) => applyEqGains(DOLBY_PRESETS[p] || DOLBY_PRESETS['Flat'], p);

    function installTopBarUI() {
        try {
            ensureStyles();
            if (!document.body) return;

            let bar = document.getElementById('ytmd-title-bar-main-panel');
            if (bar) {
                if (document.body && bar.parentElement !== document.body) {
                    document.body.prepend(bar);
                }
                return;
            }

            bar = createEl('nav', { id: 'ytmd-title-bar-main-panel' });

            // 1. Left Section: Brand, Navigation buttons, Dropdown menus
            const topbarLeft = createEl('div', { className: 'ytr-topbar-left' });

            const brand = createEl('div', { className: 'ytr-topbar-brand' });
            const brandSvg = createSvg(16, 16, '0 0 24 24', [
                { tag: 'circle', attrs: { cx: '12', cy: '12', r: '11', fill: '#ff0000' } },
                { tag: 'polygon', attrs: { points: '9.5,7.5 16.5,12 9.5,16.5', fill: '#ffffff' } }
            ]);
            const brandTitle = createEl('span', { className: 'ytr-brand-title' }, 'ytr-music');
            brand.appendChild(brandSvg);
            brand.appendChild(brandTitle);

            const navBtns = createEl('div', { className: 'ytr-nav-btns' });
            const btnBack = createEl('button', { className: 'ytr-nav-btn', id: 'ytr-btn-back', title: 'Back' }, '\u25C0');
            const btnForward = createEl('button', { className: 'ytr-nav-btn', id: 'ytr-btn-forward', title: 'Forward' }, '\u25B6');
            const btnReload = createEl('button', { className: 'ytr-nav-btn', id: 'ytr-btn-reload', title: 'Reload' }, '\u27F3');

            const btnSearch = createEl('button', {
                className: 'ytr-nav-btn',
                id: 'ytr-btn-search',
                title: 'Search (Ctrl+K or /)',
                style: 'width: auto; padding: 0 8px; gap: 5px; display: inline-flex; align-items: center;'
            });
            const searchSvg = createSvg(12, 12, '0 0 24 24', [
                { tag: 'path', attrs: { d: 'M15.5 14h-.79l-.28-.27A6.471 6.471 0 0 0 16 9.5 6.5 6.5 0 1 0 9.5 16c1.61 0 3.09-.59 4.23-1.57l.27.28v.79l5 4.99L20.49 19l-4.99-5zm-6 0C7.01 14 5 11.99 5 9.5S7.01 5 9.5 5 14 7.01 14 9.5 11.99 14 9.5 14z', fill: 'currentColor' } }
            ]);
            const searchSpan = createEl('span', { style: 'font-size: 10px; opacity: 0.85;' }, 'Search');
            const searchKbd = createEl('kbd', { style: 'font-size: 9px; background: rgba(255,255,255,0.14); padding: 1px 4px; border-radius: 3px; border: 1px solid rgba(255,255,255,0.22); font-family: monospace;' }, 'Ctrl+K');
            btnSearch.appendChild(searchSvg);
            btnSearch.appendChild(searchSpan);
            btnSearch.appendChild(searchKbd);

            const btnSmooth = createEl('button', {
                className: 'ytr-nav-btn',
                id: 'ytr-btn-smooth',
                title: 'Smooth Audio Fading (Fade on Pause, Resume, Skip) - Click to Toggle',
                style: 'width: auto; padding: 0 8px; gap: 5px; display: inline-flex; align-items: center; border-radius: 4px; transition: all 0.2s ease;'
            });
            const smoothSvg = createSvg(12, 12, '0 0 24 24', [
                { tag: 'path', attrs: { d: 'M12 3v10.55c-.59-.34-1.27-.55-2-.55-2.21 0-4 1.79-4 4s1.79 4 4 4 4-1.79 4-4V7h4V3h-6z', fill: 'currentColor' } }
            ]);
            const smoothSpan = createEl('span', { id: 'ytr-btn-smooth-text', style: 'font-size: 10px; font-weight: 600;' }, 'Smooth Audio: OFF');
            btnSmooth.appendChild(smoothSvg);
            btnSmooth.appendChild(smoothSpan);

            const btnEq = createEl('button', {
                className: 'ytr-nav-btn',
                id: 'ytr-btn-eq',
                title: 'Dolby Audio Equalizer & Studio Presets (Click to Open)',
                style: 'width: auto; padding: 0 8px; gap: 5px; display: inline-flex; align-items: center; border-radius: 4px; transition: all 0.2s ease;'
            });
            const eqSvg = createSvg(12, 12, '0 0 24 24', [
                { tag: 'path', attrs: { d: 'M10 20h4V4h-4v16zm-6 0h4v-8H4v8zM16 9v11h4V9h-4z', fill: 'currentColor' } }
            ]);
            const eqSpan = createEl('span', { id: 'ytr-btn-eq-text', style: 'font-size: 10px; font-weight: 600;' }, 'EQ');
            btnEq.appendChild(eqSvg);
            btnEq.appendChild(eqSpan);

            navBtns.appendChild(btnBack);
            navBtns.appendChild(btnForward);
            navBtns.appendChild(btnReload);
            navBtns.appendChild(btnSearch);
            navBtns.appendChild(btnSmooth);
            navBtns.appendChild(btnEq);

            const menuItems = createEl('div', { className: 'ytr-menu-items' });
            function makeDropdown(label, items) {
                const dd = createEl('div', { className: 'ytr-menu-dropdown' });
                const lbl = createEl('button', { className: 'ytr-menu-label' }, label);
                const content = createEl('div', { className: 'ytr-dropdown-content' });
                for (let i = 0; i < items.length; i++) {
                    const it = items[i];
                    if (it === 'divider') {
                        content.appendChild(createEl('div', { className: 'ytr-divider' }));
                    } else {
                        content.appendChild(createEl('div', { className: 'ytr-menu-item', id: it.id }, it.text));
                    }
                }
                dd.appendChild(lbl);
                dd.appendChild(content);
                return dd;
            }

            const fileDd = makeDropdown('File', [
                { id: 'ytr-menu-pip', text: 'Desktop Miniplayer (Ctrl+Alt+M)' },
                { id: 'ytr-menu-trim', text: 'Trim RAM Compaction' },
                'divider',
                { id: 'ytr-menu-quit', text: 'Quit' }
            ]);
            const navDd = makeDropdown('Navigate', [
                { id: 'ytr-menu-home', text: 'Home' },
                { id: 'ytr-menu-explore', text: 'Explore' },
                { id: 'ytr-menu-library', text: 'Library' }
            ]);
            const playDd = makeDropdown('Playback', [
                { id: 'ytr-menu-playpause', text: 'Play / Pause (Space)' },
                { id: 'ytr-menu-next', text: 'Next Track (N)' },
                { id: 'ytr-menu-prev', text: 'Previous Track (P)' },
                { id: 'ytr-menu-like', text: 'Like Track (L)' },
                'divider',
                { id: 'ytr-menu-fade', text: 'Smooth Audio (Fade): OFF' },
                { id: 'ytr-menu-eq', text: 'Dolby Equalizer (EQ)...' }
            ]);

            menuItems.appendChild(fileDd);
            menuItems.appendChild(navDd);
            menuItems.appendChild(playDd);

            topbarLeft.appendChild(brand);
            topbarLeft.appendChild(navBtns);
            topbarLeft.appendChild(menuItems);

            // 2. Center Section: Song Ticker
            const topbarCenter = createEl('div', { className: 'ytr-topbar-center' });
            const ticker = createEl('span', { id: 'ytr-topbar-ticker' }, '\uD83C\uDFB5 Ready to Play');
            topbarCenter.appendChild(ticker);

            // 3. Right Section: GitHub badge + PiP button
            const topbarRight = createEl('div', { className: 'ytr-topbar-right' });
            const badgeLink = createEl('a', {
                href: 'https://github.com/iAlturki',
                target: '_blank',
                className: 'ytr-badge-link',
                title: 'ytr-music (iALTURKi Edition) - Visit GitHub'
            });
            const b1 = createEl('span', { style: 'color: #ff3d00; font-weight: 700;' }, 'ytr-music');
            const b2 = createEl('span', { style: 'color: rgba(255,255,255,0.35);' }, ' \u2022 ');
            const b3 = createEl('span', { style: 'color: #ffffff; font-weight: 500;' }, 'iALTURKi Edition \u00A9 2026');
            badgeLink.appendChild(b1);
            badgeLink.appendChild(b2);
            badgeLink.appendChild(b3);

            const pipBtn = createEl('button', {
                id: 'ytr-pip-btn',
                className: 'ytr-pip-button',
                title: 'Picture-in-Picture: Close window into floating desktop miniplayer'
            });
            const pipSvg = createSvg(13, 13, '0 0 24 24', [
                { tag: 'path', attrs: { d: 'M19 11h-8v6h8v-6zm4 8V4.98C23 3.88 22.1 3 21 3H3c-1.1 0-2 .88-2 1.98V19c0 1.1.9 2 2 2h18c1.1 0 2-.9 2-2zm-2 .02H3V4.97h18v14.05z', fill: '#ff3d00' } }
            ]);
            const pipSpan = createEl('span', {}, 'Picture-in-Picture');
            pipBtn.appendChild(pipSvg);
            pipBtn.appendChild(pipSpan);

            topbarRight.appendChild(badgeLink);
            topbarRight.appendChild(pipBtn);

            // Assemble into root bar
            bar.appendChild(topbarLeft);
            bar.appendChild(topbarCenter);
            bar.appendChild(topbarRight);

            // Attach event listeners safely (immune to CSP inline script blocking)
            bar.querySelector('#ytr-btn-back')?.addEventListener('click', () => window.history.back());
            bar.querySelector('#ytr-btn-forward')?.addEventListener('click', () => window.history.forward());
            bar.querySelector('#ytr-btn-reload')?.addEventListener('click', () => window.location.reload());
            bar.querySelector('#ytr-btn-search')?.addEventListener('click', focusSearchBox);

            function sendAppMsg(obj) {
                if (window.chrome && window.chrome.webview) {
                    window.chrome.webview.postMessage(JSON.stringify(obj));
                }
            }

            bar.querySelector('#ytr-menu-pip')?.addEventListener('click', () => sendAppMsg({ type: 'enter_pip' }));
            bar.querySelector('#ytr-pip-btn')?.addEventListener('click', () => sendAppMsg({ type: 'enter_pip' }));
            bar.querySelector('#ytr-menu-trim')?.addEventListener('click', () => sendAppMsg({ type: 'trim_memory' }));
            bar.querySelector('#ytr-menu-quit')?.addEventListener('click', () => sendAppMsg({ type: 'quit' }));

            bar.querySelector('#ytr-menu-home')?.addEventListener('click', () => {
                const el = document.querySelector('ytmusic-pivot-bar-item-renderer:nth-child(1), a[href="/"]');
                if (el) el.click(); else window.location.href = '/';
            });
            bar.querySelector('#ytr-menu-explore')?.addEventListener('click', () => {
                const el = document.querySelector('ytmusic-pivot-bar-item-renderer:nth-child(2), a[href*="explore"]');
                if (el) el.click(); else window.location.href = '/explore';
            });
            bar.querySelector('#ytr-menu-library')?.addEventListener('click', () => {
                const el = document.querySelector('ytmusic-pivot-bar-item-renderer:nth-child(3), a[href*="library"]');
                if (el) el.click(); else window.location.href = '/library';
            });

            bar.querySelector('#ytr-menu-playpause')?.addEventListener('click', () => {
                smoothTogglePlayPause();
            });
            bar.querySelector('#ytr-menu-next')?.addEventListener('click', () => {
                smoothNextTrack();
            });
            bar.querySelector('#ytr-menu-prev')?.addEventListener('click', () => {
                smoothPrevTrack();
            });
            bar.querySelector('#ytr-menu-like')?.addEventListener('click', () => {
                const btn = document.querySelector('#like-button-renderer yt-button-shape button, .ytmusic-like-button-renderer button, ytmusic-like-button-renderer tp-yt-paper-icon-button');
                if (btn) btn.click();
            });
            function toggleSmoothAudio() {
                smoothAudioEnabled = !smoothAudioEnabled;
                localStorage.setItem('ytr_smooth_audio', smoothAudioEnabled ? 'true' : 'false');
                updateSmoothAudioUI();
                if (window.chrome && window.chrome.webview) {
                    window.chrome.webview.postMessage(JSON.stringify({
                        type: 'log',
                        message: 'Smooth Audio toggled: ' + (smoothAudioEnabled ? 'ON' : 'OFF')
                    }));
                }
            }

            bar.querySelector('#ytr-btn-smooth')?.addEventListener('click', toggleSmoothAudio);
            bar.querySelector('#ytr-menu-fade')?.addEventListener('click', toggleSmoothAudio);
            bar.querySelector('#ytr-btn-eq')?.addEventListener('click', toggleEqualizerModal);
            bar.querySelector('#ytr-menu-eq')?.addEventListener('click', toggleEqualizerModal);

            updateSmoothAudioUI();
            updateEqButtonUI();
            buildEqualizerModal();

            document.body.prepend(bar);

            if (window.chrome && window.chrome.webview) {
                window.chrome.webview.postMessage(JSON.stringify({
                    type: 'log',
                    message: 'TopBar successfully inserted into document.body!'
                }));
            }
        } catch (err) {
            if (window.chrome && window.chrome.webview) {
                window.chrome.webview.postMessage(JSON.stringify({
                    type: 'log',
                    message: 'TopBar install error: ' + (err && err.message)
                }));
            }
        }
    }

    // 5. Track metadata and state reporter
    let lastTitle = '';
    let lastArtist = '';
    let lastPaused = true;
    let lastTime = 0;
    let lastLiked = false;
    let lastDisliked = false;

    function sendState(force = false) {
        try {
            const video = document.querySelector('video');
            const media = navigator.mediaSession ? navigator.mediaSession.metadata : null;

            const titleEl = document.querySelector('ytmusic-player-bar .title');
            const artistEl = document.querySelector('ytmusic-player-bar .byline');
            const imgEl = document.querySelector('ytmusic-player-bar .image');

            const title = (media && media.title) ? media.title : (titleEl ? titleEl.textContent.trim() : '');
            const artist = (media && media.artist) ? media.artist : (artistEl ? artistEl.textContent.trim() : '');
            const artwork = (media && media.artwork && media.artwork.length) ? media.artwork[media.artwork.length - 1].src : (imgEl ? imgEl.src : '');
            const paused = video ? video.paused : true;
            const curTime = video ? Math.floor(video.currentTime) : 0;
            const duration = video && isFinite(video.duration) ? Math.floor(video.duration) : 0;

            let volume = 100;
            try {
                const player = document.querySelector('#movie_player');
                if (player && typeof player.getVolume === 'function') {
                    const pv = player.getVolume();
                    if (typeof pv === 'number' && !isNaN(pv)) volume = Math.round(pv);
                } else if (video && typeof video.volume === 'number') {
                    volume = Math.round(Math.sqrt(video.volume) * 100);
                }
            } catch (_) {}

            const likeRenderer = document.querySelector('ytmusic-like-button-renderer, #like-button-renderer');
            const likeStatus = likeRenderer ? likeRenderer.getAttribute('like-status') : '';
            const isLiked = likeStatus === 'LIKE';
            const isDisliked = likeStatus === 'DISLIKE';

            // Update topbar song ticker
            const ticker = document.getElementById('ytr-topbar-ticker');
            if (ticker) {
                const likeBadge = isLiked ? ' \u2665' : '';
                if (title && artist) {
                    ticker.textContent = `\uD83C\uDFB5 ${title} \u2014 ${artist}${likeBadge}`;
                } else if (title) {
                    ticker.textContent = `\uD83C\uDFB5 ${title}${likeBadge}`;
                } else {
                    ticker.textContent = '\uD83C\uDFB5 Ready to Play';
                }
            }

            if (force || title !== lastTitle || artist !== lastArtist || paused !== lastPaused || isLiked !== lastLiked || isDisliked !== lastDisliked || Math.abs(curTime - lastTime) >= 1) {
                lastTitle = title;
                lastArtist = artist;
                lastPaused = paused;
                lastLiked = isLiked;
                lastDisliked = isDisliked;
                lastTime = curTime;

                if (window.chrome && window.chrome.webview) {
                    window.chrome.webview.postMessage(JSON.stringify({
                        type: 'state',
                        title: title || 'Ready to Play',
                        artist: artist || 'YouTube Music',
                        artwork: artwork,
                        paused: paused,
                        currentTime: curTime,
                        duration: duration,
                        volume: volume,
                        isLiked: isLiked,
                        isDisliked: isDisliked
                    }));
                }
            }
        } catch (_) {}
    }

    // 6. Initialization & Event Wiring
    let isBridgeInitialized = false;

    function initBridge() {
        try {
            ensureStyles();
            neutralizeAds();
            installTopBarUI();
            const initVid = document.querySelector('video');
            if (initVid) initAudioGraph(initVid);
            sendState(true);
        } catch (e) {
            if (window.chrome && window.chrome.webview) {
                window.chrome.webview.postMessage(JSON.stringify({ type: 'log', message: 'initBridge error: ' + (e && e.message) }));
            }
        }

        if (isBridgeInitialized) return;
        isBridgeInitialized = true;

        if (window.chrome && window.chrome.webview) {
            window.chrome.webview.postMessage(JSON.stringify({ type: 'log', message: 'Bridge successfully initialized, starting timers' }));
        }

        setInterval(neutralizeAds, 500);
        setInterval(installTopBarUI, 1000);
        setInterval(() => {
            const vid = document.querySelector('video');
            if (vid) initAudioGraph(vid);
            sendState(false);
        }, 500);

        try {
            const unlockAudio = () => {
                if (audioCtx && audioCtx.state === 'suspended') {
                    audioCtx.resume().catch(() => {});
                }
            };
            window.addEventListener('click', unlockAudio, true);
            window.addEventListener('keydown', unlockAudio, true);

            document.addEventListener('play', () => {
                const video = document.querySelector('video');
                if (video) initAudioGraph(video);
                unlockAudio();
                if (smoothAudioEnabled && shouldFadeInNextTrack && audioCtx && fadeGainNode) {
                    shouldFadeInNextTrack = false;
                    const now = audioCtx.currentTime;
                    fadeGainNode.gain.cancelScheduledValues(now);
                    fadeGainNode.gain.setValueAtTime(0.001, now);
                    fadeGainNode.gain.linearRampToValueAtTime(1.0, now + 0.22);
                }
                sendState(true);
            }, true);
            document.addEventListener('volumechange', () => sendState(false), true);
            document.addEventListener('pause', () => sendState(true), true);
            document.addEventListener('loadeddata', () => {
                const video = document.querySelector('video');
                if (video) initAudioGraph(video);
                sendState(true);
            }, true);
            document.addEventListener('canplay', () => {
                const video = document.querySelector('video');
                if (video) initAudioGraph(video);
            }, true);
            document.addEventListener('timeupdate', () => neutralizeAds(), true);

            window.addEventListener('keydown', (e) => {
                const target = e.target;
                const isEditing = target && (target.tagName === 'INPUT' || target.tagName === 'TEXTAREA' || target.isContentEditable);
                if (e.key === 'Escape') {
                    const overlay = document.getElementById('ytr-eq-overlay');
                    if (overlay && overlay.classList.contains('ytr-open')) {
                        overlay.classList.remove('ytr-open');
                        return;
                    }
                }
                if (e.ctrlKey && e.altKey && (e.key === 'e' || e.key === 'E')) {
                    e.preventDefault();
                    toggleEqualizerModal();
                    return;
                }
                if ((e.ctrlKey && e.key === 'k') || (e.key === '/' && !isEditing)) {
                    e.preventDefault();
                    focusSearchBox();
                    return;
                }
                if ((e.code === 'Space' || e.key === ' ') && !isEditing && smoothAudioEnabled) {
                    e.preventDefault();
                    smoothTogglePlayPause();
                    return;
                }
            }, true);

            document.addEventListener('click', (e) => {
                if (isActionBypassed || !smoothAudioEnabled) return;
                const path = e.composedPath();
                const isPlayBtn = path.some(el => el instanceof HTMLElement && (
                    el.id === 'play-pause-button' || el.classList?.contains('play-pause-button')
                ));
                if (isPlayBtn) {
                    e.preventDefault();
                    e.stopImmediatePropagation();
                    smoothTogglePlayPause();
                    return;
                }
                const isNextBtn = path.some(el => el instanceof HTMLElement && (
                    el.classList?.contains('next-button') || el.id === 'next-button'
                ));
                if (isNextBtn) {
                    const video = document.querySelector('video');
                    if (video && !video.paused) {
                        e.preventDefault();
                        e.stopImmediatePropagation();
                        smoothNextTrack();
                        return;
                    }
                }
                const isPrevBtn = path.some(el => el instanceof HTMLElement && (
                    el.classList?.contains('previous-button') || el.id === 'previous-button'
                ));
                if (isPrevBtn) {
                    const video = document.querySelector('video');
                    if (video && !video.paused) {
                        e.preventDefault();
                        e.stopImmediatePropagation();
                        smoothPrevTrack();
                        return;
                    }
                }
            }, true);
        } catch (_) {}
    }

    window.__ytr_init = initBridge;

    if (document.readyState === 'loading') {
        document.addEventListener('DOMContentLoaded', initBridge);
    } else {
        initBridge();
    }
})();
)JS";

    m_bridgeScript = script;

    auto addHandler = new CoreAddScriptCompletedHandler(
        [](HRESULT hr, LPCWSTR id) -> HRESULT {
            if (FAILED(hr)) {
                LogBridge(L"AddScriptToExecuteOnDocumentCreated FAILED: hr=" + std::to_wstring(hr));
            } else {
                LogBridge(L"AddScriptToExecuteOnDocumentCreated SUCCEEDED: id=" + std::wstring(id ? id : L"none"));
            }
            return S_OK;
        }
    );
    HRESULT hr = m_webview->AddScriptToExecuteOnDocumentCreated(script.c_str(), addHandler);
    if (FAILED(hr)) {
        LogBridge(L"AddScriptToExecuteOnDocumentCreated returned error: " + std::to_wstring(hr));
    }
}



void WebViewEngine::Resize(int width, int height) {
    if (m_controller) {
        RECT bounds = { 0, 0, width, height };
        m_controller->put_Bounds(bounds);
    }
}

void WebViewEngine::SetVisible(bool visible) {
    if (m_controller) {
        m_controller->put_IsVisible(visible ? TRUE : FALSE);
    }
}

void WebViewEngine::ExecuteScript(const std::wstring& script) {
    if (m_webview) {
        auto execHandler = new CoreExecuteScriptCompletedHandler(
            [](HRESULT hr, LPCWSTR res) -> HRESULT {
                if (FAILED(hr)) {
                    LogBridge(L"ExecuteScript FAILED: hr=" + std::to_wstring(hr));
                }
                return S_OK;
            }
        );
        m_webview->ExecuteScript(script.c_str(), execHandler);
    }
}

void WebViewEngine::Navigate(const std::wstring& url) {
    if (m_webview) {
        m_webview->Navigate(url.c_str());
    }
}

void WebViewEngine::SendControl(const std::wstring& action) {
    if (action == L"playPause") {
        ExecuteScript(LR"JS(
            (function() {
                if (typeof window.__ytr_smoothToggle === 'function') {
                    window.__ytr_smoothToggle();
                    return;
                }
                const btn = document.querySelector('#play-pause-button, .play-pause-button');
                if (btn) {
                    btn.click();
                    return;
                }
                const mp = document.querySelector('#movie_player');
                if (mp && typeof mp.getPlayerState === 'function') {
                    if (mp.getPlayerState() === 1) {
                        if (typeof mp.pauseVideo === 'function') { mp.pauseVideo(); return; }
                    } else {
                        if (typeof mp.playVideo === 'function') { mp.playVideo(); return; }
                    }
                }
                const v = document.querySelector('video');
                if (v) {
                    v.paused ? v.play() : v.pause();
                }
            })();
        )JS");
    } else if (action == L"next") {
        ExecuteScript(LR"JS(
            (function() {
                if (typeof window.__ytr_smoothNext === 'function') {
                    window.__ytr_smoothNext();
                    return;
                }
                const btn = document.querySelector('.next-button.ytmusic-player-bar, #next-button, button.next-button');
                if (btn) { btn.click(); return; }
                const mp = document.querySelector('#movie_player');
                if (mp && typeof mp.nextVideo === 'function') { mp.nextVideo(); return; }
            })();
        )JS");
    } else if (action == L"previous") {
        ExecuteScript(LR"JS(
            (function() {
                if (typeof window.__ytr_smoothPrev === 'function') {
                    window.__ytr_smoothPrev();
                    return;
                }
                const btn = document.querySelector('.previous-button.ytmusic-player-bar, #previous-button, button.previous-button');
                if (btn) { btn.click(); return; }
                const mp = document.querySelector('#movie_player');
                if (mp && typeof mp.previousVideo === 'function') { mp.previousVideo(); return; }
            })();
        )JS");
    } else if (action == L"like") {
        ExecuteScript(LR"JS(
            (function() {
                const btn = document.querySelector('#like-button-renderer yt-button-shape button, .ytmusic-like-button-renderer button, ytmusic-like-button-renderer tp-yt-paper-icon-button');
                if (btn) btn.click();
            })();
        )JS");
    } else if (action == L"toggleEqualizer" || action == L"toggleConsistency") {
        ExecuteScript(LR"JS(
            (function() {
                if (typeof window.__ytr_toggleEqualizer === 'function') {
                    window.__ytr_toggleEqualizer();
                }
            })();
        )JS");
    }
}

void WebViewEngine::SeekTo(double seconds) {
    std::wstringstream ss;
    ss << L"const v = document.querySelector('video'); if (v) { v.currentTime = " << seconds << L"; }";
    ExecuteScript(ss.str());
}

void WebViewEngine::SetVolume(int volumePercent) {
    std::wstringstream ss;
    ss << L"(function() { "
          L"const mp = document.querySelector('#movie_player'); "
          L"if (mp && typeof mp.setVolume === 'function') { mp.setVolume(" << volumePercent << L"); } "
          L"else { const v = document.querySelector('video'); if (v) { v.volume = Math.pow(" << (volumePercent / 100.0) << L", 2); } } "
          L"})();";
    ExecuteScript(ss.str());
}

static std::wstring ExtractJsonField(const std::wstring& json, const std::wstring& key) {
    std::wstring search = L"\"" + key + L"\":\"";
    size_t start = json.find(search);
    if (start != std::wstring::npos) {
        start += search.length();
        size_t end = json.find(L"\"", start);
        if (end != std::wstring::npos) {
            return json.substr(start, end - start);
        }
    }
    return L"";
}

static double ExtractJsonNumber(const std::wstring& json, const std::wstring& key) {
    std::wstring search = L"\"" + key + L"\":";
    size_t start = json.find(search);
    if (start != std::wstring::npos) {
        start += search.length();
        while (start < json.length() && (json[start] == L' ' || json[start] == L'\t')) start++;
        size_t end = start;
        while (end < json.length() && ((json[end] >= L'0' && json[end] <= L'9') || json[end] == L'.' || json[end] == L'-')) end++;
        if (end > start) {
            try {
                return std::stod(json.substr(start, end - start));
            } catch (...) {}
        }
    }
    return 0.0;
}

static bool ExtractJsonBool(const std::wstring& json, const std::wstring& key) {
    std::wstring search = L"\"" + key + L"\":";
    size_t start = json.find(search);
    if (start != std::wstring::npos) {
        start += search.length();
        while (start < json.length() && (json[start] == L' ' || json[start] == L'\t')) start++;
        if (json.substr(start, 4) == L"true") return true;
        if (json.substr(start, 5) == L"false") return false;
    }
    return false;
}

void WebViewEngine::HandleWebMessage(const std::wstring& rawJson) {
    std::wstring unquoted = rawJson;
    if (unquoted.size() >= 2 && unquoted.front() == L'"' && unquoted.back() == L'"') {
        // Unescape JSON string if it was wrapped in outer string quotes
        std::wstring parsed;
        for (size_t i = 1; i + 1 < unquoted.size(); ++i) {
            if (unquoted[i] == L'\\' && i + 1 < unquoted.size()) {
                if (unquoted[i + 1] == L'"') { parsed += L'"'; i++; continue; }
                if (unquoted[i + 1] == L'\\') { parsed += L'\\'; i++; continue; }
                if (unquoted[i + 1] == L'n') { parsed += L'\n'; i++; continue; }
            }
            parsed += unquoted[i];
        }
        unquoted = parsed;
    }

    std::wstring type = ExtractJsonField(unquoted, L"type");
    if (type == L"log") {
        std::wstring msg = ExtractJsonField(unquoted, L"message");
        LogBridge(L"[JS] " + msg);
        return;
    }
    if (type == L"enter_pip") {
        App_HideMainWindow();
        App_ToggleMiniplayer();
        App_TrimWorkingSet();
        return;
    }
    if (type == L"trim_memory") {
        App_TrimWorkingSet();
        return;
    }
    if (type == L"quit") {
        App_Quit();
        return;
    }
    if (type == L"state") {
        g_currentSong.title = ExtractJsonField(unquoted, L"title");
        g_currentSong.artist = ExtractJsonField(unquoted, L"artist");
        g_currentSong.artworkUrl = ExtractJsonField(unquoted, L"artwork");
        g_currentSong.isPaused = ExtractJsonBool(unquoted, L"paused");
        g_currentSong.currentTime = ExtractJsonNumber(unquoted, L"currentTime");
        g_currentSong.duration = ExtractJsonNumber(unquoted, L"duration");
        g_currentSong.volume = (int)ExtractJsonNumber(unquoted, L"volume");
        g_currentSong.isLiked = ExtractJsonBool(unquoted, L"isLiked");
        g_currentSong.isDisliked = ExtractJsonBool(unquoted, L"isDisliked");

        App_OnSongStateUpdated(g_currentSong);
    }
}
