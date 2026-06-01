/*
 * casc_internal.h — Internal types and helpers for libcasc
 *
 * NOT part of the public API. Do not include from outside src/.
 */

#ifndef CASC_INTERNAL_H
#define CASC_INTERNAL_H

#include "libcasc.h"
#include <wasmtime.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*  Constants                                                                 */
/* -------------------------------------------------------------------------- */

#define CASC_MAX_PARAMS          256
#define CASC_MAX_AUDIO_PORTS     8
#define CASC_MAX_FEATURES        16
#define CASC_MAX_STRING          256
#define CASC_MAX_PRESETS         64
#define CASC_STATE_MAX_BYTES     (1 << 20)  /* 1 MB max state blob */

/* -------------------------------------------------------------------------- */
/*  Parsed manifest types                                                     */
/* -------------------------------------------------------------------------- */

typedef struct casc_manifest_param {
    int         id;
    char        name[CASC_MAX_STRING];
    char        short_name[CASC_MAX_STRING];
    char        module[CASC_MAX_STRING];
    double      min_value;
    double      max_value;
    double      default_value;
    char        unit[64];
    uint32_t    flags;          /* bitmask of CASC_PARAM_FLAG_* */
    int         steps;          /* 0 = continuous */
} casc_manifest_param_t;

typedef struct casc_manifest_port {
    char        name[CASC_MAX_STRING];
    int         channels;
} casc_manifest_port_t;

typedef struct casc_manifest_gui {
    bool        present;
    char        type[32];       /* "wasm" or "html" */
    char        entry[CASC_MAX_STRING];
    int         width;
    int         height;
    bool        resizable;
} casc_manifest_gui_t;

typedef struct casc_manifest {
    char        casc_version[32];
    char        id[CASC_MAX_STRING];
    char        name[CASC_MAX_STRING];
    char        version[64];
    char        vendor[CASC_MAX_STRING];
    char        url[CASC_MAX_STRING];
    char        description[CASC_MAX_STRING];
    char        category[64];

    char        features[CASC_MAX_FEATURES][64];
    int         feature_count;

    casc_manifest_port_t audio_inputs[CASC_MAX_AUDIO_PORTS];
    int         audio_input_count;
    casc_manifest_port_t audio_outputs[CASC_MAX_AUDIO_PORTS];
    int         audio_output_count;

    bool        midi_input;
    bool        midi_output;
    bool        mpe_support;

    int         latency_frames;
    double      tail_seconds;
    bool        hard_realtime;

    casc_manifest_param_t params[CASC_MAX_PARAMS];
    int         param_count;

    casc_manifest_gui_t gui;
} casc_manifest_t;

/* -------------------------------------------------------------------------- */
/*  Plugin (loaded archive)                                                   */
/* -------------------------------------------------------------------------- */

struct casc_plugin {
    char                path[1024];         /* .casc file path */
    casc_manifest_t     manifest;

    /* Raw dsp.wasm bytes (extracted from archive) */
    uint8_t*            wasm_bytes;
    size_t              wasm_bytes_len;

    /* Compiled Wasm module (shared across instances) */
    wasm_engine_t*      engine;
    wasmtime_module_t*  module;

    /* AOT cache info */
    char                cache_dir[1024];
    uint8_t             wasm_sha256[32];
};

/* -------------------------------------------------------------------------- */
/*  Instance (running plugin)                                                 */
/* -------------------------------------------------------------------------- */

typedef struct casc_wasm_exports {
    wasmtime_func_t     dsp_create;
    wasmtime_func_t     dsp_destroy;
    wasmtime_func_t     dsp_reset;
    wasmtime_func_t     dsp_set_param;
    wasmtime_func_t     dsp_get_param;
    wasmtime_func_t     dsp_process;
    wasmtime_func_t     dsp_save_state;
    wasmtime_func_t     dsp_load_state;
    wasmtime_func_t     dsp_get_latency;
    wasmtime_func_t     dsp_get_tail;
    wasmtime_func_t     casc_alloc;
    wasmtime_func_t     casc_free;
    /* Optional */
    bool                has_dsp_send_midi;
    wasmtime_func_t     dsp_send_midi;
} casc_wasm_exports_t;

struct casc_instance {
    casc_plugin_t*      plugin;

    /* Wasmtime runtime state */
    wasmtime_store_t*   store;
    wasmtime_instance_t instance;
    wasmtime_memory_t   memory;

    /* Cached export function handles */
    casc_wasm_exports_t exports;

    /* DSP handle returned by dsp_create() */
    int32_t             dsp_handle;

    /* Audio config */
    double              sample_rate;
    int                 max_block_size;
    int                 in_channels;
    int                 out_channels;

    /* Wasm memory pointers for audio buffers */
    int32_t             wasm_in_ptr;
    int32_t             wasm_out_ptr;
    size_t              buf_alloc_size;
};

/* -------------------------------------------------------------------------- */
/*  Internal function declarations                                            */
/* -------------------------------------------------------------------------- */

/* casc_manifest.c */
casc_error_t casc_manifest_parse(const char* json_str, size_t json_len,
                                  casc_manifest_t* out);

/* casc_loader.c */
casc_error_t casc_loader_extract(const char* path,
                                  uint8_t** out_wasm, size_t* out_wasm_len,
                                  char** out_manifest_json, size_t* out_manifest_len,
                                  char* err_buf, size_t err_buf_len);
char* casc_loader_read_manifest_only(const char* path);

/* casc_wasm.c */
wasm_engine_t* casc_wasm_get_engine(void);
casc_error_t casc_wasm_compile(wasm_engine_t* engine,
                                const uint8_t* wasm_bytes, size_t wasm_len,
                                wasmtime_module_t** out_module,
                                char* err_buf, size_t err_buf_len);
casc_error_t casc_wasm_instantiate(casc_instance_t* inst,
                                    char* err_buf, size_t err_buf_len);
casc_error_t casc_wasm_resolve_exports(casc_instance_t* inst,
                                        char* err_buf, size_t err_buf_len);

/* casc_aot.c */
void casc_aot_compute_sha256(const uint8_t* data, size_t len, uint8_t out[32]);
casc_error_t casc_aot_get_cache_dir(const char* plugin_id, char* buf, size_t buf_len);
casc_error_t casc_aot_cache_store(wasm_engine_t* engine,
                                   wasmtime_module_t* module,
                                   const char* cache_dir,
                                   const uint8_t sha256[32]);
casc_error_t casc_aot_cache_load(wasm_engine_t* engine,
                                  const char* cache_dir,
                                  const uint8_t sha256[32],
                                  wasmtime_module_t** out_module);

/* casc_host_imports.c */
casc_error_t casc_host_imports_define(wasmtime_linker_t* linker);

#ifdef __cplusplus
}
#endif

#endif /* CASC_INTERNAL_H */
