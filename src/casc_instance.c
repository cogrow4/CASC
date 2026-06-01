/*
 * casc_instance.c — Plugin lifecycle, audio processing, and public API
 *
 * Implements all public casc_*() functions from libcasc.h.
 * Handles buffer marshalling between host float pointers and Wasm linear memory.
 */

#include "casc_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/*  Version                                                                   */
/* -------------------------------------------------------------------------- */

const char* casc_version(void) {
    return CASC_VERSION_STRING;
}

/* -------------------------------------------------------------------------- */
/*  Error strings                                                             */
/* -------------------------------------------------------------------------- */

const char* casc_error_string(casc_error_t err) {
    switch (err) {
        case CASC_OK:                   return "OK";
        case CASC_ERR_INVALID_ARG:      return "Invalid argument";
        case CASC_ERR_FILE_NOT_FOUND:   return "File not found";
        case CASC_ERR_INVALID_ARCHIVE:  return "Invalid .casc archive";
        case CASC_ERR_INVALID_MANIFEST: return "Invalid manifest.json";
        case CASC_ERR_WASM_COMPILE:     return "Wasm compilation error";
        case CASC_ERR_WASM_LINK:        return "Wasm linking error";
        case CASC_ERR_WASM_RUNTIME:     return "Wasm runtime error";
        case CASC_ERR_MISSING_EXPORT:   return "Missing required Wasm export";
        case CASC_ERR_OUT_OF_MEMORY:    return "Out of memory";
        case CASC_ERR_IO:               return "I/O error";
        case CASC_ERR_SECURITY:         return "Security violation";
        default:                        return "Unknown error";
    }
}

/* -------------------------------------------------------------------------- */
/*  Wasm function call helpers                                                */
/* -------------------------------------------------------------------------- */

static wasmtime_context_t* inst_ctx(casc_instance_t* inst) {
    return wasmtime_store_context(inst->store);
}

static int32_t call_i32_f64_i32(casc_instance_t* inst, wasmtime_func_t* func,
                                 double arg1, int32_t arg2) {
    wasmtime_val_t args[2] = {
        { .kind = WASMTIME_F64, .of.f64 = arg1 },
        { .kind = WASMTIME_I32, .of.i32 = arg2 }
    };
    wasmtime_val_t results[1];
    wasm_trap_t* trap = NULL;
    wasmtime_func_call(inst_ctx(inst), func, args, 2, results, 1, &trap);
    if (trap) { wasm_trap_delete(trap); return -1; }
    return results[0].of.i32;
}

static void call_void_i32(casc_instance_t* inst, wasmtime_func_t* func, int32_t arg) {
    wasmtime_val_t args[1] = { { .kind = WASMTIME_I32, .of.i32 = arg } };
    wasm_trap_t* trap = NULL;
    wasmtime_func_call(inst_ctx(inst), func, args, 1, NULL, 0, &trap);
    if (trap) wasm_trap_delete(trap);
}

static void call_void_i32_f64_i32(casc_instance_t* inst, wasmtime_func_t* func,
                                    int32_t a1, double a2, int32_t a3) {
    wasmtime_val_t args[3] = {
        { .kind = WASMTIME_I32, .of.i32 = a1 },
        { .kind = WASMTIME_F64, .of.f64 = a2 },
        { .kind = WASMTIME_I32, .of.i32 = a3 }
    };
    wasm_trap_t* trap = NULL;
    wasmtime_func_call(inst_ctx(inst), func, args, 3, NULL, 0, &trap);
    if (trap) wasm_trap_delete(trap);
}

static void call_void_i32_i32_f64(casc_instance_t* inst, wasmtime_func_t* func,
                                    int32_t a1, int32_t a2, double a3) {
    wasmtime_val_t args[3] = {
        { .kind = WASMTIME_I32, .of.i32 = a1 },
        { .kind = WASMTIME_I32, .of.i32 = a2 },
        { .kind = WASMTIME_F64, .of.f64 = a3 }
    };
    wasm_trap_t* trap = NULL;
    wasmtime_func_call(inst_ctx(inst), func, args, 3, NULL, 0, &trap);
    if (trap) wasm_trap_delete(trap);
}

static double call_f64_i32_i32(casc_instance_t* inst, wasmtime_func_t* func,
                                int32_t a1, int32_t a2) {
    wasmtime_val_t args[2] = {
        { .kind = WASMTIME_I32, .of.i32 = a1 },
        { .kind = WASMTIME_I32, .of.i32 = a2 }
    };
    wasmtime_val_t results[1];
    wasm_trap_t* trap = NULL;
    wasmtime_func_call(inst_ctx(inst), func, args, 2, results, 1, &trap);
    if (trap) { wasm_trap_delete(trap); return 0.0; }
    return results[0].of.f64;
}

static int32_t call_i32_i32(casc_instance_t* inst, wasmtime_func_t* func, int32_t arg) {
    wasmtime_val_t args[1] = { { .kind = WASMTIME_I32, .of.i32 = arg } };
    wasmtime_val_t results[1];
    wasm_trap_t* trap = NULL;
    wasmtime_func_call(inst_ctx(inst), func, args, 1, results, 1, &trap);
    if (trap) { wasm_trap_delete(trap); return 0; }
    return results[0].of.i32;
}

static int32_t wasm_alloc(casc_instance_t* inst, int32_t nbytes) {
    return call_i32_i32(inst, &inst->exports.casc_alloc, nbytes);
}

static void wasm_free(casc_instance_t* inst, int32_t ptr) {
    call_void_i32(inst, &inst->exports.casc_free, ptr);
}

static uint8_t* wasm_memory_ptr(casc_instance_t* inst) {
    return wasmtime_memory_data(inst_ctx(inst), &inst->memory);
}

/* -------------------------------------------------------------------------- */
/*  casc_load / casc_unload                                                   */
/* -------------------------------------------------------------------------- */

casc_plugin_t* casc_load(const char* path, char* err_buf, size_t err_buf_len) {
    if (!path) return NULL;

    casc_plugin_t* plugin = (casc_plugin_t*)calloc(1, sizeof(casc_plugin_t));
    if (!plugin) {
        if (err_buf) snprintf(err_buf, err_buf_len, "Out of memory");
        return NULL;
    }
    snprintf(plugin->path, sizeof(plugin->path), "%s", path);

    /* Extract archive */
    char* manifest_json = NULL;
    size_t manifest_len = 0;
    casc_error_t err = casc_loader_extract(path,
        &plugin->wasm_bytes, &plugin->wasm_bytes_len,
        &manifest_json, &manifest_len, err_buf, err_buf_len);
    if (err != CASC_OK) { free(plugin); return NULL; }

    /* Parse manifest */
    err = casc_manifest_parse(manifest_json, manifest_len, &plugin->manifest);
    free(manifest_json);
    if (err != CASC_OK) {
        if (err_buf) snprintf(err_buf, err_buf_len, "Invalid manifest.json");
        free(plugin->wasm_bytes);
        free(plugin);
        return NULL;
    }

    /* Get shared engine */
    plugin->engine = casc_wasm_get_engine();

    /* Compute SHA-256 of wasm */
    casc_aot_compute_sha256(plugin->wasm_bytes, plugin->wasm_bytes_len, plugin->wasm_sha256);

    /* Resolve cache directory */
    casc_aot_get_cache_dir(plugin->manifest.id, plugin->cache_dir, sizeof(plugin->cache_dir));

    /* Try loading from AOT cache */
    err = casc_aot_cache_load(plugin->engine, plugin->cache_dir,
                               plugin->wasm_sha256, &plugin->module);
    if (err != CASC_OK) {
        /* Compile from source */
        err = casc_wasm_compile(plugin->engine,
                                 plugin->wasm_bytes, plugin->wasm_bytes_len,
                                 &plugin->module, err_buf, err_buf_len);
        if (err != CASC_OK) {
            free(plugin->wasm_bytes);
            free(plugin);
            return NULL;
        }

        /* Store to AOT cache (best effort) */
        casc_aot_cache_store(plugin->engine, plugin->module,
                              plugin->cache_dir, plugin->wasm_sha256);
    }

    return plugin;
}

void casc_unload(casc_plugin_t* plugin) {
    if (!plugin) return;
    if (plugin->module) wasmtime_module_delete(plugin->module);
    free(plugin->wasm_bytes);
    free(plugin->ui_html);
    free(plugin);
}

char* casc_read_manifest(const char* path) {
    return casc_loader_read_manifest_only(path);
}

/* -------------------------------------------------------------------------- */
/*  casc_instantiate / casc_destroy_instance / casc_reset                     */
/* -------------------------------------------------------------------------- */

casc_instance_t* casc_instantiate(casc_plugin_t* plugin,
                                   double sample_rate,
                                   int max_block_size) {
    if (!plugin) return NULL;

    casc_instance_t* inst = (casc_instance_t*)calloc(1, sizeof(casc_instance_t));
    if (!inst) return NULL;

    inst->plugin = plugin;
    inst->sample_rate = sample_rate;
    inst->max_block_size = max_block_size;

    /* Compute channel counts from manifest */
    inst->in_channels = 0;
    for (int i = 0; i < plugin->manifest.audio_input_count; i++)
        inst->in_channels += plugin->manifest.audio_inputs[i].channels;

    inst->out_channels = 0;
    for (int i = 0; i < plugin->manifest.audio_output_count; i++)
        inst->out_channels += plugin->manifest.audio_outputs[i].channels;

    /* Default to stereo if no ports specified */
    if (inst->in_channels == 0) inst->in_channels = 2;
    if (inst->out_channels == 0) inst->out_channels = 2;

    char err_buf[512] = {0};

    /* Create Wasm instance */
    casc_error_t err = casc_wasm_instantiate(inst, err_buf, sizeof(err_buf));
    if (err != CASC_OK) {
        fprintf(stderr, "casc_instantiate: %s\n", err_buf);
        free(inst);
        return NULL;
    }

    /* Resolve exports */
    err = casc_wasm_resolve_exports(inst, err_buf, sizeof(err_buf));
    if (err != CASC_OK) {
        fprintf(stderr, "casc_instantiate: %s\n", err_buf);
        wasmtime_store_delete(inst->store);
        free(inst);
        return NULL;
    }

    /* Allocate audio buffers in Wasm memory */
    int max_ch = inst->in_channels > inst->out_channels ? inst->in_channels : inst->out_channels;
    inst->buf_alloc_size = (size_t)max_block_size * (size_t)max_ch * sizeof(float);

    inst->wasm_in_ptr  = wasm_alloc(inst, (int32_t)inst->buf_alloc_size);
    inst->wasm_out_ptr = wasm_alloc(inst, (int32_t)inst->buf_alloc_size);

    /* Call dsp_create */
    inst->dsp_handle = call_i32_f64_i32(inst, &inst->exports.dsp_create,
                                         sample_rate, max_block_size);

    return inst;
}

void casc_destroy_instance(casc_instance_t* inst) {
    if (!inst) return;

    /* Close the GUI if the host left it open. */
    if (inst->ui) casc_close_ui(inst);

    /* Call dsp_destroy */
    call_void_i32(inst, &inst->exports.dsp_destroy, inst->dsp_handle);

    /* Free Wasm buffers */
    if (inst->wasm_in_ptr)  wasm_free(inst, inst->wasm_in_ptr);
    if (inst->wasm_out_ptr) wasm_free(inst, inst->wasm_out_ptr);

    /* Clean up wasmtime */
    if (inst->store) wasmtime_store_delete(inst->store);
    free(inst);
}

void casc_reset(casc_instance_t* inst, double sample_rate, int max_block_size) {
    if (!inst) return;
    inst->sample_rate = sample_rate;
    inst->max_block_size = max_block_size;
    call_void_i32_f64_i32(inst, &inst->exports.dsp_reset,
                           inst->dsp_handle, sample_rate, max_block_size);
}

/* -------------------------------------------------------------------------- */
/*  casc_process                                                              */
/* -------------------------------------------------------------------------- */

void casc_process(casc_instance_t* inst,
                   const float** inputs,
                   float** outputs,
                   int frames) {
    if (!inst || frames <= 0) return;

    wasmtime_context_t* ctx = inst_ctx(inst);
    uint8_t* mem = wasmtime_memory_data(ctx, &inst->memory);

    int in_ch = inst->in_channels;
    int out_ch = inst->out_channels;

    /*
     * Marshal host deinterleaved buffers → Wasm interleaved buffer.
     * Host format: inputs[channel][frame]
     * Wasm format: [L0, R0, L1, R1, ...] (interleaved)
     *
     * Instruments (and any host that has no audio input connected) may pass
     * inputs == NULL, or individual NULL channel pointers. In that case feed
     * the DSP silence rather than dereferencing a null pointer.
     */
    float* wasm_in = (float*)(mem + inst->wasm_in_ptr);
    for (int f = 0; f < frames; f++) {
        for (int c = 0; c < in_ch; c++) {
            const float* src = (inputs && inputs[c]) ? inputs[c] : NULL;
            wasm_in[f * in_ch + c] = src ? src[f] : 0.0f;
        }
    }

    /* Call dsp_process */
    wasmtime_val_t args[6] = {
        { .kind = WASMTIME_I32, .of.i32 = inst->dsp_handle },
        { .kind = WASMTIME_I32, .of.i32 = inst->wasm_in_ptr },
        { .kind = WASMTIME_I32, .of.i32 = inst->wasm_out_ptr },
        { .kind = WASMTIME_I32, .of.i32 = frames },
        { .kind = WASMTIME_I32, .of.i32 = in_ch },
        { .kind = WASMTIME_I32, .of.i32 = out_ch }
    };
    wasm_trap_t* trap = NULL;
    wasmtime_func_call(ctx, &inst->exports.dsp_process, args, 6, NULL, 0, &trap);
    if (trap) {
        wasm_trap_delete(trap);
        /* On trap, zero outputs */
        for (int c = 0; c < out_ch; c++)
            memset(outputs[c], 0, (size_t)frames * sizeof(float));
        return;
    }

    /*
     * Marshal Wasm interleaved output → host deinterleaved buffers.
     * Re-read memory pointer (may have changed if memory grew).
     */
    mem = wasmtime_memory_data(ctx, &inst->memory);
    float* wasm_out = (float*)(mem + inst->wasm_out_ptr);
    for (int f = 0; f < frames; f++) {
        for (int c = 0; c < out_ch; c++) {
            outputs[c][f] = wasm_out[f * out_ch + c];
        }
    }
}

/* -------------------------------------------------------------------------- */
/*  casc_send_midi                                                            */
/* -------------------------------------------------------------------------- */

void casc_send_midi(casc_instance_t* inst,
                     const casc_midi_event_t* events,
                     int count) {
    if (!inst || !events || count <= 0) return;
    if (!inst->exports.has_dsp_send_midi) return;

    /* Allocate space in Wasm memory for the events */
    int32_t event_size = 8; /* 4 + 1 + 1 + 1 + 1 */
    int32_t total_size = event_size * count;
    int32_t wasm_ptr = wasm_alloc(inst, total_size);

    uint8_t* mem = wasm_memory_ptr(inst);
    uint8_t* dst = mem + wasm_ptr;

    for (int i = 0; i < count; i++) {
        int32_t offset = events[i].frame_offset;
        memcpy(dst, &offset, 4);
        dst[4] = events[i].status;
        dst[5] = events[i].data1;
        dst[6] = events[i].data2;
        dst[7] = 0; /* pad */
        dst += event_size;
    }

    /* Call dsp_send_midi(handle, events_ptr, event_count) */
    wasmtime_val_t args[3] = {
        { .kind = WASMTIME_I32, .of.i32 = inst->dsp_handle },
        { .kind = WASMTIME_I32, .of.i32 = wasm_ptr },
        { .kind = WASMTIME_I32, .of.i32 = count }
    };
    wasm_trap_t* trap = NULL;
    wasmtime_func_call(inst_ctx(inst), &inst->exports.dsp_send_midi,
                        args, 3, NULL, 0, &trap);
    if (trap) wasm_trap_delete(trap);

    wasm_free(inst, wasm_ptr);
}

/* -------------------------------------------------------------------------- */
/*  Parameters                                                                */
/* -------------------------------------------------------------------------- */

void casc_set_param(casc_instance_t* inst, int param_id, double value) {
    if (!inst) return;
    call_void_i32_i32_f64(inst, &inst->exports.dsp_set_param,
                           inst->dsp_handle, param_id, value);
}

double casc_get_param(casc_instance_t* inst, int param_id) {
    if (!inst) return 0.0;
    return call_f64_i32_i32(inst, &inst->exports.dsp_get_param,
                             inst->dsp_handle, param_id);
}

int casc_get_latency(casc_instance_t* inst) {
    if (!inst) return 0;
    return call_i32_i32(inst, &inst->exports.dsp_get_latency, inst->dsp_handle);
}

int casc_get_tail(casc_instance_t* inst) {
    if (!inst) return 0;
    return call_i32_i32(inst, &inst->exports.dsp_get_tail, inst->dsp_handle);
}

/* -------------------------------------------------------------------------- */
/*  State                                                                     */
/* -------------------------------------------------------------------------- */

void* casc_save_state(casc_instance_t* inst, size_t* out_size) {
    if (!inst || !out_size) return NULL;

    /* Allocate a buffer in Wasm memory for state */
    int32_t max_bytes = CASC_STATE_MAX_BYTES;
    int32_t wasm_ptr = wasm_alloc(inst, max_bytes);

    /* Call dsp_save_state(handle, ptr, max_bytes) → byte_count */
    wasmtime_val_t args[3] = {
        { .kind = WASMTIME_I32, .of.i32 = inst->dsp_handle },
        { .kind = WASMTIME_I32, .of.i32 = wasm_ptr },
        { .kind = WASMTIME_I32, .of.i32 = max_bytes }
    };
    wasmtime_val_t results[1];
    wasm_trap_t* trap = NULL;
    wasmtime_func_call(inst_ctx(inst), &inst->exports.dsp_save_state,
                        args, 3, results, 1, &trap);
    if (trap) {
        wasm_trap_delete(trap);
        wasm_free(inst, wasm_ptr);
        return NULL;
    }

    int32_t byte_count = results[0].of.i32;
    if (byte_count <= 0 || byte_count > max_bytes) {
        wasm_free(inst, wasm_ptr);
        return NULL;
    }

    /* Copy from Wasm memory to host heap */
    void* blob = malloc((size_t)byte_count);
    if (!blob) {
        wasm_free(inst, wasm_ptr);
        return NULL;
    }

    uint8_t* mem = wasm_memory_ptr(inst);
    memcpy(blob, mem + wasm_ptr, (size_t)byte_count);
    *out_size = (size_t)byte_count;

    wasm_free(inst, wasm_ptr);
    return blob;
}

int casc_load_state(casc_instance_t* inst, const void* data, size_t size) {
    if (!inst || !data || size == 0) return CASC_ERR_INVALID_ARG;
    if (size > (size_t)CASC_STATE_MAX_BYTES) return CASC_ERR_INVALID_ARG;

    /* Copy state blob into Wasm memory */
    int32_t wasm_ptr = wasm_alloc(inst, (int32_t)size);
    uint8_t* mem = wasm_memory_ptr(inst);
    memcpy(mem + wasm_ptr, data, size);

    /* Call dsp_load_state(handle, ptr, byte_count) */
    wasmtime_val_t args[3] = {
        { .kind = WASMTIME_I32, .of.i32 = inst->dsp_handle },
        { .kind = WASMTIME_I32, .of.i32 = wasm_ptr },
        { .kind = WASMTIME_I32, .of.i32 = (int32_t)size }
    };
    wasm_trap_t* trap = NULL;
    wasmtime_func_call(inst_ctx(inst), &inst->exports.dsp_load_state,
                        args, 3, NULL, 0, &trap);
    wasm_free(inst, wasm_ptr);

    if (trap) {
        wasm_trap_delete(trap);
        return CASC_ERR_WASM_RUNTIME;
    }
    return CASC_OK;
}

/* -------------------------------------------------------------------------- */
/*  Manifest query functions                                                  */
/* -------------------------------------------------------------------------- */

const char* casc_plugin_get_id(const casc_plugin_t* p) {
    return p ? p->manifest.id : "";
}

const char* casc_plugin_get_name(const casc_plugin_t* p) {
    return p ? p->manifest.name : "";
}

const char* casc_plugin_get_vendor(const casc_plugin_t* p) {
    return p ? p->manifest.vendor : "";
}

const char* casc_plugin_get_version(const casc_plugin_t* p) {
    return p ? p->manifest.version : "";
}

const char* casc_plugin_get_description(const casc_plugin_t* p) {
    return p ? p->manifest.description : "";
}

const char* casc_plugin_get_category(const casc_plugin_t* p) {
    return p ? p->manifest.category : "";
}

int casc_plugin_get_param_count(const casc_plugin_t* p) {
    return p ? p->manifest.param_count : 0;
}

int casc_plugin_get_param_info(const casc_plugin_t* p, int index,
                                casc_param_info_t* info) {
    if (!p || !info || index < 0 || index >= p->manifest.param_count)
        return CASC_ERR_INVALID_ARG;

    const casc_manifest_param_t* mp = &p->manifest.params[index];
    info->id = mp->id;
    info->name = mp->name;
    info->short_name = mp->short_name;
    info->module = mp->module;
    info->min_value = mp->min_value;
    info->max_value = mp->max_value;
    info->default_value = mp->default_value;
    info->unit = mp->unit;
    info->flags = mp->flags;
    info->steps = mp->steps;
    return CASC_OK;
}

int casc_plugin_get_audio_input_count(const casc_plugin_t* p) {
    return p ? p->manifest.audio_input_count : 0;
}

int casc_plugin_get_audio_output_count(const casc_plugin_t* p) {
    return p ? p->manifest.audio_output_count : 0;
}

int casc_plugin_get_audio_input_info(const casc_plugin_t* p, int index,
                                      casc_audio_port_info_t* info) {
    if (!p || !info || index < 0 || index >= p->manifest.audio_input_count)
        return CASC_ERR_INVALID_ARG;
    info->name = p->manifest.audio_inputs[index].name;
    info->channels = p->manifest.audio_inputs[index].channels;
    return CASC_OK;
}

int casc_plugin_get_audio_output_info(const casc_plugin_t* p, int index,
                                       casc_audio_port_info_t* info) {
    if (!p || !info || index < 0 || index >= p->manifest.audio_output_count)
        return CASC_ERR_INVALID_ARG;
    info->name = p->manifest.audio_outputs[index].name;
    info->channels = p->manifest.audio_outputs[index].channels;
    return CASC_OK;
}

int casc_plugin_has_midi_input(const casc_plugin_t* p) {
    return (p && p->manifest.midi_input) ? 1 : 0;
}

int casc_plugin_get_latency_frames(const casc_plugin_t* p) {
    return p ? p->manifest.latency_frames : 0;
}

double casc_plugin_get_tail_seconds(const casc_plugin_t* p) {
    return p ? p->manifest.tail_seconds : 0.0;
}

int casc_plugin_get_feature_count(const casc_plugin_t* p) {
    return p ? p->manifest.feature_count : 0;
}

const char* casc_plugin_get_feature(const casc_plugin_t* p, int index) {
    if (!p || index < 0 || index >= p->manifest.feature_count) return "";
    return p->manifest.features[index];
}
