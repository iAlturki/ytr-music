#pragma once

#include "app.h"
#include <WebView2.h>
#include <string>
#include <functional>

class WebViewEngine {
public:
    static WebViewEngine& Instance();

    bool Initialize(HWND hWndContainer, std::function<void()> onInitialized = nullptr);
    void Resize(int width, int height);
    void SetVisible(bool visible);
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

    void SetupInjectedBridge();
    void HandleWebMessage(const std::wstring& message);

    HWND m_hWndContainer = nullptr;
    ICoreWebView2Environment* m_environment = nullptr;
    ICoreWebView2Controller* m_controller = nullptr;
    ICoreWebView2* m_webview = nullptr;
    bool m_isReady = false;
    std::function<void()> m_onInitialized;
};
