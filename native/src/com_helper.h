#pragma once

#include <unknwn.h>
#include <functional>
#include <utility>
#include <WebView2.h>

// WebView2 callback object wrapping a std::function. It starts with one reference
// owned by the creator: pass it to the API (which AddRefs what it keeps), then Release().
template <typename Interface, const IID& Iid, typename... Args>
class CoreCallback final : public Interface {
    LONG m_refCount = 1;
    std::function<HRESULT(Args...)> m_func;
public:
    explicit CoreCallback(std::function<HRESULT(Args...)> func)
        : m_func(std::move(func)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == Iid) {
            *ppv = static_cast<Interface*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_refCount); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG count = InterlockedDecrement(&m_refCount);
        if (count == 0) delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE Invoke(Args... args) override {
        return m_func ? m_func(args...) : S_OK;
    }
};

using CoreEnvCompletedHandler = CoreCallback<
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
    IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler,
    HRESULT, ICoreWebView2Environment*>;

using CoreControllerCompletedHandler = CoreCallback<
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
    IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler,
    HRESULT, ICoreWebView2Controller*>;

using CoreExecuteScriptCompletedHandler = CoreCallback<
    ICoreWebView2ExecuteScriptCompletedHandler,
    IID_ICoreWebView2ExecuteScriptCompletedHandler,
    HRESULT, LPCWSTR>;

using CoreAddScriptCompletedHandler = CoreCallback<
    ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler,
    IID_ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler,
    HRESULT, LPCWSTR>;

using CoreWebMessageReceivedHandler = CoreCallback<
    ICoreWebView2WebMessageReceivedEventHandler,
    IID_ICoreWebView2WebMessageReceivedEventHandler,
    ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs*>;

using CoreWebResourceRequestedHandler = CoreCallback<
    ICoreWebView2WebResourceRequestedEventHandler,
    IID_ICoreWebView2WebResourceRequestedEventHandler,
    ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs*>;

using CoreContentLoadingHandler = CoreCallback<
    ICoreWebView2ContentLoadingEventHandler,
    IID_ICoreWebView2ContentLoadingEventHandler,
    ICoreWebView2*, ICoreWebView2ContentLoadingEventArgs*>;

using CoreNavigationCompletedHandler = CoreCallback<
    ICoreWebView2NavigationCompletedEventHandler,
    IID_ICoreWebView2NavigationCompletedEventHandler,
    ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*>;

using CoreProcessFailedHandler = CoreCallback<
    ICoreWebView2ProcessFailedEventHandler,
    IID_ICoreWebView2ProcessFailedEventHandler,
    ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs*>;

using CoreNewWindowRequestedHandler = CoreCallback<
    ICoreWebView2NewWindowRequestedEventHandler,
    IID_ICoreWebView2NewWindowRequestedEventHandler,
    ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs*>;
