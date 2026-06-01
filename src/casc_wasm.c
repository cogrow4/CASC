/*
 * casc_wasm.c — Wasmtime integration
 *
 * Manages the Wasm engine (singleton), module compilation,
 * instance creation, and export resolution.
 */

#include "casc_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/*  Singleton engine                                                          */
/* -------------------------------------------------------------------------- */

static wasm_engine_t* g_engine = NULL;

wasm_engine_t* casc_wasm_get_engine(void) {
    if (!g_engine) {
        wasm_config_t* config = wasm_config_new();
        /* Enable Cranelift optimizations */
        wasmtime_config_cranelift_opt_level_set(config, WASMTIME_OPT_LEVEL_SPEED);
        /* Enable SIMD */
        wasmtime_config_wasm_simd_set(config, true);
        g_engine = wasm_engine_new_with_config(config);
    }
    return g_engine;
}

/* -------------------------------------------------------------------------- */
/*  Module compilation                                                        */
/* -------------------------------------------------------------------------- */

casc_error_t casc_wasm_compile(wasm_engine_t* engine,
                                const uint8_t* wasm_bytes, size_t wasm_len,
                                wasmtime_module_t** out_module,
                                char* err_buf, size_t err_buf_len) {
    wasmtime_error_t* error = wasmtime_module_new(engine, wasm_bytes, wasm_len, out_module);
    if (error) {
        wasm_name_t msg;
        wasmtime_error_message(error, &msg);
        if (err_buf) {
            snprintf(err_buf, err_buf_len, "Wasm compile error: %.*s",
                     (int)msg.size, msg.data);
        }
        wasm_byte_vec_delete(&msg);
        wasmtime_error_delete(error);
        return CASC_ERR_WASM_COMPILE;
    }
    return CASC_OK;
}

/* -------------------------------------------------------------------------- */
/*  Instance creation                                                         */
/* -------------------------------------------------------------------------- */

casc_error_t casc_wasm_instantiate(casc_instance_t* inst,
                                    char* err_buf, size_t err_buf_len) {
    /* Create store */
    inst->store = wasmtime_store_new(inst->plugin->engine, NULL, NULL);
    if (!inst->store) {
        if (err_buf) snprintf(err_buf, err_buf_len, "Failed to create Wasm store");
        return CASC_ERR_WASM_RUNTIME;
    }

    wasmtime_context_t* ctx = wasmtime_store_context(inst->store);

    /* Create linker and define host imports */
    wasmtime_linker_t* linker = wasmtime_linker_new(inst->plugin->engine);

    casc_error_t lerr = casc_host_imports_define(linker);
    if (lerr != CASC_OK) {
        if (err_buf) snprintf(err_buf, err_buf_len, "Failed to define host imports");
        wasmtime_linker_delete(linker);
        return lerr;
    }

    /* Instantiate */
    wasm_trap_t* trap = NULL;
    wasmtime_error_t* error = wasmtime_linker_instantiate(
        linker, ctx, inst->plugin->module, &inst->instance, &trap);

    wasmtime_linker_delete(linker);

    if (error || trap) {
        if (error) {
            wasm_name_t msg;
            wasmtime_error_message(error, &msg);
            if (err_buf) snprintf(err_buf, err_buf_len, "Wasm instantiate error: %.*s",
                                   (int)msg.size, msg.data);
            wasm_byte_vec_delete(&msg);
            wasmtime_error_delete(error);
        }
        if (trap) {
            wasm_message_t msg;
            wasm_trap_message(trap, &msg);
            if (err_buf) snprintf(err_buf, err_buf_len, "Wasm trap: %.*s",
                                   (int)msg.size, msg.data);
            wasm_byte_vec_delete(&msg);
            wasm_trap_delete(trap);
        }
        return CASC_ERR_WASM_LINK;
    }

    return CASC_OK;
}

/* -------------------------------------------------------------------------- */
/*  Export resolution                                                         */
/* -------------------------------------------------------------------------- */

static bool resolve_func(wasmtime_context_t* ctx,
                          const wasmtime_instance_t* instance,
                          const char* name,
                          wasmtime_func_t* out_func) {
    wasmtime_extern_t ext;
    bool ok = wasmtime_instance_export_get(ctx, instance, name, strlen(name), &ext);
    if (!ok || ext.kind != WASMTIME_EXTERN_FUNC) return false;
    *out_func = ext.of.func;
    return true;
}

casc_error_t casc_wasm_resolve_exports(casc_instance_t* inst,
                                        char* err_buf, size_t err_buf_len) {
    wasmtime_context_t* ctx = wasmtime_store_context(inst->store);
    casc_wasm_exports_t* ex = &inst->exports;

    /* Required exports */
    struct { const char* name; wasmtime_func_t* target; } required[] = {
        { "dsp_create",     &ex->dsp_create     },
        { "dsp_destroy",    &ex->dsp_destroy    },
        { "dsp_reset",      &ex->dsp_reset      },
        { "dsp_set_param",  &ex->dsp_set_param  },
        { "dsp_get_param",  &ex->dsp_get_param  },
        { "dsp_process",    &ex->dsp_process    },
        { "dsp_save_state", &ex->dsp_save_state },
        { "dsp_load_state", &ex->dsp_load_state },
        { "dsp_get_latency",&ex->dsp_get_latency},
        { "dsp_get_tail",   &ex->dsp_get_tail   },
        { "casc_alloc",     &ex->casc_alloc     },
        { "casc_free",      &ex->casc_free      },
    };

    for (size_t i = 0; i < sizeof(required)/sizeof(required[0]); i++) {
        if (!resolve_func(ctx, &inst->instance, required[i].name, required[i].target)) {
            if (err_buf) snprintf(err_buf, err_buf_len,
                "Missing required Wasm export: '%s'", required[i].name);
            return CASC_ERR_MISSING_EXPORT;
        }
    }

    /* Optional exports */
    ex->has_dsp_send_midi = resolve_func(ctx, &inst->instance, "dsp_send_midi", &ex->dsp_send_midi);

    /* Resolve memory export */
    wasmtime_extern_t mem_ext;
    if (wasmtime_instance_export_get(ctx, &inst->instance, "memory", 6, &mem_ext) &&
        mem_ext.kind == WASMTIME_EXTERN_MEMORY) {
        inst->memory = mem_ext.of.memory;
    } else {
        if (err_buf) snprintf(err_buf, err_buf_len, "Missing 'memory' export");
        return CASC_ERR_MISSING_EXPORT;
    }

    return CASC_OK;
}
