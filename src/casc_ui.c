/*
 * casc_ui.c — Portable GUI host logic for libcasc
 *
 * Owns the JS<->DSP bridge protocol and drives the platform WebView backend
 * (casc_ui_mac.m / casc_ui_win.c / casc_ui_gtk.c / casc_ui_null.c).
 *
 * Protocol (injected into the page as window.casc before page scripts run):
 *
 *   window.casc.setParam(id, value)   UI -> DSP. The backend forwards the call
 *                                     to casc_ui_on_set_param(), which sets the
 *                                     DSP param and notifies the host callback.
 *   window.casc.getParam(id) -> value Returns the cached value (kept in sync by
 *                                     the bootstrap shim + DSP->UI pushes).
 *   window.casc.subscribe(cb)         cb(id, value) is invoked when the host
 *                                     pushes a parameter change via
 *                                     casc_ui_notify_param() (automation, presets).
 *
 * The same ui.html therefore works unchanged in a DAW (real window.casc) and
 * standalone in a browser (the page's own no-op fallback shim).
 */

#include "casc_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/*  Bridge object — shared between casc_ui.c and the platform backend         */
/* -------------------------------------------------------------------------- */

struct casc_ui_bridge {
    casc_instance_t* inst;     /* owning instance */
};

/* Called by the platform backend when the page invokes window.casc.setParam. */
void casc_ui_on_set_param(casc_ui_bridge_t* bridge, int param_id, double value) {
    if (!bridge || !bridge->inst) return;
    casc_instance_t* inst = bridge->inst;

    /* Clamp to the normalised range the protocol guarantees. */
    if (value < 0.0) value = 0.0;
    if (value > 1.0) value = 1.0;

    /* Drive the DSP. */
    casc_set_param(inst, param_id, value);

    /* Let the host record automation / refresh its own param view. */
    if (inst->ui_param_cb)
        inst->ui_param_cb(inst->ui_param_cb_user, param_id, value);
}

/* -------------------------------------------------------------------------- */
/*  Bootstrap JS — defines window.casc before the page's scripts run          */
/* -------------------------------------------------------------------------- */
/*
 * The platform backend exposes a single host function the shim can call to send
 * a setParam message:
 *   - macOS:   window.webkit.messageHandlers.casc.postMessage({id, value})
 *   - Windows: window.chrome.webview.postMessage(JSON.stringify({id, value}))
 *   - Linux:   window.webkit.messageHandlers.casc... (WebKitGTK script-message)
 * We normalise all three behind window.__casc_post(msg).
 */
char* casc_ui_make_bootstrap_js(casc_instance_t* inst) {
    /* Seed the cached parameter snapshot with current DSP values so getParam()
     * is correct on first paint. */
    char* seed = NULL;
    size_t seed_cap = 0, seed_len = 0;

    /* Build "{\"0\":0.5,\"1\":0.3,...}" */
    int pcount = inst && inst->plugin ? inst->plugin->manifest.param_count : 0;

    /* Grow buffer helper (rough upper bound: 32 chars per param). */
    seed_cap = (size_t)pcount * 32 + 8;
    seed = (char*)malloc(seed_cap);
    if (!seed) return NULL;
    seed[0] = '{';
    seed_len = 1;
    for (int i = 0; i < pcount; i++) {
        const casc_manifest_param_t* mp = &inst->plugin->manifest.params[i];
        double v = casc_get_param(inst, mp->id);
        int n = snprintf(seed + seed_len, seed_cap - seed_len,
                         "%s\"%d\":%.6f", (i ? "," : ""), mp->id, v);
        if (n < 0 || (size_t)n >= seed_cap - seed_len) break;
        seed_len += (size_t)n;
    }
    if (seed_len < seed_cap - 1) seed[seed_len++] = '}';
    seed[seed_len] = '\0';

    static const char* tmpl =
        "(function(){\n"
        "  if (window.casc && window.casc.__casc_native) return;\n"
        "  var seed = %s;\n"
        "  function post(msg){\n"
        "    try {\n"
        "      if (window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.casc) {\n"
        "        window.webkit.messageHandlers.casc.postMessage(msg); return; }\n"
        "      if (window.chrome && window.chrome.webview) {\n"
        "        window.chrome.webview.postMessage(JSON.stringify(msg)); return; }\n"
        "    } catch(e){}\n"
        "  }\n"
        "  var subs = [];\n"
        "  window.casc = {\n"
        "    __casc_native: true,\n"
        "    _values: seed,\n"
        "    setParam: function(id, value){\n"
        "      value = Math.max(0, Math.min(1, +value));\n"
        "      this._values[id] = value;\n"
        "      post({id:(id|0), value:value});\n"
        "    },\n"
        "    getParam: function(id){\n"
        "      var v = this._values[id]; return (v===undefined?0:v);\n"
        "    },\n"
        "    subscribe: function(cb){ if(typeof cb==='function') subs.push(cb); },\n"
        "    /* Called by the host (libcasc) to push DSP->UI updates. */\n"
        "    _hostSet: function(id, value){\n"
        "      this._values[id] = value;\n"
        "      for (var i=0;i<subs.length;i++){ try{ subs[i](id, value); }catch(e){} }\n"
        "    }\n"
        "  };\n"
        "})();\n";

    size_t need = strlen(tmpl) + seed_len + 8;
    char* js = (char*)malloc(need);
    if (!js) { free(seed); return NULL; }
    snprintf(js, need, tmpl, seed);
    free(seed);
    return js;
}

/* -------------------------------------------------------------------------- */
/*  Public API: GUI manifest queries                                          */
/* -------------------------------------------------------------------------- */

int casc_plugin_has_ui(const casc_plugin_t* p) {
    if (!p) return 0;
    if (!p->manifest.gui.present) return 0;
    /* Only HTML GUIs are hostable today. */
    return (strcmp(p->manifest.gui.type, "html") == 0) ? 1 : 0;
}

casc_ui_type_t casc_plugin_get_ui_type(const casc_plugin_t* p) {
    if (!p || !p->manifest.gui.present) return CASC_UI_NONE;
    if (strcmp(p->manifest.gui.type, "html") == 0) return CASC_UI_HTML;
    if (strcmp(p->manifest.gui.type, "wasm") == 0) return CASC_UI_WASM;
    return CASC_UI_NONE;
}

int casc_plugin_get_ui_width(const casc_plugin_t* p) {
    return (p && p->manifest.gui.present) ? p->manifest.gui.width : 0;
}

int casc_plugin_get_ui_height(const casc_plugin_t* p) {
    return (p && p->manifest.gui.present) ? p->manifest.gui.height : 0;
}

int casc_plugin_get_ui_resizable(const casc_plugin_t* p) {
    return (p && p->manifest.gui.present && p->manifest.gui.resizable) ? 1 : 0;
}

/* -------------------------------------------------------------------------- */
/*  Public API: UI lifecycle                                                  */
/* -------------------------------------------------------------------------- */

void casc_set_ui_param_callback(casc_instance_t* inst,
                                 casc_ui_param_cb cb, void* user_data) {
    if (!inst) return;
    inst->ui_param_cb = cb;
    inst->ui_param_cb_user = user_data;
}

/* Internal per-GUI handle: ties the platform handle to the bridge. */
typedef struct {
    void*             backend;   /* opaque handle from casc_ui_backend_open */
    casc_ui_bridge_t  bridge;
} casc_ui_state_t;

/* Lazily extract ui.html from the archive and cache it on the plugin. */
static const char* ensure_ui_html(casc_plugin_t* p, size_t* out_len) {
    if (!p) return NULL;
    if (!p->ui_html_checked) {
        p->ui_html_checked = true;
        const char* entry = p->manifest.gui.entry[0] ? p->manifest.gui.entry
                                                       : "ui.html";
        size_t len = 0;
        p->ui_html = (char*)casc_loader_extract_entry(p->path, entry, &len);
        p->ui_html_len = len;
    }
    if (out_len) *out_len = p->ui_html_len;
    return p->ui_html;
}

int casc_open_ui(casc_instance_t* inst, void* parent_handle) {
    if (!inst || !inst->plugin) return CASC_ERR_INVALID_ARG;
    if (inst->ui) return CASC_OK;             /* already open */
    if (!casc_plugin_has_ui(inst->plugin)) return CASC_ERR_INVALID_ARG;

    size_t html_len = 0;
    const char* html = ensure_ui_html(inst->plugin, &html_len);
    if (!html) return CASC_ERR_FILE_NOT_FOUND;

    casc_ui_state_t* st = (casc_ui_state_t*)calloc(1, sizeof(*st));
    if (!st) return CASC_ERR_OUT_OF_MEMORY;
    st->bridge.inst = inst;

    char* boot = casc_ui_make_bootstrap_js(inst);
    if (!boot) { free(st); return CASC_ERR_OUT_OF_MEMORY; }

    int w = inst->plugin->manifest.gui.width;
    int h = inst->plugin->manifest.gui.height;
    if (w <= 0) w = 480;
    if (h <= 0) h = 320;

    void* backend = NULL;
    casc_error_t err = casc_ui_backend_open(
        inst, parent_handle, html, html_len, boot, &st->bridge,
        w, h, inst->plugin->manifest.gui.resizable, &backend);
    free(boot);

    if (err != CASC_OK || !backend) {
        free(st);
        return err == CASC_OK ? CASC_ERR_IO : err;
    }

    st->backend = backend;
    inst->ui = st;
    return CASC_OK;
}

void casc_close_ui(casc_instance_t* inst) {
    if (!inst || !inst->ui) return;
    casc_ui_state_t* st = (casc_ui_state_t*)inst->ui;
    casc_ui_backend_close(st->backend);
    free(st);
    inst->ui = NULL;
}

int casc_ui_is_open(casc_instance_t* inst) {
    return (inst && inst->ui) ? 1 : 0;
}

int casc_set_ui_size(casc_instance_t* inst, int width, int height) {
    if (!inst || !inst->ui) return CASC_ERR_INVALID_ARG;
    casc_ui_state_t* st = (casc_ui_state_t*)inst->ui;
    return casc_ui_backend_set_size(st->backend, width, height);
}

void casc_ui_tick(casc_instance_t* inst) {
    if (!inst || !inst->ui) return;
    casc_ui_state_t* st = (casc_ui_state_t*)inst->ui;
    casc_ui_backend_tick(st->backend);
}

void casc_ui_notify_param(casc_instance_t* inst, int param_id, double value) {
    if (!inst || !inst->ui) return;
    casc_ui_state_t* st = (casc_ui_state_t*)inst->ui;

    if (value < 0.0) value = 0.0;
    if (value > 1.0) value = 1.0;

    char js[160];
    snprintf(js, sizeof(js),
             "if(window.casc&&window.casc._hostSet)window.casc._hostSet(%d,%.6f);",
             param_id, value);
    casc_ui_backend_eval_js(st->backend, js);
}
