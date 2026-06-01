/*
 * delay.c — CASC plugin: Stereo Delay
 *
 * Stereo delay line with feedback and a one-pole lowpass (Tone) on the
 * feedback path. Clean circular buffer per channel: read = write - delaySamples.
 *
 * Build:
 *   clang --target=wasm32 -O3 -nostdlib -Wl,--no-entry -Wl,--export-dynamic \
 *         delay.c -o dsp.wasm
 *
 * Params (normalised 0..1):
 *   0 Time     -> 0.02 .. 1.5 seconds
 *   1 Feedback -> 0 .. 0.95
 *   2 Tone     -> one-pole lowpass on feedback path (0=dark, 1=bright)
 *   3 Mix      -> dry/wet
 */
#include <stdint.h>

/* ----- minimal math (no libc) ----- */
static inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

/* Max delay buffer: 2.0s at 96kHz = 192000 floats per channel. */
#define DELAY_MAX 192000

typedef struct {
    float bufL[DELAY_MAX];
    float bufR[DELAY_MAX];
    int   write;          /* shared write index */
    float lpL, lpR;       /* one-pole lowpass states (feedback path) */

    /* normalised params */
    float p_time, p_fb, p_tone, p_mix;

    double sample_rate;
    int    max_block;
    int    active;
} Delay;

#define MAX_INSTANCES 8
static Delay g_inst[MAX_INSTANCES];
static int g_next = 0;

/* ----- helpers ----- */
static void buf_clear(float* b, int n) { for (int i=0;i<n;i++) b[i]=0.0f; }

static void delay_setup(Delay* d, double sr) {
    d->sample_rate = sr;
    d->write = 0;
    d->lpL = 0.0f;
    d->lpR = 0.0f;
    buf_clear(d->bufL, DELAY_MAX);
    buf_clear(d->bufR, DELAY_MAX);
}

/* ----- exports ----- */
__attribute__((export_name("dsp_create")))
int32_t dsp_create(double sample_rate, int32_t max_block_size) {
    if (g_next >= MAX_INSTANCES) return -1;
    int h = g_next++;
    Delay* d = &g_inst[h];
    d->p_time = 0.25f; d->p_fb = 0.35f; d->p_tone = 0.7f; d->p_mix = 0.35f;
    d->max_block = max_block_size;
    d->active = 1;
    delay_setup(d, sample_rate);
    return h;
}

__attribute__((export_name("dsp_destroy")))
void dsp_destroy(int32_t handle) { (void)handle; }

__attribute__((export_name("dsp_reset")))
void dsp_reset(int32_t handle, double sample_rate, int32_t max_block_size) {
    if (handle < 0 || handle >= MAX_INSTANCES) return;
    g_inst[handle].max_block = max_block_size;
    delay_setup(&g_inst[handle], sample_rate);
}

__attribute__((export_name("dsp_set_param")))
void dsp_set_param(int32_t handle, int32_t id, double value) {
    if (handle < 0 || handle >= MAX_INSTANCES) return;
    Delay* d = &g_inst[handle];
    float v = (float)value;
    if      (id == 0) d->p_time = clampf(v,0,1);
    else if (id == 1) d->p_fb   = clampf(v,0,1);
    else if (id == 2) d->p_tone = clampf(v,0,1);
    else if (id == 3) d->p_mix  = clampf(v,0,1);
}

__attribute__((export_name("dsp_get_param")))
double dsp_get_param(int32_t handle, int32_t id) {
    if (handle < 0 || handle >= MAX_INSTANCES) return 0.0;
    Delay* d = &g_inst[handle];
    if      (id == 0) return d->p_time;
    else if (id == 1) return d->p_fb;
    else if (id == 2) return d->p_tone;
    else if (id == 3) return d->p_mix;
    return 0.0;
}

__attribute__((export_name("dsp_process")))
void dsp_process(int32_t handle, int32_t in_ptr, int32_t out_ptr,
                 int32_t frames, int32_t in_ch, int32_t out_ch) {
    if (handle < 0 || handle >= MAX_INSTANCES) return;
    Delay* d = &g_inst[handle];
    float* in  = (float*)(uintptr_t)in_ptr;
    float* out = (float*)(uintptr_t)out_ptr;

    /* Map params to real ranges. */
    float timeSec = 0.02f + d->p_time * 1.48f;          /* 0.02 .. 1.5 s */
    int   delaySamples = (int)(timeSec * (float)d->sample_rate + 0.5f);
    if (delaySamples < 1) delaySamples = 1;
    if (delaySamples > DELAY_MAX - 1) delaySamples = DELAY_MAX - 1;

    float feedback = d->p_fb * 0.95f;                   /* 0 .. 0.95 */
    /* Tone: one-pole lowpass coefficient. tone=1 -> bright (coef~1),
     * tone=0 -> dark (coef small). y = y + a*(x - y). */
    float toneCoef = 0.02f + d->p_tone * 0.98f;         /* 0.02 .. 1.0 */
    float wet = d->p_mix;
    float dry = 1.0f - d->p_mix;

    for (int n = 0; n < frames; n++) {
        float inL, inR;
        if (in_ch >= 2) { inL = in[n*in_ch + 0]; inR = in[n*in_ch + 1]; }
        else            { inL = in[n*in_ch + 0]; inR = inL; }

        int readIdx = d->write - delaySamples;
        if (readIdx < 0) readIdx += DELAY_MAX;

        float dL = d->bufL[readIdx];
        float dR = d->bufR[readIdx];

        /* one-pole lowpass on the feedback (delayed) signal */
        d->lpL = d->lpL + toneCoef * (dL - d->lpL);
        d->lpR = d->lpR + toneCoef * (dR - d->lpR);

        /* write input + filtered feedback */
        d->bufL[d->write] = inL + d->lpL * feedback;
        d->bufR[d->write] = inR + d->lpR * feedback;

        if (++d->write >= DELAY_MAX) d->write = 0;

        float yL = inL * dry + dL * wet;
        float yR = inR * dry + dR * wet;

        if (out_ch >= 2) { out[n*out_ch + 0] = yL; out[n*out_ch + 1] = yR; }
        else             { out[n*out_ch + 0] = (yL + yR) * 0.5f; }
    }
}

__attribute__((export_name("dsp_get_latency")))
int32_t dsp_get_latency(int32_t handle) { (void)handle; return 0; }

__attribute__((export_name("dsp_get_tail")))
int32_t dsp_get_tail(int32_t handle) {
    if (handle < 0 || handle >= MAX_INSTANCES) return 0;
    /* tail = ~2s of delay decay */
    return (int32_t)(g_inst[handle].sample_rate * 2.0);
}

/* ----- state: store the 4 normalised params ----- */
__attribute__((export_name("dsp_save_state")))
int32_t dsp_save_state(int32_t handle, int32_t ptr, int32_t max_bytes) {
    if (handle < 0 || handle >= MAX_INSTANCES) return 0;
    if (max_bytes < (int32_t)(4*sizeof(float))) return 0;
    float* o = (float*)(uintptr_t)ptr;
    Delay* d = &g_inst[handle];
    o[0]=d->p_time; o[1]=d->p_fb; o[2]=d->p_tone; o[3]=d->p_mix;
    return (int32_t)(4*sizeof(float));
}

__attribute__((export_name("dsp_load_state")))
void dsp_load_state(int32_t handle, int32_t ptr, int32_t byte_count) {
    if (handle < 0 || handle >= MAX_INSTANCES) return;
    if (byte_count < (int32_t)(4*sizeof(float))) return;
    float* s = (float*)(uintptr_t)ptr;
    Delay* d = &g_inst[handle];
    d->p_time=s[0]; d->p_fb=s[1]; d->p_tone=s[2]; d->p_mix=s[3];
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
