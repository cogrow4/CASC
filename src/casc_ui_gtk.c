/*
 * casc_ui_gtk.c — Linux WebView GUI backend (WebKitGTK 6 / 4.1)
 *
 * Implements the casc_ui_backend_* hooks using a WebKitWebView embedded in a
 * GtkWidget. Compiled only on Linux when WebKitGTK + GTK are found by CMake
 * (which then defines CASC_UI_HAVE_WEBKITGTK); otherwise casc_ui_null.c is used.
 *
 * UI -> DSP: a script message handler named "casc" receives the JSON {id,value}
 *            posted by window.webkit.messageHandlers.casc.postMessage(...).
 * DSP -> UI: casc_ui_backend_eval_js() runs JS in the page.
 *
 * parent_handle is interpreted as a GtkWidget* container to add the web view
 * into. When NULL, a standalone GtkWindow is created.
 */

#include "casc_internal.h"

#if defined(__linux__) && defined(CASC_UI_HAVE_WEBKITGTK)

#include <gtk/gtk.h>
#include <webkit/webkit.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    GtkWidget*        window;     /* non-NULL only in standalone mode */
    GtkWidget*        webview;    /* WebKitWebView */
    casc_ui_bridge_t* bridge;
} casc_ui_gtk_t;

/* Called when the page posts to messageHandlers.casc. */
static void on_script_message(WebKitUserContentManager* mgr,
                              WebKitJavascriptResult* res,
                              gpointer user_data) {
    (void)mgr;
    casc_ui_gtk_t* st = (casc_ui_gtk_t*)user_data;
    if (!st || !st->bridge) return;

    JSCValue* value = webkit_javascript_result_get_js_value(res);
    if (!value || !jsc_value_is_object(value)) return;

    JSCValue* jid = jsc_value_object_get_property(value, "id");
    JSCValue* jval = jsc_value_object_get_property(value, "value");
    if (jid && jval) {
        int    pid = (int)jsc_value_to_int32(jid);
        double v   = jsc_value_to_double(jval);
        casc_ui_on_set_param(st->bridge, pid, v);
    }
    if (jid)  g_object_unref(jid);
    if (jval) g_object_unref(jval);
}

casc_error_t casc_ui_backend_open(casc_instance_t* inst, void* parent_handle,
                                   const char* html, size_t html_len,
                                   const char* bootstrap_js,
                                   casc_ui_bridge_t* bridge,
                                   int width, int height, bool resizable,
                                   void** out_handle) {
    (void)inst; (void)html_len;
    if (out_handle) *out_handle = NULL;

    if (!gtk_init_check()) return CASC_ERR_IO;

    casc_ui_gtk_t* st = (casc_ui_gtk_t*)calloc(1, sizeof(*st));
    if (!st) return CASC_ERR_OUT_OF_MEMORY;
    st->bridge = bridge;

    WebKitUserContentManager* ucm = webkit_user_content_manager_new();

    /* Register the "casc" script message handler. */
    webkit_user_content_manager_register_script_message_handler(ucm, "casc", NULL);
    g_signal_connect(ucm, "script-message-received::casc",
                     G_CALLBACK(on_script_message), st);

    /* Inject the bootstrap shim at document start. */
    if (bootstrap_js) {
        WebKitUserScript* script = webkit_user_script_new(
            bootstrap_js,
            WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
            WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
            NULL, NULL);
        webkit_user_content_manager_add_script(ucm, script);
        webkit_user_script_unref(script);
    }

    GtkWidget* webview = GTK_WIDGET(g_object_new(WEBKIT_TYPE_WEB_VIEW,
                                                 "user-content-manager", ucm,
                                                 NULL));
    st->webview = webview;
    gtk_widget_set_size_request(webview, width, height);

    webkit_web_view_load_html(WEBKIT_WEB_VIEW(webview),
                              html ? html : "", NULL);

    if (parent_handle) {
        GtkWidget* parent = (GtkWidget*)parent_handle;
        /* For a GtkBox/GtkWindow container, append the web view. */
        if (GTK_IS_WINDOW(parent)) {
            gtk_window_set_child(GTK_WINDOW(parent), webview);
        } else if (GTK_IS_BOX(parent)) {
            gtk_box_append(GTK_BOX(parent), webview);
        } else {
            gtk_widget_set_parent(webview, parent);
        }
    } else {
        GtkWidget* win = gtk_window_new();
        gtk_window_set_default_size(GTK_WINDOW(win), width, height);
        gtk_window_set_resizable(GTK_WINDOW(win), resizable ? TRUE : FALSE);
        gtk_window_set_child(GTK_WINDOW(win), webview);
        gtk_window_present(GTK_WINDOW(win));
        st->window = win;
    }

    if (out_handle) *out_handle = st;
    return CASC_OK;
}

void casc_ui_backend_close(void* handle) {
    if (!handle) return;
    casc_ui_gtk_t* st = (casc_ui_gtk_t*)handle;
    if (st->window) {
        gtk_window_destroy(GTK_WINDOW(st->window));
    } else if (st->webview) {
        gtk_widget_unparent(st->webview);
    }
    free(st);
}

void casc_ui_backend_tick(void* handle) {
    (void)handle;
    /* Pump pending GTK events without blocking. */
    while (g_main_context_pending(NULL))
        g_main_context_iteration(NULL, FALSE);
}

void casc_ui_backend_eval_js(void* handle, const char* js) {
    if (!handle || !js) return;
    casc_ui_gtk_t* st = (casc_ui_gtk_t*)handle;
    if (!st->webview) return;
    webkit_web_view_evaluate_javascript(WEBKIT_WEB_VIEW(st->webview),
                                        js, -1, NULL, NULL, NULL, NULL, NULL);
}

casc_error_t casc_ui_backend_set_size(void* handle, int width, int height) {
    if (!handle) return CASC_ERR_INVALID_ARG;
    casc_ui_gtk_t* st = (casc_ui_gtk_t*)handle;
    if (st->window)
        gtk_window_set_default_size(GTK_WINDOW(st->window), width, height);
    if (st->webview)
        gtk_widget_set_size_request(st->webview, width, height);
    return CASC_OK;
}

#endif /* __linux__ && CASC_UI_HAVE_WEBKITGTK */
