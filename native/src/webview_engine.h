#pragma once

#include "app.h"
#include <WebView2.h>
#include <string>
#include <functional>
#include <map>

class WebViewEngine {
public:
    static WebViewEngine& Instance();

    bool Initialize(HWND hWndContainer, std::function<void()> onInitialized = nullptr);
    void Resize(int width, int height);
    // Host window shown/hidden. The page is only hidden from Chromium after it has
    // played media once (hidden never-played frames defer media loads).
    void SetVisible(bool visible);
    // MemoryUsageTargetLevel LOW/NORMAL (ICoreWebView2_19); cached until ready.
    void SetMemoryLow(bool low);
    // Release controller/webview/environment. Must run before CoUninitialize.
    void Shutdown();
    void ExecuteScript(const std::wstring& script);
    void Navigate(const std::wstring& url);

    void SendControl(const std::wstring& action);
    void SeekTo(double seconds);
    void SetVolume(int volumePercent);

    ICoreWebView2Controller* GetController() const { return m_controller; }
    ICoreWebView2* GetWebView() const { return m_webview; }
    bool IsReady() const { return m_isReady; }

private:
    WebViewEngine();
    ~WebViewEngine();

    using CreateEnvFn = HRESULT(STDAPICALLTYPE*)(
        PCWSTR browserExecutableFolder,
        PCWSTR userDataFolder,
        ICoreWebView2EnvironmentOptions* environmentOptions,
        ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler* environmentCreatedHandler);

    bool CreateEnvironment();
    void OnEnvironmentCreated(HRESULT hr, ICoreWebView2Environment* env);
    bool OnEnvironmentFailed(HRESULT hr);
    void OnControllerCreated(HRESULT hr, ICoreWebView2Controller* controller);
    void ApplySettings();
    void RegisterEventHandlers();
    void AddAdBlockFilters();
    void SetupInjectedBridge();
    void HandleWebMessage(const wchar_t* json);
    void OnProcessFailed(ICoreWebView2ProcessFailedEventArgs* args);
    void OnNewWindowRequested(ICoreWebView2NewWindowRequestedEventArgs* args);
    void ScheduleRecovery(bool recreateEnvironment);
    void ReleaseWebView();
    void ApplyVisibility();
    void ApplyMemoryTarget();
    void RunScript(const wchar_t* script);
    bool RunDeferred(UINT delayMs, std::function<void()> task);
    void CancelDeferred();
    static void CALLBACK DeferredTimerProc(HWND, UINT, UINT_PTR id, DWORD);

    HWND m_hWndContainer = nullptr;
    CreateEnvFn m_pfnCreateEnv = nullptr;
    std::wstring m_userDataFolder;
    ICoreWebView2Environment* m_environment = nullptr;
    ICoreWebView2Controller* m_controller = nullptr;
    ICoreWebView2* m_webview = nullptr;
    ICoreWebView2ExecuteScriptCompletedHandler* m_execHandler = nullptr;
    EventRegistrationToken m_msgToken = {};
    EventRegistrationToken m_contentLoadingToken = {};
    EventRegistrationToken m_navDoneToken = {};
    EventRegistrationToken m_resToken = {};
    EventRegistrationToken m_failToken = {};
    EventRegistrationToken m_newWindowToken = {};
    bool m_isReady = false;
    bool m_shuttingDown = false;
    std::function<void()> m_onInitialized;

    unsigned m_envAttempt = 0;
    bool m_envCallbackDone = false;
    int m_envRetries = 0;

    bool m_wantVisible = true;
    bool m_appliedVisible = true;
    bool m_hasPlayedMedia = false;

    bool m_wantMemLow = false;
    bool m_firstNavDone = false;
    int m_appliedMemLevel = 0;

    int m_lastPagePaused = -1;

    // Volume the host just requested; page echoes that disagree are ignored briefly.
    int m_sentVolume = -1;
    ULONGLONG m_sentVolumeAt = 0;

    ULONGLONG m_unresponsiveSince = 0;
    ULONGLONG m_recoveryWindowStart = 0;
    int m_recoveryCount = 0;
    int m_pendingRecovery = 0;

    std::map<UINT_PTR, std::function<void()>> m_deferred;
};
