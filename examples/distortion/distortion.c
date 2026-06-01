/*
 * distortion.c — CASC plugin: Waveshaper Distortion
 *
 * Per-sample soft-clip waveshaper. Drive gain pushes the signal into a
 * smooth tanh-style transfer curve, followed by an output level trim, a
 * one-pole tone (lowpass) control, and a dry/wet mix. Fully stereo.
 *
 * Build:
 *   clang --target=wasm32 -O3 -nostdlib -Wl,--no-entry -Wl,--export-dynamic \
 *         distortion.c -o dsp.wasm
 *
 * Params (normalised 0..1):
 *   0 Drive   pre-shaper gain, mapped 1 .. 50
 *   1 Tone    one-pole lowpass post-shaper (0 dark .. 1 bright)
 *   2 Output  linear output level 0 .. 1
 *   3 Mix     dry/wet
 */
#include <stdint.h>

/* ----- minimal math (no libc / no math.h) ----- */
static inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

/* Cheap smooth soft-clip (rational tanh approximation).
 * tanh(x) ~= x*(27 + x*x) / (27 + 9*x*x), clamped to keep it bounded for
 * very large drive values. Saturates toward +/-1. */
static inline float fast_tanh(float x) {
    x = clampf(x, -8.0f, 8.0f);
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

typedef struct {
    /* normalised params */
    float p_drive, p_tone, p_output, p_mix;

    /* tone lowpass state (per channel) */
    float lpL, lpR;

    double sample_rate;
    int    max_block;
    int    active;
} Distortion;

#define MAX_INSTANCES 8
static Distortion g_inst[MAX_INSTANCES];
static int g_next = 0;

/* ----- helpers ----- */
static void dist_setup(Distortion* d, double sr) {
    d->sample_rate = sr;
    d->lpL = 0.0f;
    d->lpR = 0.0f;
}

/* ----- exports ----- */
__attribute__((export_name("dsp_create")))
int32_t dsp_create(double sample_rate, int32_t max_block_size) {
    if (g_next >= MAX_INSTANCES) return -1;
    int h = g_next++;
    Distortion* d = &g_inst[h];
    d->p_drive = 0.4f; d->p_tone = 0.6f; d->p_output = 0.8f; d->p_mix = 1.0f;
    d->max_block = max_block_size;
    d->active = 1;
    dist_setup(d, sample_rate);
    return h;
}

__attribute__((export_name("dsp_destroy")))
void dsp_destroy(int32_t handle) { (void)handle; }

__attribute__((export_name("dsp_reset")))
void dsp_reset(int32_t handle, double sample_rate, int32_t max_block_size) {
    if (handle < 0 || handle >= MAX_INSTANCES) return;
    g_inst[handle].max_block = max_block_size;
    dist_setup(&g_inst[handle], sample_rate);
}

__attribute__((export_name("dsp_set_param")))
void dsp_set_param(int32_t handle, int32_t id, double value) {
    if (handle < 0 || handle >= MAX_INSTANCES) return;
    Distortion* d = &g_inst[handle];
    float v = (float)value;
    if      (id == 0) d->p_drive  = clampf(v,0,1);
    else if (id == 1) d->p_tone   = clampf(v,0,1);
    else if (id == 2) d->p_output = clampf(v,0,1);
    else if (id == 3) d->p_mix    = clampf(v,0,1);
}

__attribute__((export_name("dsp_get_param")))
double dsp_get_param(int32_t handle, int32_t id) {
    if (handle < 0 || handle >= MAX_INSTANCES) return 0.0;
    Distortion* d = &g_inst[handle];
    if      (id == 0) return d->p_drive;
    else if (id == 1) return d->p_tone;
    else if (id == 2) return d->p_output;
    else if (id == 3) return d->p_mix;
    return 0.0;
}

__attribute__((export_name("dsp_process")))
void dsp_process(int32_t handle, int32_t in_ptr, int32_t out_ptr,
                 int32_t frames, int32_t in_ch, int32_t out_ch) {
    if (handle < 0 || handle >= MAX_INSTANCES) return;
    Distortion* d = &g_inst[handle];
    float* in  = (float*)(uintptr_t)in_ptr;
    float* out = (float*)(uintptr_t)out_ptr;

    /* Map params. */
    float drive = 1.0f + d->p_drive * 49.0f;   /* 1 .. 50 */
    float outg  = d->p_output;                  /* 0 .. 1 linear */
    float wet   = d->p_mix;
    float dry   = 1.0f - d->p_mix;
    /* Tone: one-pole lowpass coefficient. 0 = dark (heavy smoothing),
     * 1 = bright (almost no smoothing). a is the new-sample weight. */
    float a = 0.02f + d->p_tone * 0.98f;        /* 0.02 .. 1.0 */

    float lpL = d->lpL, lpR = d->lpR;

    for (int n = 0; n < frames; n++) {
        float inL, inR;
        if (in_ch >= 2) { inL = in[n*in_ch + 0]; inR = in[n*in_ch + 1]; }
        else            { inL = in[n*in_ch + 0]; inR = inL; }

        /* drive -> shaper -> output gain */
        float sL = fast_tanh(inL * drive) * outg;
        float sR = fast_tanh(inR * drive) * outg;

        /* tone: one-pole lowpass */
        lpL += a * (sL - lpL);
        lpR += a * (sR - lpR);

        /* dry/wet mix */
        float yL = inL * dry + lpL * wet;
        float yR = inR * dry + lpR * wet;

        if (out_ch >= 2) { out[n*out_ch + 0] = yL; out[n*out_ch + 1] = yR; }
        else             { out[n*out_ch + 0] = (yL + yR) * 0.5f; }
    }

    d->lpL = lpL; d->lpR = lpR;
}

__attribute__((export_name("dsp_get_latency")))
int32_t dsp_get_latency(int32_t handle) { (void)handle; return 0; }

__attribute__((export_name("dsp_get_tail")))
int32_t dsp_get_tail(int32_t handle) { (void)handle; return 0; }

/* ----- state: store the 4 normalised params ----- */
__attribute__((export_name("dsp_save_state")))
int32_t dsp_save_state(int32_t handle, int32_t ptr, int32_t max_bytes) {
    if (handle < 0 || handle >= MAX_INSTANCES) return 0;
    if (max_bytes < (int32_t)(4*sizeof(float))) return 0;
    float* o = (float*)(uintptr_t)ptr;
    Distortion* d = &g_inst[handle];
    o[0]=d->p_drive; o[1]=d->p_tone; o[2]=d->p_output; o[3]=d->p_mix;
    return (int32_t)(4*sizeof(float));
}

__attribute__((export_name("dsp_load_state")))
void dsp_load_state(int32_t handle, int32_t ptr, int32_t byte_count) {
    if (handle < 0 || handle >= MAX_INSTANCES) return;
    if (byte_count < (int32_t)(4*sizeof(float))) return;
    float* s = (float*)(uintptr_t)ptr;
    Distortion* d = &g_inst[handle];
    d->p_drive=s[0]; d->p_tone=s[1]; d->p_output=s[2]; d->p_mix=s[3];
}

/* ----- bump allocator ----- */
static uint8_t heap[1 << 20];
static int heap_top = 0;
__attribute__((export_name("casc_alloc")))
int32_t casc_alloc(int32_t n) {
    int a = (n + 7) & ~7;
    if (heap_top + a > (int)sizeof(heap)) return 0;
    int p = heap_top; heap_top += a;
    return (int32_t)((uintptr_t)heap + p);
}
__attribute__((export_name("casc_free")))
void casc_free(int32_t ptr) { (void)ptr; }
