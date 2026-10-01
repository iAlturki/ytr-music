#include "webview_engine.h"
#include "com_helper.h"
#include "miniplayer.h"
#include "../res/resource.h"
#include <shlobj.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <vector>

namespace {

const wchar_t kHomeUrl[] = L"https://music.youtube.com";
const LONGLONG kMaxLogBytes = 1024 * 1024;
const int kMaxEnvRetries = 5;
const int kMaxRecoveries = 3;
const ULONGLONG kRecoveryWindowMs = 5 * 60 * 1000;
const ULONGLONG kUnresponsiveReloadMs = 10 * 1000;
const ULONGLONG kUnresponsiveStaleMs = 60 * 1000;
const DWORD kLoaderLoadFlags = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32;

// The subdomain wildcards also cover googleads.g.doubleclick.net and pagead2/tpc.googlesyndication.com.
const wchar_t* const kAdFilters[] = {
    L"*://*.doubleclick.net/*",
    L"*://*.googlesyndication.com/*",
    L"*://www.youtube.com/pagead/*",
    L"*://music.youtube.com/pagead/*",
    L"*://music.youtube.com/youtubei/v1/player/ad_break*",
    L"*://www.youtube.com/youtubei/v1/player/ad_break*",
    L"*://music.youtube.com/api/stats/ads*",
    L"*://www.youtube.com/api/stats/ads*",
    L"*://adservice.google.*/*",
};

std::wstring ResolveUserDataFolder() {
    WCHAR path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, path))) {
        return std::wstring(path) + L"\\ytr-music-native";
    }
    // LoadLibraryExW with LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR needs an absolute path.
    DWORD len = GetFullPathNameW(L"ytr-music-native", MAX_PATH, path, nullptr);
    return (len > 0 && len < MAX_PATH) ? std::wstring(path) : std::wstring(L".\\ytr-music-native");
}

// Read by the runtime while the environment and controller are created, then cleared so
// processes started later (ShellExecute) do not inherit them.
void SetCreationEnvironment(bool set) {
    if (!set) {
        SetEnvironmentVariableW(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", nullptr);
        SetEnvironmentVariableW(L"WEBVIEW2_DEFAULT_BACKGROUND_COLOR", nullptr);
        return;
    }
    std::wstring args =
        L"--disable-features=CalculateNativeWinOcclusion,SpareRendererForSitePerProcess "
        L"--enable-gpu-rasterization --enable-zero-copy";
    if (App_IsDebugMode()) {
        args += L" --remote-debugging-port=9222";
    }
    SetEnvironmentVariableW(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", args.c_str());
    // Unlike put_DefaultBackgroundColor, this also covers the controller's very first frame.
    SetEnvironmentVariableW(L"WEBVIEW2_DEFAULT_BACKGROUND_COLOR", L"FF030303");
}

UINT32 Fnv1a(const BYTE* data, DWORD size) {
    UINT32 hash = 2166136261u;
    for (DWORD i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= 16777619u;
    }
    return hash;
}

bool FileHasContent(const std::wstring& path, const BYTE* data, DWORD size) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    bool same = false;
    LARGE_INTEGER fileSize;
    if (GetFileSizeEx(file, &fileSize) && fileSize.QuadPart == (LONGLONG)size) {
        std::vector<BYTE> buffer(size);
        DWORD read = 0;
        same = ReadFile(file, buffer.data(), size, &read, nullptr) && read == size &&
               memcmp(buffer.data(), data, size) == 0;
    }
    CloseHandle(file);
    return same;
}

bool WriteFileReplacing(const std::wstring& path, const BYTE* data, DWORD size) {
    std::wstring tmp = path + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    HANDLE file = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(file, data, size, &written, nullptr) && written == size;
    CloseHandle(file);
    ok = ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

void RemoveOtherLoaders(const std::wstring& folder, const wchar_t* keepName) {
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((folder + L"\\WebView2Loader*.dll").c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && _wcsicmp(fd.cFileName, keepName) != 0) {
            DeleteFileW((folder + L"\\" + fd.cFileName).c_str());
        }
    } while (FindNextFileW(find, &fd));
    FindClose(find);
}

// Never loads by bare name: that would also probe the current directory and PATH.
HMODULE LoadWebView2Loader(const std::wstring& userDataFolder) {
    WCHAR exePath[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        WCHAR* slash = wcsrchr(exePath, L'\\');
        if (slash) {
            slash[1] = L'\0';
            std::wstring path = std::wstring(exePath) + L"WebView2Loader.dll";
            HMODULE module = LoadLibraryExW(path.c_str(), nullptr, kLoaderLoadFlags);
            if (module) return module;
        }
    }

    HMODULE self = GetModuleHandleW(nullptr);
    HRSRC res = FindResourceW(self, MAKEINTRESOURCEW(IDR_WEBVIEW2_LOADER), MAKEINTRESOURCEW(10));
    DWORD size = res ? SizeofResource(self, res) : 0;
    HGLOBAL handle = size ? LoadResource(self, res) : nullptr;
    const BYTE* data = handle ? (const BYTE*)LockResource(handle) : nullptr;
    if (!data) return nullptr;

    // Named after the embedded bytes, so a newer build never reuses an older extracted copy.
    WCHAR name[48];
    swprintf_s(name, L"WebView2Loader-%08x.dll", (unsigned)Fnv1a(data, size));
    std::wstring path = userDataFolder + L"\\" + name;
    bool extracted = false;
    bool verified = FileHasContent(path, data, size);
    if (!verified) {
        verified = extracted = WriteFileReplacing(path, data, size);
    }
    if (verified) {
        HMODULE module = LoadLibraryExW(path.c_str(), nullptr, kLoaderLoadFlags);
        if (module) {
            if (extracted) RemoveOtherLoaders(userDataFolder, name);
            return module;
        }
    }
    // Copy extracted by older builds; only used when the profile folder is not writable.
    std::wstring legacy = userDataFolder + L"\\WebView2Loader.dll";
    return LoadLibraryExW(legacy.c_str(), nullptr, kLoaderLoadFlags);
}

std::wstring LoadUtf8Resource(int id) {
    HMODULE hMod = GetModuleHandleW(NULL);
    HRSRC hRes = FindResourceW(hMod, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));
    if (!hRes) return std::wstring();
    HGLOBAL hData = LoadResource(hMod, hRes);
    DWORD size = SizeofResource(hMod, hRes);
    const char* data = hData ? (const char*)LockResource(hData) : nullptr;
    if (!data || size == 0) return std::wstring();
    if (size >= 3 && (BYTE)data[0] == 0xEF && (BYTE)data[1] == 0xBB && (BYTE)data[2] == 0xBF) {
        data += 3;
        size -= 3;
    }
    int len = MultiByteToWideChar(CP_UTF8, 0, data, (int)size, NULL, 0);
    if (len <= 0) return std::wstring();
    std::wstring out(len, L' ');
    MultiByteToWideChar(CP_UTF8, 0, data, (int)size, &out[0], len);
    return out;
}

bool IsHeaderValue(const wchar_t* value) {
    return value && *value && !wcspbrk(value, L"\r\n");
}

void AppendEchoedHeader(ICoreWebView2HttpRequestHeaders* request, const wchar_t* requestName,
                        const wchar_t* responseName, std::wstring& headers) {
    LPWSTR value = nullptr;
    if (SUCCEEDED(request->GetHeader(requestName, &value)) && IsHeaderValue(value)) {
        headers += L"\r\n";
        headers += responseName;
        headers += L": ";
        headers += value;
    }
    CoTaskMemFree(value);
}

// ACAO:* is rejected for credentialed requests, so echo the caller's origin when it is
// visible: every blocked cross-origin call then sees a clean empty 204 instead of a CORS error.
std::wstring BlockedResponseHeaders(ICoreWebView2WebResourceRequestedEventArgs* args) {
    std::wstring headers = L"Access-Control-Allow-Origin: *\r\n"
                           L"Access-Control-Allow-Methods: GET, POST, OPTIONS, HEAD\r\n"
                           L"Access-Control-Allow-Headers: *";
    ICoreWebView2WebResourceRequest* request = nullptr;
    ICoreWebView2HttpRequestHeaders* requestHeaders = nullptr;
    if (SUCCEEDED(args->get_Request(&request)) && request &&
        SUCCEEDED(request->get_Headers(&requestHeaders)) && requestHeaders) {
        LPWSTR origin = nullptr;
        if (SUCCEEDED(requestHeaders->GetHeader(L"Origin", &origin)) && IsHeaderValue(origin)) {
            headers = L"Access-Control-Allow-Origin: ";
            headers += origin;
            headers += L"\r\nAccess-Control-Allow-Credentials: true\r\nVary: Origin";
            AppendEchoedHeader(requestHeaders, L"Access-Control-Request-Method", L"Access-Control-Allow-Methods", headers);
            AppendEchoedHeader(requestHeaders, L"Access-Control-Request-Headers", L"Access-Control-Allow-Headers", headers);
        }
        CoTaskMemFree(origin);
    }
    if (requestHeaders) requestHeaders->Release();
    if (request) request->Release();
    return headers;
}

bool IsWebUrl(const std::wstring& url) {
    return _wcsnicmp(url.c_str(), L"https://", 8) == 0 || _wcsnicmp(url.c_str(), L"http://", 7) == 0;
}

bool HostEquals(const std::wstring& url, const wchar_t* host) {
    WCHAR buffer[256];
    DWORD len = ARRAYSIZE(buffer);
    return SUCCEEDED(UrlGetPartW(url.c_str(), buffer, &len, URL_PART_HOSTNAME, 0)) && _wcsicmp(buffer, host) == 0;
}

// --- Page message decoding (one flat JSON object per message) ---

struct BridgeMessage {
    std::wstring type;
    std::wstring message;
    std::wstring title;
    std::wstring artist;
    std::wstring artwork;
    double currentTime = 0.0;
    double duration = 0.0;
    double volume = 0.0;
    bool paused = false;
    bool hasPaused = false;
    bool isLiked = false;
    bool isDisliked = false;
};

void SkipSpace(const wchar_t*& p) {
    while (*p == L' ' || *p == L'\t' || *p == L'\n' || *p == L'\r') ++p;
}

int HexDigit(wchar_t c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}

bool ReadHex4(const wchar_t* p, unsigned& out) {
    unsigned value = 0;
    for (int i = 0; i < 4; ++i) {
        int digit = HexDigit(p[i]);
        if (digit < 0) return false;
        value = (value << 4) | (unsigned)digit;
    }
    out = value;
    return true;
}

// p must point at the opening quote and is left just past the closing one. out may be null.
bool ParseJsonString(const wchar_t*& p, std::wstring* out) {
    if (*p != L'"') return false;
    ++p;
    for (;;) {
        wchar_t c = *p;
        if (c == L'\0') return false;
        ++p;
        if (c == L'"') return true;
        if (c != L'\\') {
            if (out) out->push_back(c);
            continue;
        }
        wchar_t e = *p;
        if (e == L'\0') return false;
        ++p;
        wchar_t decoded = e;  // \" \\ \/ (and unknown escapes) map to themselves
        switch (e) {
            case L'b': decoded = L'\b'; break;
            case L'f': decoded = L'\f'; break;
            case L'n': decoded = L'\n'; break;
            case L'r': decoded = L'\r'; break;
            case L't': decoded = L'\t'; break;
            case L'u': {
                unsigned cp = 0;
                if (!ReadHex4(p, cp)) return false;
                p += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    unsigned low = 0;
                    if (p[0] == L'\\' && p[1] == L'u' && ReadHex4(p + 2, low) && low >= 0xDC00 && low <= 0xDFFF) {
                        p += 6;
                        if (out) {
                            out->push_back((wchar_t)cp);
                            out->push_back((wchar_t)low);
                        }
                        continue;
                    }
                    decoded = L'\xFFFD';
                } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    decoded = L'\xFFFD';
                } else {
                    decoded = (wchar_t)cp;
                }
                break;
            }
            default:
                break;
        }
        if (out) out->push_back(decoded);
    }
}

// Locale-independent; non-finite results become 0.
bool ParseJsonNumber(const wchar_t*& p, double& out) {
    const wchar_t* start = p;
    bool negative = false;
    if (*p == L'-' || *p == L'+') {
        negative = (*p == L'-');
        ++p;
    }
    double mantissa = 0.0;
    int exponent = 0;
    bool digits = false;
    for (; *p >= L'0' && *p <= L'9'; ++p) {
        digits = true;
        if (mantissa < 1e17) mantissa = mantissa * 10.0 + (*p - L'0');
        else ++exponent;
    }
    if (*p == L'.') {
        ++p;
        for (; *p >= L'0' && *p <= L'9'; ++p) {
            digits = true;
            if (mantissa < 1e17) {
                mantissa = mantissa * 10.0 + (*p - L'0');
                --exponent;
            }
        }
    }
    if (!digits) {
        p = start;
        return false;
    }
    if (*p == L'e' || *p == L'E') {
        const wchar_t* e = p + 1;
        bool expNegative = false;
        if (*e == L'-' || *e == L'+') {
            expNegative = (*e == L'-');
            ++e;
        }
        if (*e >= L'0' && *e <= L'9') {
            int value = 0;
            for (; *e >= L'0' && *e <= L'9'; ++e) {
                if (value < 100000) value = value * 10 + (*e - L'0');
            }
            exponent += expNegative ? -value : value;
            p = e;
        }
    }
    double result = mantissa;
    if (exponent > 0) result *= std::pow(10.0, exponent);
    else if (exponent < 0) result /= std::pow(10.0, -exponent);
    if (negative) result = -result;
    out = std::isfinite(result) ? result : 0.0;
    return true;
}

bool ReadLiteral(const wchar_t*& p, const wchar_t* word, size_t len) {
    if (wcsncmp(p, word, len) != 0) return false;
    p += len;
    return true;
}

bool SkipJsonComposite(const wchar_t*& p) {
    int depth = 0;
    while (*p) {
        wchar_t c = *p;
        if (c == L'"') {
            if (!ParseJsonString(p, nullptr)) return false;
            continue;
        }
        ++p;
        if (c == L'{' || c == L'[') {
            ++depth;
        } else if (c == L'}' || c == L']') {
            if (--depth == 0) return true;
        }
    }
    return false;
}

std::wstring* StringField(BridgeMessage& m, const std::wstring& key) {
    if (key == L"type") return &m.type;
    if (key == L"title") return &m.title;
    if (key == L"artist") return &m.artist;
    if (key == L"artwork") return &m.artwork;
    if (key == L"message") return &m.message;
    return nullptr;
}

void SetBoolField(BridgeMessage& m, const std::wstring& key, bool value) {
    if (key == L"paused") {
        m.paused = value;
        m.hasPaused = true;
    } else if (key == L"isLiked") {
        m.isLiked = value;
    } else if (key == L"isDisliked") {
        m.isDisliked = value;
    }
}

void SetNumberField(BridgeMessage& m, const std::wstring& key, double value) {
    if (key == L"currentTime") m.currentTime = value;
    else if (key == L"duration") m.duration = value;
    else if (key == L"volume") m.volume = value;
}

bool ParseBridgeMessage(const wchar_t* p, BridgeMessage& m) {
    SkipSpace(p);
    if (*p != L'{') return false;
    ++p;
    SkipSpace(p);
    if (*p == L'}') return true;
    std::wstring key;
    for (;;) {
        SkipSpace(p);
        key.clear();
        if (!ParseJsonString(p, &key)) return false;
        SkipSpace(p);
        if (*p != L':') return false;
        ++p;
        SkipSpace(p);
        if (*p == L'"') {
            std::wstring* target = StringField(m, key);
            if (target) target->clear();
            if (!ParseJsonString(p, target)) return false;
        } else if (*p == L'{' || *p == L'[') {
            if (!SkipJsonComposite(p)) return false;
        } else if (ReadLiteral(p, L"true", 4)) {
            SetBoolField(m, key, true);
        } else if (ReadLiteral(p, L"false", 5)) {
            SetBoolField(m, key, false);
        } else if (!ReadLiteral(p, L"null", 4)) {
            double value = 0.0;
            if (!ParseJsonNumber(p, value)) return false;
            SetNumberField(m, key, value);
        }
        SkipSpace(p);
        if (*p == L',') {
            ++p;
            continue;
        }
        return *p == L'}';
    }
}

} // namespace

WebViewEngine& WebViewEngine::Instance() {
    static WebViewEngine instance;
    return instance;
}

WebViewEngine::WebViewEngine() {}

WebViewEngine::~WebViewEngine() {
    Shutdown();
}

void LogBridge(const std::wstring& text) {
    if (!App_IsDebugMode()) return;
    static std::wstring s_logPath;
    static LONGLONG s_logBytes = -1;
    if (s_logBytes < 0) {
        s_logBytes = 0;
        std::wstring dir = ResolveUserDataFolder();
        CreateDirectoryW(dir.c_str(), NULL);
        s_logPath = dir + L"\\debug.log";
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (GetFileAttributesExW(s_logPath.c_str(), GetFileExInfoStandard, &fa)) {
            s_logBytes = ((LONGLONG)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
        }
    }
    const wchar_t* mode = L"a, ccs=UTF-8";
    if (s_logBytes > kMaxLogBytes) {
        std::wstring previous = s_logPath + L".1";
        if (!MoveFileExW(s_logPath.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            mode = L"w, ccs=UTF-8";
        }
        s_logBytes = 0;
    }
    FILE* fp = _wfopen(s_logPath.c_str(), mode);
    if (!fp) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    int written = fwprintf(fp, L"[%02d:%02d:%02d.%03d] %ls\n", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, text.c_str());
    fclose(fp);
    if (written > 0) s_logBytes += written;
}

bool WebViewEngine::Initialize(HWND hWndContainer, std::function<void()> onInitialized) {
    m_hWndContainer = hWndContainer;
    m_onInitialized = std::move(onInitialized);
    m_shuttingDown = false;

    LogBridge(L"=== WebViewEngine::Initialize called ===");

    m_userDataFolder = ResolveUserDataFolder();
    CreateDirectoryW(m_userDataFolder.c_str(), NULL);

    HMODULE hLoader = LoadWebView2Loader(m_userDataFolder);
    if (!hLoader) {
        MessageBoxW(hWndContainer, L"Could not load WebView2Loader.dll!", L"ytr-music", MB_ICONERROR);
        return false;
    }

    m_pfnCreateEnv = reinterpret_cast<CreateEnvFn>(reinterpret_cast<void (*)()>(
        GetProcAddress(hLoader, "CreateCoreWebView2EnvironmentWithOptions")));
    if (!m_pfnCreateEnv) {
        MessageBoxW(hWndContainer, L"WebView2 entry point not found!", L"ytr-music Native", MB_ICONERROR);
        return false;
    }

    return CreateEnvironment();
}

bool WebViewEngine::CreateEnvironment() {
    if (!m_pfnCreateEnv || m_shuttingDown) return false;

    SetCreationEnvironment(true);
    const unsigned attempt = ++m_envAttempt;
    m_envCallbackDone = false;

    auto envHandler = new CoreEnvCompletedHandler(
        [this, attempt](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
            if (attempt != m_envAttempt || m_envCallbackDone || m_shuttingDown) return S_OK;
            m_envCallbackDone = true;
            OnEnvironmentCreated(hr, env);
            return S_OK;
        }
    );
    HRESULT hr = m_pfnCreateEnv(nullptr, m_userDataFolder.c_str(), nullptr, envHandler);
    envHandler->Release();

    if (FAILED(hr) && attempt == m_envAttempt && !m_envCallbackDone) {
        m_envCallbackDone = true;
        return OnEnvironmentFailed(hr);
    }
    return SUCCEEDED(hr);
}

bool WebViewEngine::OnEnvironmentFailed(HRESULT hr) {
    // A browser process left over from a run with different switches still owns the
    // profile; it exits within a few seconds.
    if (hr == HRESULT_FROM_WIN32(ERROR_INVALID_STATE) && m_envRetries < kMaxEnvRetries &&
        RunDeferred(1000, [this]() { CreateEnvironment(); })) {
        ++m_envRetries;
        LogBridge(L"CoreWebView2Environment busy, retrying");
        return true;
    }
    SetCreationEnvironment(false);
    WCHAR errBuf[128];
    swprintf_s(errBuf, L"CreateCoreWebView2Environment failed! hr=0x%08X", (UINT)hr);
    LogBridge(errBuf);
    MessageBoxW(m_hWndContainer, L"Failed to create CoreWebView2Environment!", L"ytr-music Native", MB_ICONERROR);
    return false;
}

void WebViewEngine::OnEnvironmentCreated(HRESULT hr, ICoreWebView2Environment* env) {
    if (FAILED(hr) || !env) {
        OnEnvironmentFailed(FAILED(hr) ? hr : E_FAIL);
        return;
    }
    m_envRetries = 0;
    if (m_environment) m_environment->Release();
    m_environment = env;
    m_environment->AddRef();

    const unsigned attempt = m_envAttempt;
    auto controllerHandler = new CoreControllerCompletedHandler(
        [this, attempt](HRESULT hr2, ICoreWebView2Controller* controller) -> HRESULT {
            if (attempt != m_envAttempt || m_shuttingDown || !m_environment) {
                if (controller) controller->Close();
                return S_OK;
            }
            OnControllerCreated(hr2, controller);
            return S_OK;
        }
    );
    HRESULT hrCreate = m_environment->CreateCoreWebView2Controller(m_hWndContainer, controllerHandler);
    controllerHandler->Release();
    if (FAILED(hrCreate)) {
        SetCreationEnvironment(false);
        LogBridge(L"CreateCoreWebView2Controller returned error: " + std::to_wstring(hrCreate));
    }
}

void WebViewEngine::OnControllerCreated(HRESULT hr, ICoreWebView2Controller* controller) {
    SetCreationEnvironment(false);
    if (FAILED(hr) || !controller) {
        WCHAR errBuf[256];
        swprintf_s(errBuf, L"CreateCoreWebView2Controller failed! hr=0x%08X", (UINT)hr);
        LogBridge(errBuf);
        MessageBoxW(m_hWndContainer, L"Failed to create CoreWebView2Controller!", L"ytr-music Native", MB_ICONERROR);
        return;
    }
    m_controller = controller;
    m_controller->AddRef();

    if (FAILED(m_controller->get_CoreWebView2(&m_webview)) || !m_webview) {
        LogBridge(L"get_CoreWebView2 failed");
        return;
    }

    ICoreWebView2Controller2* controller2 = nullptr;
    if (SUCCEEDED(m_controller->QueryInterface(IID_ICoreWebView2Controller2, (void**)&controller2)) && controller2) {
        COREWEBVIEW2_COLOR background = { 255, 3, 3, 3 };
        controller2->put_DefaultBackgroundColor(background);
        controller2->Release();
    }

    RECT bounds;
    GetClientRect(m_hWndContainer, &bounds);
    if (bounds.right > 0 && bounds.bottom > 0) {
        m_controller->put_Bounds(bounds);
    }
    // Start visible: a hidden frame that never played media defers media loads, so the
    // page is only hidden after its first playback (see ApplyVisibility).
    m_controller->put_IsVisible(TRUE);
    m_appliedVisible = true;
    m_hasPlayedMedia = false;
    ApplyVisibility();

    ApplySettings();
    RegisterEventHandlers();
    SetupInjectedBridge();
    AddAdBlockFilters();

    m_firstNavDone = false;
    m_appliedMemLevel = 0;
    ApplyMemoryTarget();

    m_webview->Navigate(kHomeUrl);

    m_isReady = true;
    if (m_onInitialized) {
        m_onInitialized();
    }
}

void WebViewEngine::ApplySettings() {
    const BOOL debug = App_IsDebugMode() ? TRUE : FALSE;
    ICoreWebView2Settings* settings = nullptr;
    if (FAILED(m_webview->get_Settings(&settings)) || !settings) return;

    settings->put_IsScriptEnabled(TRUE);
    settings->put_AreDefaultScriptDialogsEnabled(TRUE);
    settings->put_IsWebMessageEnabled(TRUE);
    settings->put_AreDevToolsEnabled(debug);
    settings->put_AreDefaultContextMenusEnabled(debug);
    settings->put_IsStatusBarEnabled(FALSE);

    ICoreWebView2Settings4* settings4 = nullptr;
    if (SUCCEEDED(settings->QueryInterface(IID_ICoreWebView2Settings4, (void**)&settings4)) && settings4) {
        settings4->put_IsPasswordAutosaveEnabled(FALSE);
        settings4->put_IsGeneralAutofillEnabled(FALSE);
        settings4->Release();
    }

    ICoreWebView2Settings5* settings5 = nullptr;
    if (SUCCEEDED(settings->QueryInterface(IID_ICoreWebView2Settings5, (void**)&settings5)) && settings5) {
        settings5->put_IsPinchZoomEnabled(FALSE);
        settings5->Release();
    }

    ICoreWebView2Settings6* settings6 = nullptr;
    if (SUCCEEDED(settings->QueryInterface(IID_ICoreWebView2Settings6, (void**)&settings6)) && settings6) {
        settings6->put_IsSwipeNavigationEnabled(FALSE);
        settings6->Release();
    }

    settings->Release();
}

void WebViewEngine::RegisterEventHandlers() {
    auto msgHandler = new CoreWebMessageReceivedHandler(
        [this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
            // Only YouTube Music may drive the host (quit, PiP, now-playing state); the main frame
            // can also show the Google sign-in page or a site reached through a link.
            LPWSTR source = nullptr;
            const bool trusted = SUCCEEDED(args->get_Source(&source)) && source &&
                _wcsnicmp(source, L"https://", 8) == 0 && HostEquals(source, L"music.youtube.com");
            CoTaskMemFree(source);
            if (!trusted) return S_OK;

            // The page posts JSON.stringify(...) strings; reading them as a string avoids
            // a second layer of JSON escaping.
            LPWSTR raw = nullptr;
            if (SUCCEEDED(args->TryGetWebMessageAsString(&raw)) && raw) {
                HandleWebMessage(raw);
            } else {
                CoTaskMemFree(raw);
                raw = nullptr;
                if (SUCCEEDED(args->get_WebMessageAsJson(&raw)) && raw) {
                    if (raw[0] == L'"') {
                        std::wstring inner;
                        const wchar_t* p = raw;
                        if (ParseJsonString(p, &inner)) HandleWebMessage(inner.c_str());
                    } else {
                        HandleWebMessage(raw);
                    }
                }
            }
            CoTaskMemFree(raw);
            return S_OK;
        }
    );
    m_webview->add_WebMessageReceived(msgHandler, &m_msgToken);
    msgHandler->Release();

    auto contentLoadingHandler = new CoreContentLoadingHandler(
        [this](ICoreWebView2*, ICoreWebView2ContentLoadingEventArgs*) -> HRESULT {
            // A new document has not played media yet. ContentLoading (not NavigationStarting)
            // fires only once it commits, so states still posted by the outgoing playing
            // document cannot re-arm the gate; it is not raised for pushState navigations.
            m_unresponsiveSince = 0;
            if (m_hasPlayedMedia) {
                m_hasPlayedMedia = false;
                ApplyVisibility();
            }
            return S_OK;
        }
    );
    m_webview->add_ContentLoading(contentLoadingHandler, &m_contentLoadingToken);
    contentLoadingHandler->Release();

    auto navDoneHandler = new CoreNavigationCompletedHandler(
        [this](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
            m_unresponsiveSince = 0;
            if (!m_firstNavDone) {
                m_firstNavDone = true;
                ApplyMemoryTarget();
            }
            BOOL success = FALSE;
            if (args && SUCCEEDED(args->get_IsSuccess(&success)) && success) {
                RunScript(L"if (typeof window.__ytr_recheck === 'function') { window.__ytr_recheck(); }");
            }
            return S_OK;
        }
    );
    m_webview->add_NavigationCompleted(navDoneHandler, &m_navDoneToken);
    navDoneHandler->Release();

    auto failHandler = new CoreProcessFailedHandler(
        [this](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* args) -> HRESULT {
            OnProcessFailed(args);
            return S_OK;
        }
    );
    m_webview->add_ProcessFailed(failHandler, &m_failToken);
    failHandler->Release();

    auto newWindowHandler = new CoreNewWindowRequestedHandler(
        [this](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
            OnNewWindowRequested(args);
            return S_OK;
        }
    );
    m_webview->add_NewWindowRequested(newWindowHandler, &m_newWindowToken);
    newWindowHandler->Release();
}

void WebViewEngine::AddAdBlockFilters() {
    for (const wchar_t* filter : kAdFilters) {
        m_webview->AddWebResourceRequestedFilter(filter, COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
    }

    auto resHandler = new CoreWebResourceRequestedHandler(
        [this](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
            if (!m_environment) return S_OK;
            std::wstring headers = BlockedResponseHeaders(args);
            ICoreWebView2WebResourceResponse* response = nullptr;
            m_environment->CreateWebResourceResponse(nullptr, 204, L"No Content", headers.c_str(), &response);
            if (response) {
                args->put_Response(response);
                response->Release();
            }
            return S_OK;
        }
    );
    m_webview->add_WebResourceRequested(resHandler, &m_resToken);
    resHandler->Release();
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
            } else if (App_IsDebugMode()) {
                LogBridge(L"AddScriptToExecuteOnDocumentCreated SUCCEEDED: id=" + std::wstring(id ? id : L"none"));
            }
            return S_OK;
        }
    );
    HRESULT hr = m_webview->AddScriptToExecuteOnDocumentCreated(script.c_str(), addHandler);
    addHandler->Release();
    if (FAILED(hr)) {
        LogBridge(L"AddScriptToExecuteOnDocumentCreated returned error: " + std::to_wstring(hr));
    }
}

void WebViewEngine::OnProcessFailed(ICoreWebView2ProcessFailedEventArgs* args) {
    COREWEBVIEW2_PROCESS_FAILED_KIND kind;
    if (!args || FAILED(args->get_ProcessFailedKind(&kind))) return;
    LogBridge(L"ProcessFailed kind=" + std::to_wstring((int)kind));

    switch (kind) {
        case COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED:
            ScheduleRecovery(true);
            break;
        case COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED:
            m_unresponsiveSince = 0;
            ScheduleRecovery(false);
            break;
        case COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE: {
            // Raised repeatedly while the page stays hung; one report alone can be a long
            // GC or a busy machine, so only reload once the hang has lasted a while. A hidden,
            // paused page posts nothing, so an old first report must expire instead of
            // turning a later one-off report into an immediate reload.
            ULONGLONG now = GetTickCount64();
            if (!m_unresponsiveSince || now - m_unresponsiveSince > kUnresponsiveStaleMs) {
                m_unresponsiveSince = now;
            } else if (now - m_unresponsiveSince >= kUnresponsiveReloadMs) {
                m_unresponsiveSince = 0;
                ScheduleRecovery(false);
            }
            break;
        }
        default:
            // GPU, utility and iframe processes are restarted by the runtime itself.
            break;
    }
}

void WebViewEngine::ScheduleRecovery(bool recreateEnvironment) {
    if (m_shuttingDown) return;
    if (m_pendingRecovery) {
        if (recreateEnvironment) m_pendingRecovery = 2;
        return;
    }
    ULONGLONG now = GetTickCount64();
    if (m_recoveryCount == 0 || now - m_recoveryWindowStart > kRecoveryWindowMs) {
        m_recoveryWindowStart = now;
        m_recoveryCount = 0;
    }
    if (m_recoveryCount >= kMaxRecoveries) {
        LogBridge(L"WebView recovery limit reached");
        return;
    }
    const UINT delayMs = 1000u << m_recoveryCount;
    const bool scheduled = RunDeferred(delayMs, [this]() {
        const int pending = m_pendingRecovery;
        m_pendingRecovery = 0;
        if (m_shuttingDown) return;
        if (pending == 2) {
            // Never tear the WebView down inside its own event; this runs from a timer.
            LogBridge(L"Recreating WebView after browser process exit");
            ReleaseWebView();
            CreateEnvironment();
        } else if (m_webview) {
            LogBridge(L"Reloading page after renderer failure");
            m_webview->Reload();
        }
    });
    // Without a timer nothing would ever clear m_pendingRecovery and block later recoveries.
    if (!scheduled) return;
    ++m_recoveryCount;
    m_pendingRecovery = recreateEnvironment ? 2 : 1;
}

void WebViewEngine::OnNewWindowRequested(ICoreWebView2NewWindowRequestedEventArgs* args) {
    if (!args) return;
    BOOL userInitiated = FALSE;
    args->get_IsUserInitiated(&userInitiated);
    LPWSTR rawUri = nullptr;
    args->get_Uri(&rawUri);
    std::wstring uri = rawUri ? rawUri : L"";
    CoTaskMemFree(rawUri);

    const bool web = IsWebUrl(uri);
    // Google sign-in popups keep the runtime's default window so window.opener flows work.
    if (userInitiated && web && HostEquals(uri, L"accounts.google.com")) return;

    args->put_Handled(TRUE);
    if (!userInitiated || !web) return;
    RunDeferred(0, [uri]() {
        ShellExecuteW(nullptr, L"open", uri.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    });
}

void WebViewEngine::ReleaseWebView() {
    m_isReady = false;
    if (m_webview) {
        m_webview->remove_WebMessageReceived(m_msgToken);
        m_webview->remove_ContentLoading(m_contentLoadingToken);
        m_webview->remove_NavigationCompleted(m_navDoneToken);
        m_webview->remove_WebResourceRequested(m_resToken);
        m_webview->remove_ProcessFailed(m_failToken);
        m_webview->remove_NewWindowRequested(m_newWindowToken);
    }
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
    m_appliedVisible = true;
    m_hasPlayedMedia = false;
    m_firstNavDone = false;
    m_appliedMemLevel = 0;
    m_unresponsiveSince = 0;
}

void WebViewEngine::Shutdown() {
    m_shuttingDown = true;
    CancelDeferred();
    m_pendingRecovery = 0;
    ReleaseWebView();
    if (m_execHandler) {
        m_execHandler->Release();
        m_execHandler = nullptr;
    }
}

bool WebViewEngine::RunDeferred(UINT delayMs, std::function<void()> task) {
    UINT_PTR id = SetTimer(nullptr, 0, delayMs, DeferredTimerProc);
    if (!id) return false;
    m_deferred[id] = std::move(task);
    return true;
}

void WebViewEngine::CancelDeferred() {
    for (const auto& entry : m_deferred) {
        KillTimer(nullptr, entry.first);
    }
    m_deferred.clear();
}

void CALLBACK WebViewEngine::DeferredTimerProc(HWND, UINT, UINT_PTR id, DWORD) {
    KillTimer(nullptr, id);
    WebViewEngine& self = Instance();
    auto it = self.m_deferred.find(id);
    if (it == self.m_deferred.end()) return;
    std::function<void()> task = std::move(it->second);
    self.m_deferred.erase(it);
    if (task) task();
}

void WebViewEngine::Resize(int width, int height) {
    // Minimized windows report 0x0; laying the page out at that size is pure waste.
    if (!m_controller || width <= 0 || height <= 0) return;
    RECT bounds = { 0, 0, width, height };
    m_controller->put_Bounds(bounds);
}

void WebViewEngine::SetVisible(bool visible) {
    m_wantVisible = visible;
    ApplyVisibility();
}

void WebViewEngine::ApplyVisibility() {
    if (!m_controller) return;
    const bool visible = m_wantVisible || !m_hasPlayedMedia;
    if (visible == m_appliedVisible) return;
    if (SUCCEEDED(m_controller->put_IsVisible(visible ? TRUE : FALSE))) {
        m_appliedVisible = visible;
    }
}

void WebViewEngine::SetMemoryLow(bool low) {
    m_wantMemLow = low;
    ApplyMemoryTarget();
}

void WebViewEngine::ApplyMemoryTarget() {
    if (!m_webview) return;
    // LOW during the initial page load would only slow the load down.
    if (m_wantMemLow && !m_firstNavDone) return;
    const int level = m_wantMemLow ? 1 : 0;
    if (level == m_appliedMemLevel) return;
    ICoreWebView2_19* webview19 = nullptr;
    if (SUCCEEDED(m_webview->QueryInterface(IID_ICoreWebView2_19, (void**)&webview19)) && webview19) {
        HRESULT hr = webview19->put_MemoryUsageTargetLevel(level
            ? COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_LOW
            : COREWEBVIEW2_MEMORY_USAGE_TARGET_LEVEL_NORMAL);
        if (SUCCEEDED(hr)) m_appliedMemLevel = level;
        webview19->Release();
    }
}

void WebViewEngine::ExecuteScript(const std::wstring& script) {
    RunScript(script.c_str());
}

void WebViewEngine::RunScript(const wchar_t* script) {
    if (!m_webview) return;
    if (!m_execHandler) {
        // One shared completion handler; WebView2 holds its own reference per pending call.
        m_execHandler = new CoreExecuteScriptCompletedHandler(
            [](HRESULT hr, LPCWSTR) -> HRESULT {
                if (FAILED(hr)) {
                    LogBridge(L"ExecuteScript FAILED: hr=" + std::to_wstring(hr));
                }
                return S_OK;
            }
        );
    }
    m_webview->ExecuteScript(script, m_execHandler);
}

void WebViewEngine::Navigate(const std::wstring& url) {
    if (m_webview) {
        m_webview->Navigate(url.c_str());
    }
}

void WebViewEngine::SendControl(const std::wstring& action) {
    if (action == L"playPause") {
        RunScript(LR"JS(
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
        RunScript(LR"JS(
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
        RunScript(LR"JS(
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
        RunScript(LR"JS(
            (function() {
                const btn = document.querySelector('#like-button-renderer yt-button-shape button, .ytmusic-like-button-renderer button, ytmusic-like-button-renderer tp-yt-paper-icon-button');
                if (btn) btn.click();
            })();
        )JS");
    } else if (action == L"toggleEqualizer" || action == L"toggleConsistency") {
        RunScript(LR"JS(
            (function() {
                if (typeof window.__ytr_toggleEqualizer === 'function') {
                    window.__ytr_toggleEqualizer();
                }
            })();
        )JS");
    }
}

// Numbers are formatted as integers so the generated JS never depends on the C locale.
void WebViewEngine::SeekTo(double seconds) {
    if (!m_webview) return;
    if (!(seconds >= 0.0)) seconds = 0.0;  // also catches NaN
    if (seconds > 2000000.0) seconds = 2000000.0;
    const int ms = (int)(seconds * 1000.0 + 0.5);
    wchar_t buf[384];
    swprintf_s(buf,
        L"(function(){const t=%d/1000;const mp=document.querySelector('#movie_player');"
        L"if(mp&&typeof mp.seekTo==='function'){mp.seekTo(t,true);}"
        L"else{const v=document.querySelector('video');if(v){v.currentTime=t;}}})();",
        ms);
    RunScript(buf);
}

void WebViewEngine::SetVolume(int volumePercent) {
    if (!m_webview) return;
    if (volumePercent < 0) volumePercent = 0;
    else if (volumePercent > 100) volumePercent = 100;
    // Raising the volume also lifts a user mute, but never the mute the ad blocker
    // applies while an ad plays.
    wchar_t buf[1024];
    swprintf_s(buf,
        L"(function(){const p=%d;"
        L"if(typeof window.__ytr_setVolume==='function'){window.__ytr_setVolume(p);return;}"
        L"const mp=document.querySelector('#movie_player');const v=document.querySelector('video');"
        L"let ad=!!document.querySelector('#movie_player.ad-showing,#movie_player.ad-interrupting,#movie_player[ad-interrupting]')"
        L"||!!(v&&v.style.opacity==='0');"
        L"try{if(!ad&&mp&&typeof mp.getAdState==='function'&&mp.getAdState()>0)ad=true;}catch(e){}"
        L"if(mp&&typeof mp.setVolume==='function'){mp.setVolume(p);"
        L"if(p>0&&!ad&&typeof mp.isMuted==='function'&&mp.isMuted()&&typeof mp.unMute==='function')mp.unMute();}"
        L"else if(v){v.volume=Math.pow(p/100,2);if(p>0&&!ad&&v.muted)v.muted=false;}})();",
        volumePercent);
    m_sentVolume = volumePercent;
    m_sentVolumeAt = GetTickCount64();
    RunScript(buf);
}

void WebViewEngine::HandleWebMessage(const wchar_t* json) {
    // Any message proves the renderer is alive.
    m_unresponsiveSince = 0;

    BridgeMessage msg;
    if (!json || !ParseBridgeMessage(json, msg)) return;

    if (msg.type == L"state") {
        double volume = msg.volume < 0.0 ? 0.0 : (msg.volume > 100.0 ? 100.0 : msg.volume);
        g_currentSong.title = std::move(msg.title);
        g_currentSong.artist = std::move(msg.artist);
        g_currentSong.artworkUrl = std::move(msg.artwork);
        g_currentSong.isPaused = msg.paused;
        g_currentSong.currentTime = msg.currentTime > 0.0 ? msg.currentTime : 0.0;
        g_currentSong.duration = msg.duration > 0.0 ? msg.duration : 0.0;
        const int pageVolume = (int)(volume + 0.5);
        // A stale volumechange posted before our SetVolume landed would snap the
        // miniplayer slider back mid-drag.
        if (m_sentVolume < 0 || pageVolume == m_sentVolume || GetTickCount64() - m_sentVolumeAt > 400) {
            g_currentSong.volume = pageVolume;
            if (pageVolume == m_sentVolume) m_sentVolume = -1;
        }
        g_currentSong.isLiked = msg.isLiked;
        g_currentSong.isDisliked = msg.isDisliked;

        // Use the page-reported flag: the miniplayer flips g_currentSong.isPaused optimistically.
        if (msg.hasPaused) {
            if (!msg.paused && msg.currentTime > 0.0 && !m_hasPlayedMedia) {
                m_hasPlayedMedia = true;
                ApplyVisibility();
            }
            const int paused = msg.paused ? 1 : 0;
            if (paused != m_lastPagePaused) {
                m_lastPagePaused = paused;
                App_OnPagePlaybackChanged(msg.paused);
            }
        }

        App_OnSongStateUpdated(g_currentSong);
        return;
    }
    if (msg.type == L"log") {
        if (App_IsDebugMode()) {
            LogBridge(L"[JS] " + msg.message);
        }
        return;
    }
    if (msg.type == L"enter_pip") {
        App_HideMainWindow();
        MiniplayerWindow& miniplayer = MiniplayerWindow::Instance();
        if (!miniplayer.IsVisible()) {
            miniplayer.Show();
        }
        return;
    }
    if (msg.type == L"quit") {
        App_Quit();
    }
}
