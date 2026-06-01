/*
 * casc_ui_null.c — Fallback GUI backend
 *
 * Compiled when no native WebView backend is available for the target platform
 * (or when CASC_DISABLE_UI is set). Every hook is a safe no-op; open() reports
 * CASC_ERR_IO so the host runtime falls back to its generic parameter panel.
 *
 * This guarantees libcasc always links and casc_open_ui() degrades gracefully
 * rather than failing to build.
 */

#include "casc_internal.h"

#if !defined(__APPLE__) || defined(CASC_DISABLE_UI)

casc_error_t casc_ui_backend_open(casc_instance_t* inst, void* parent_handle,
                                   const char* html, size_t html_len,
                                   const char* bootstrap_js,
                                   casc_ui_bridge_t* bridge,
                                   int width, int height, bool resizable,
                                   void** out_handle) {
    (void)inst; (void)parent_handle; (void)html; (void)html_len;
    (void)bootstrap_js; (void)bridge; (void)width; (void)height; (void)resizable;
    if (out_handle) *out_handle = NULL;
    return CASC_ERR_IO; /* no GUI backend on this platform */
}

void casc_ui_backend_close(void* handle) { (void)handle; }
void casc_ui_backend_tick(void* handle) { (void)handle; }
void casc_ui_backend_eval_js(void* handle, const char* js) { (void)handle; (void)js; }

casc_error_t casc_ui_backend_set_size(void* handle, int width, int height) {
    (void)handle; (void)width; (void)height;
    return CASC_ERR_IO;
}

#endif /* !__APPLE__ || CASC_DISABLE_UI */
