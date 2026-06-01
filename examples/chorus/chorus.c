/*
 * chorus.c — CASC plugin: Stereo Chorus (modulated short delay lines)
 *
 * Classic chorus: one modulated delay line per channel, driven by two sine
 * LFOs in quadrature (90° apart) for wide stereo movement. The delay time is
 * swept around a ~12ms base by a ~1..9ms (depth-scaled) modulation. Fractional
 * delay reads are linearly interpolated. The LFO uses a self-contained phase
 * accumulator and a parabolic sine approximation (no math.h / no libc).
 *
 * Build:
 *   clang --target=wasm32 -O3 -nostdlib -Wl,--no-entry -Wl,--export-dynamic \
 *         chorus.c -o dsp.wasm
 *
 * Params (normalised 0..1):
 *   0 Rate   LFO rate, mapped 0.05 .. 6 Hz
 *   1 Depth  modulation depth, scales a ~1..9ms sweep around a ~12ms base
 *   2 Mix    dry/wet
 */
#include <stdint.h>

/* ----- minimal math (no libc) ----- */
static inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}
static inline float my_fabsf(float x) { return x < 0.0f ? -x : x; }

#define PI   3.14159265358979323846f
#define TWO_PI (2.0f * PI)

/* Parabolic sine approximation. Input phase in -PI..PI. */
static inline float fast_sin(float x) {
    /* wrap into -PI..PI */
    while (x >  PI) x -= TWO_PI;
    while (x < -PI) x += TWO_PI;
    /* base parabola */
    float s = 1.27323954f * x - 0.405284735f * x * my_fabsf(x);
    /* extra precision pass */
    s = 0.225f * (s * my_fabsf(s) - s) + s;
    return s;
}

#define DELAY_LEN 4096          /* ~42ms @ 96k */
#define DELAY_MASK (DELAY_LEN - 1)

typedef struct {
    float bufL[DELAY_LEN];
    float bufR[DELAY_LEN];
    int   widx;                 /* shared write index */

    float lfo_phase;            /* radians, 0..2PI */

    /* normalised params */
    float p_rate, p_depth, p_mix;

    double sample_rate;
    int    max_block;
    int    active;
} Chorus;

#define MAX_INSTANCES 8
static Chorus g_inst[MAX_INSTANCES];
static int g_next = 0;

/* ----- helpers ----- */
static void buf_clear(float* b, int n) { for (int i=0;i<n;i++) b[i]=0.0f; }

static void chorus_setup(Chorus* c, double sr) {
    c->sample_rate = sr;
    c->widx = 0;
    c->lfo_phase = 0.0f;
    buf_clear(c->bufL, DELAY_LEN);
    buf_clear(c->bufR, DELAY_LEN);
}

/* read delay buffer with linear interpolation; delay_samps is fractional,
 * measured backwards from the current write index. */
static inline float read_frac(const float* buf, int widx, float delay_samps) {
    float rpos = (float)widx - delay_samps;
    /* wrap into 0..DELAY_LEN */
    while (rpos < 0.0f)         rpos += (float)DELAY_LEN;
    while (rpos >= (float)DELAY_LEN) rpos -= (float)DELAY_LEN;
    int i0 = (int)rpos;
    float frac = rpos - (float)i0;
    int i1 = (i0 + 1) & DELAY_MASK;
    return buf[i0] + (buf[i1] - buf[i0]) * frac;
}

/* ----- exports ----- */
__attribute__((export_name("dsp_create")))
int32_t dsp_create(double sample_rate, int32_t max_block_size) {
    if (g_next >= MAX_INSTANCES) return -1;
    int h = g_next++;
    Chorus* c = &g_inst[h];
    c->p_rate = 0.3f; c->p_depth = 0.5f; c->p_mix = 0.5f;
    c->max_block = max_block_size;
    c->active = 1;
    chorus_setup(c, sample_rate);
    return h;
}

__attribute__((export_name("dsp_destroy")))
void dsp_destroy(int32_t handle) { (void)handle; }

__attribute__((export_name("dsp_reset")))
void dsp_reset(int32_t handle, double sample_rate, int32_t max_block_size) {
    if (handle < 0 || handle >= MAX_INSTANCES) return;
    g_inst[handle].max_block = max_block_size;
    chorus_setup(&g_inst[handle], sample_rate);
}

__attribute__((export_name("dsp_set_param")))
void dsp_set_param(int32_t handle, int32_t id, double value) {
    if (handle < 0 || handle >= MAX_INSTANCES) return;
    Chorus* c = &g_inst[handle];
    float v = (float)value;
    if      (id == 0) c->p_rate  = clampf(v,0,1);
    else if (id == 1) c->p_depth = clampf(v,0,1);
    else if (id == 2) c->p_mix   = clampf(v,0,1);
}

__attribute__((export_name("dsp_get_param")))
double dsp_get_param(int32_t handle, int32_t id) {
    if (handle < 0 || handle >= MAX_INSTANCES) return 0.0;
    Chorus* c = &g_inst[handle];
    if      (id == 0) return c->p_rate;
    else if (id == 1) return c->p_depth;
    else if (id == 2) return c->p_mix;
    return 0.0;
}

__attribute__((export_name("dsp_process")))
void dsp_process(int32_t handle, int32_t in_ptr, int32_t out_ptr,
                 int32_t frames, int32_t in_ch, int32_t out_ch) {
    if (handle < 0 || handle >= MAX_INSTANCES) return;
    Chorus* c = &g_inst[handle];
    float* in  = (float*)(uintptr_t)in_ptr;
    float* out = (float*)(uintptr_t)out_ptr;

    float sr = (float)c->sample_rate;

    /* Map params. */
    float rateHz = 0.05f + c->p_rate * 5.95f;   /* 0.05 .. 6 Hz */
    float phase_inc = TWO_PI * rateHz / sr;

    /* delay design (ms -> samples) */
    float base_ms  = 12.0f;
    float sweep_ms = 1.0f + c->p_depth * 8.0f;  /* ~1..9 ms peak deviation */
    float base_samps  = base_ms  * 0.001f * sr;
    float sweep_samps = sweep_ms * 0.001f * sr;

    float wet = c->p_mix;
    float dry = 1.0f - c->p_mix;

    float phase = c->lfo_phase;
    int   widx  = c->widx;

    for (int n = 0; n < frames; n++) {
        float inL, inR;
        if (in_ch >= 2) { inL = in[n*in_ch + 0]; inR = in[n*in_ch + 1]; }
        else            { inL = in[n*in_ch + 0]; inR = inL; }

        /* write current input into the delay lines */
        c->bufL[widx] = inL;
        c->bufR[widx] = inR;

        /* quadrature LFOs: left uses sin(phase), right uses sin(phase + PI/2) */
        float modL = fast_sin(phase - PI);              /* shift to -PI..PI domain */
        float modR = fast_sin(phase - PI + (PI * 0.5f));

        float dL = base_samps + sweep_samps * modL;
        float dR = base_samps + sweep_samps * modR;

        /* clamp to safe range inside the buffer */
        if (dL < 1.0f) dL = 1.0f; if (dL > (float)(DELAY_LEN-2)) dL = (float)(DELAY_LEN-2);
        if (dR < 1.0f) dR = 1.0f; if (dR > (float)(DELAY_LEN-2)) dR = (float)(DELAY_LEN-2);

        float wetL = read_frac(c->bufL, widx, dL);
        float wetR = read_frac(c->bufR, widx, dR);

        float yL = inL * dry + wetL * wet;
        float yR = inR * dry + wetR * wet;

        if (out_ch >= 2) { out[n*out_ch + 0] = yL; out[n*out_ch + 1] = yR; }
        else             { out[n*out_ch + 0] = (yL + yR) * 0.5f; }

        /* advance */
        widx = (widx + 1) & DELAY_MASK;
        phase += phase_inc;
        if (phase >= TWO_PI) phase -= TWO_PI;
    }

    c->lfo_phase = phase;
    c->widx = widx;
}

__attribute__((export_name("dsp_get_latency")))
int32_t dsp_get_latency(int32_t handle) { (void)handle; return 0; }

__attribute__((export_name("dsp_get_tail")))
int32_t dsp_get_tail(int32_t handle) { (void)handle; return 0; }

/* ----- state: store the 3 normalised params ----- */
__attribute__((export_name("dsp_save_state")))
int32_t dsp_save_state(int32_t handle, int32_t ptr, int32_t max_bytes) {
    if (handle < 0 || handle >= MAX_INSTANCES) return 0;
    if (max_bytes < (int32_t)(3*sizeof(float))) return 0;
    float* d = (float*)(uintptr_t)ptr;
    Chorus* c = &g_inst[handle];
    d[0]=c->p_rate; d[1]=c->p_depth; d[2]=c->p_mix;
    return (int32_t)(3*sizeof(float));
}

__attribute__((export_name("dsp_load_state")))
void dsp_load_state(int32_t handle, int32_t ptr, int32_t byte_count) {
    if (handle < 0 || handle >= MAX_INSTANCES) return;
    if (byte_count < (int32_t)(3*sizeof(float))) return;
    float* s = (float*)(uintptr_t)ptr;
    Chorus* c = &g_inst[handle];
    c->p_rate=s[0]; c->p_depth=s[1]; c->p_mix=s[2];
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
