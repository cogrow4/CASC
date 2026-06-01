/*
 * gain.c — Example CASC plugin: Simple Gain
 *
 * A minimal stereo gain plugin demonstrating the CASC DSP API.
 * Compile to WebAssembly with:
 *
 *   clang --target=wasm32 -O3 -msimd128 \
 *         -nostdlib -Wl,--no-entry -Wl,--export-dynamic \
 *         gain.c -o dsp.wasm
 */

#include <stdint.h>
/* Note: no <string.h> — this plugin is compiled with -nostdlib for wasm32,
 * where standard library headers are unavailable. It uses no libc functions. */

/* -------------------------------------------------------------------------- */
/*  Plugin state                                                              */
/* -------------------------------------------------------------------------- */

typedef struct {
    float   param_gain;
    double  sample_rate;
    int32_t max_block_size;
} GainPlugin;

#define MAX_INSTANCES 16
static GainPlugin instances[MAX_INSTANCES];
static int next_handle = 0;

/* -------------------------------------------------------------------------- */
/*  DSP exports (required)                                                    */
/* -------------------------------------------------------------------------- */

__attribute__((export_name("dsp_create")))
int32_t dsp_create(double sample_rate, int32_t max_block_size) {
    if (next_handle >= MAX_INSTANCES) return -1;
    int h = next_handle++;
    instances[h].sample_rate = sample_rate;
    instances[h].max_block_size = max_block_size;
    instances[h].param_gain = 1.0f;
    return h;
}

__attribute__((export_name("dsp_destroy")))
void dsp_destroy(int32_t handle) {
    /* Nothing to free in this simple plugin */
    (void)handle;
}

__attribute__((export_name("dsp_reset")))
void dsp_reset(int32_t handle, double sample_rate, int32_t max_block_size) {
    if (handle < 0 || handle >= MAX_INSTANCES) return;
    instances[handle].sample_rate = sample_rate;
    instances[handle].max_block_size = max_block_size;
}

__attribute__((export_name("dsp_set_param")))
void dsp_set_param(int32_t handle, int32_t param_id, double value) {
    if (handle < 0 || handle >= MAX_INSTANCES) return;
    if (param_id == 0) {
        instances[handle].param_gain = (float)value;
    }
}

__attribute__((export_name("dsp_get_param")))
double dsp_get_param(int32_t handle, int32_t param_id) {
    if (handle < 0 || handle >= MAX_INSTANCES) return 0.0;
    if (param_id == 0) return (double)instances[handle].param_gain;
    return 0.0;
}

__attribute__((export_name("dsp_process")))
void dsp_process(int32_t handle, int32_t in_ptr, int32_t out_ptr,
                 int32_t frames, int32_t in_channels, int32_t out_channels) {
    if (handle < 0 || handle >= MAX_INSTANCES) return;

    float* in  = (float*)(uintptr_t)in_ptr;
    float* out = (float*)(uintptr_t)out_ptr;
    float  g   = instances[handle].param_gain;

    int total_samples = frames * out_channels;
    for (int i = 0; i < total_samples; i++) {
        out[i] = in[i] * g;
    }
}

__attribute__((export_name("dsp_get_latency")))
int32_t dsp_get_latency(int32_t handle) {
    (void)handle;
    return 0;
}

__attribute__((export_name("dsp_get_tail")))
int32_t dsp_get_tail(int32_t handle) {
    (void)handle;
    return 0;
}

/* -------------------------------------------------------------------------- */
/*  State save/load                                                           */
/* -------------------------------------------------------------------------- */

__attribute__((export_name("dsp_save_state")))
int32_t dsp_save_state(int32_t handle, int32_t ptr, int32_t max_bytes) {
    if (handle < 0 || handle >= MAX_INSTANCES) return 0;
    if (max_bytes < (int32_t)sizeof(float)) return 0;

    float* dest = (float*)(uintptr_t)ptr;
    *dest = instances[handle].param_gain;
    return (int32_t)sizeof(float);
}

__attribute__((export_name("dsp_load_state")))
void dsp_load_state(int32_t handle, int32_t ptr, int32_t byte_count) {
    if (handle < 0 || handle >= MAX_INSTANCES) return;
    if (byte_count < (int32_t)sizeof(float)) return;

    float* src = (float*)(uintptr_t)ptr;
    instances[handle].param_gain = *src;
}

/* -------------------------------------------------------------------------- */
/*  Memory allocator (bump allocator for DSP buffers)                         */
/* -------------------------------------------------------------------------- */

static uint8_t heap[1 << 20];  /* 1 MB static heap */
static int heap_top = 0;

__attribute__((export_name("casc_alloc")))
int32_t casc_alloc(int32_t n_bytes) {
    int aligned = (n_bytes + 7) & ~7;  /* 8-byte alignment */
    if (heap_top + aligned > (int)sizeof(heap)) return 0;
    int ptr = heap_top;
    heap_top += aligned;
    return (int32_t)((uintptr_t)heap + ptr);
}

__attribute__((export_name("casc_free")))
void casc_free(int32_t ptr) {
    /* Bump allocator — no individual free.
     * In real plugins, use a proper allocator. */
    (void)ptr;
}
