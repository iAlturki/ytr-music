#include "webview_engine.h"
#include "com_helper.h"
#include "../res/resource.h"
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
                            ExecuteScript(L"if (typeof window.__ytr_recheck === 'function') { window.__ytr_recheck(); }");
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

static std::wstring LoadUtf8Resource(int id) {
    HMODULE hMod = GetModuleHandleW(NULL);
    HRSRC hRes = FindResourceW(hMod, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));
    if (!hRes) return std::wstring();
    HGLOBAL hData = LoadResource(hMod, hRes);
    DWORD size = SizeofResource(hMod, hRes);
    const char* data = hData ? (const char*)LockResource(hData) : nullptr;
    if (!data || size == 0) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, data, (int)size, NULL, 0);
    std::wstring out(len, L' ');
    MultiByteToWideChar(CP_UTF8, 0, data, (int)size, &out[0], len);
    return out;
}

void WebViewEngine::SetupInjectedBridge() {
    if (!m_webview) return;

    // The page bridge (ad-blocking, top bar, audio graph, state reporter) lives in
    // src/bridge.js and is embedded as a UTF-8 RCDATA resource.
    std::wstring script = LoadUtf8Resource(IDR_BRIDGE_JS);
    if (script.empty()) {
        LogBridge(L"Bridge script resource missing");
        return;
    }

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
                if (typeof window.__ytr_playerPlayPause === 'function') {
                    window.__ytr_playerPlayPause();
                    return;
                }
                const mp = document.querySelector('#movie_player');
                if (mp && typeof mp.getPlayerState === 'function') {
                    if (mp.getPlayerState() === 1) mp.pauseVideo();
                    else mp.playVideo();
                    return;
                }
                const v = document.querySelector('video');
                if (v) v.paused ? v.play() : v.pause();
            })();
        )JS");
    } else if (action == L"next") {
        ExecuteScript(LR"JS(
            (function() {
                if (typeof window.__ytr_playerNext === 'function') {
                    window.__ytr_playerNext();
                    return;
                }
                const mp = document.querySelector('#movie_player');
                if (mp && typeof mp.nextVideo === 'function') { mp.nextVideo(); return; }
                const btn = document.querySelector('.next-button.ytmusic-player-bar, #next-button, button.next-button');
                if (btn) btn.click();
            })();
        )JS");
    } else if (action == L"previous") {
        ExecuteScript(LR"JS(
            (function() {
                if (typeof window.__ytr_playerPrev === 'function') {
                    window.__ytr_playerPrev();
                    return;
                }
                const mp = document.querySelector('#movie_player');
                if (mp && typeof mp.previousVideo === 'function') { mp.previousVideo(); return; }
                const btn = document.querySelector('.previous-button.ytmusic-player-bar, #previous-button, button.previous-button');
                if (btn) btn.click();
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
    ss << L"(function() { "
          L"const mp = document.querySelector('#movie_player'); "
          L"if (mp && typeof mp.seekTo === 'function') { mp.seekTo(" << seconds << L", true); } "
          L"else { const v = document.querySelector('video'); if (v) { v.currentTime = " << seconds << L"; } } "
          L"})();";
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
