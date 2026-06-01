/*
 * casc_ui_win.cpp — Windows WebView2 GUI backend
 *
 * Implements the casc_ui_backend_* hooks using the Microsoft Edge WebView2
 * runtime. Compiled only on Windows when CMake finds the WebView2 SDK and
 * defines CASC_UI_HAVE_WEBVIEW2; otherwise casc_ui_null.c is used.
 *
 * UI -> DSP: window.chrome.webview.postMessage(JSON.stringify({id,value}))
 *            arrives as a WebMessageReceived event carrying a JSON string.
 * DSP -> UI: casc_ui_backend_eval_js() calls ExecuteScript.
 *
 * parent_handle is interpreted as an HWND to host the WebView2 in. When NULL a
 * standalone top-level window is created.
 *
 * This file is C++ (WebView2 ships a COM/C++ API); the symbols it exports are
 * declared `extern "C"` so they satisfy the C declarations in casc_internal.h.
 */

#if defined(_WIN32) && defined(CASC_UI_HAVE_WEBVIEW2)

extern "C" {
#include "casc_internal.h"
}

#include <windows.h>
#include <wrl.h>
#include <wil/com.h>
#include <WebView2.h>
#include <string>
#include <cstdlib>

using namespace Microsoft::WRL;

/* Convert UTF-8 -> UTF-16 for the Win32/WebView2 wide APIs. */
static std::wstring widen(const char* s) {
    if (!s) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
    return w;
}
static std::string narrow(LPCWSTR w) {
    if (!w) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

/* Minimal JSON pull of {"id":N,"value":F} from the posted web message. */
static bool parse_id_value(const std::string& json, int* id, double* value) {
    auto find_num = [&](const char* key, double* out) -> bool {
        std::string k = std::string("\"") + key + "\"";
        size_t p = json.find(k);
        if (p == std::string::npos) return false;
        p = json.find(':', p + k.size());
        if (p == std::string::npos) return false;
        *out = std::strtod(json.c_str() + p + 1, nullptr);
        return true;
    };
    double fid = 0, fval = 0;
    if (!find_num("id", &fid)) return false;
    if (!find_num("value", &fval)) return false;
    *id = (int)fid;
    *value = fval;
    return true;
}

struct casc_ui_win_t {
    HWND                           hwnd       = nullptr;
    bool                           own_window = false;
    casc_ui_bridge_t*              bridge     = nullptr;
    wil::com_ptr<ICoreWebView2Controller> controller;
    wil::com_ptr<ICoreWebView2>           webview;
    std::wstring                   bootstrap;
    std::wstring                   html;
    bool                           ready      = false;
};

static const wchar_t* kWndClass = L"CascWebViewHost";

static LRESULT CALLBACK casc_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_SIZE) {
        casc_ui_win_t* st = (casc_ui_win_t*)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (st && st->controller) {
            RECT rc; GetClientRect(h, &rc);
            st->controller->put_Bounds(rc);
        }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

extern "C" casc_error_t casc_ui_backend_open(
        casc_instance_t* inst, void* parent_handle,
        const char* html, size_t html_len,
        const char* bootstrap_js, casc_ui_bridge_t* bridge,
        int width, int height, bool resizable, void** out_handle) {
    (void)inst; (void)html_len;
    if (out_handle) *out_handle = nullptr;

    casc_ui_win_t* st = new (std::nothrow) casc_ui_win_t();
    if (!st) return CASC_ERR_OUT_OF_MEMORY;
    st->bridge    = bridge;
    st->bootstrap = widen(bootstrap_js);
    st->html      = widen(html);

    if (parent_handle) {
        st->hwnd = (HWND)parent_handle;
    } else {
        static bool registered = false;
        if (!registered) {
            WNDCLASSW wc = {};
            wc.lpfnWndProc   = casc_wndproc;
            wc.hInstance     = GetModuleHandleW(nullptr);
            wc.lpszClassName = kWndClass;
            RegisterClassW(&wc);
            registered = true;
        }
        DWORD style = WS_OVERLAPPEDWINDOW;
        if (!resizable) style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
        st->hwnd = CreateWindowW(kWndClass, L"CASC Plugin", style,
                                 CW_USEDEFAULT, CW_USEDEFAULT, width, height,
                                 nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        st->own_window = true;
        ShowWindow(st->hwnd, SW_SHOW);
    }
    SetWindowLongPtrW(st->hwnd, GWLP_USERDATA, (LONG_PTR)st);

    /* Create the WebView2 environment, then the controller + view. The async
     * callbacks complete on the message loop; the host pumps it via tick(). */
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, nullptr, nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [st](HRESULT, ICoreWebView2Environment* env) -> HRESULT {
                env->CreateCoreWebView2Controller(st->hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [st](HRESULT, ICoreWebView2Controller* ctrl) -> HRESULT {
                            if (!ctrl) return S_OK;
                            st->controller = ctrl;
                            ctrl->get_CoreWebView2(&st->webview);

                            RECT rc; GetClientRect(st->hwnd, &rc);
                            ctrl->put_Bounds(rc);

                            /* Inject bootstrap before the page's scripts run. */
                            if (!st->bootstrap.empty())
                                st->webview->AddScriptToExecuteOnDocumentCreated(
                                    st->bootstrap.c_str(), nullptr);

                            /* UI -> DSP messages. */
                            EventRegistrationToken tok;
                            st->webview->add_WebMessageReceived(
                                Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [st](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                        wil::unique_cotaskmem_string raw;
                                        if (SUCCEEDED(args->TryGetWebMessageAsString(&raw)) && raw) {
                                            std::string s = narrow(raw.get());
                                            int id; double v;
                                            if (parse_id_value(s, &id, &v) && st->bridge)
                                                casc_ui_on_set_param(st->bridge, id, v);
                                        }
                                        return S_OK;
                                    }).Get(), &tok);

                            st->webview->NavigateToString(st->html.c_str());
                            st->ready = true;
                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());

    if (FAILED(hr)) {
        if (st->own_window && st->hwnd) DestroyWindow(st->hwnd);
        delete st;
        return CASC_ERR_IO;
    }

    if (out_handle) *out_handle = st;
    return CASC_OK;
}

extern "C" void casc_ui_backend_close(void* handle) {
    if (!handle) return;
    casc_ui_win_t* st = (casc_ui_win_t*)handle;
    if (st->controller) st->controller->Close();
    if (st->own_window && st->hwnd) DestroyWindow(st->hwnd);
    delete st;
}

extern "C" void casc_ui_backend_tick(void* handle) {
    (void)handle;
    /* Pump the Win32 message loop non-blockingly so WebView2 callbacks run. */
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

extern "C" void casc_ui_backend_eval_js(void* handle, const char* js) {
    if (!handle || !js) return;
    casc_ui_win_t* st = (casc_ui_win_t*)handle;
    if (st->webview && st->ready)
        st->webview->ExecuteScript(widen(js).c_str(), nullptr);
}

extern "C" casc_error_t casc_ui_backend_set_size(void* handle, int width, int height) {
    if (!handle) return CASC_ERR_INVALID_ARG;
    casc_ui_win_t* st = (casc_ui_win_t*)handle;
    if (st->own_window && st->hwnd)
        SetWindowPos(st->hwnd, nullptr, 0, 0, width, height,
                     SWP_NOMOVE | SWP_NOZORDER);
    if (st->controller) {
        RECT rc{0, 0, width, height};
        st->controller->put_Bounds(rc);
    }
    return CASC_OK;
}

#endif /* _WIN32 && CASC_UI_HAVE_WEBVIEW2 */
