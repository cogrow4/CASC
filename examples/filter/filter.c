/*
 * filter.c — CASC plugin: State-Variable Filter (LP / HP / BP)
 *
 * Chamberlin SVF, independent state per channel. Cutoff is mapped
 * exponentially (~20 Hz .. ~20 kHz). Type is a 3-way stepped selector.
 *
 * Build:
 *   clang --target=wasm32 -O3 -nostdlib -Wl,--no-entry -Wl,--export-dynamic \
 *         filter.c -o dsp.wasm
 */
#include <stdint.h>

static inline float clampf(float x, float lo, float hi){
    return x<lo?lo:(x>hi?hi:x);
}

/* ---- tiny math: sinf via Bhaskara-style / polynomial, valid 0..PI/2 here ---- */
#define PI_F  3.14159265358979f
/* sine approximation good over [-PI, PI] (parabolic + refinement) */
static float my_fabsf(float x){ return x<0.0f?-x:x; }
static float fast_sin(float x){
    /* wrap to [-PI,PI] */
    while (x >  PI_F) x -= 2.0f*PI_F;
    while (x < -PI_F) x += 2.0f*PI_F;
    float y = 1.27323954f*x - 0.405284735f*x*my_fabsf(x);
    y = 0.225f*(y*my_fabsf(y) - y) + y;
    return y;
}

/* 2^x for x>=0 small range — used for exp cutoff mapping: 20 * 1000^v.
 * 1000^v = 2^(v*log2(1000)) = 2^(v*9.96578). We implement pow2. */
static float pow2f(float x){
    if (x < 0.0f) x = 0.0f;
    int i = (int)x;
    float f = x - (float)i;
    /* 2^f polynomial approx on [0,1) */
    float p = 1.0f + f*(0.6931472f + f*(0.2402265f + f*(0.0555041f + f*0.0096181f)));
    /* multiply by 2^i */
    float r = p;
    for (int k=0;k<i;k++) r *= 2.0f;
    return r;
}
static float cutoff_hz(float v){
    /* 20 .. ~20000 */
    return 20.0f * pow2f(v * 9.96578f);
}

typedef struct {
    float lpL, bpL, lpR, bpR;
    float p_cut, p_res, p_type;
    double sample_rate;
    int active;
} Filt;

#define MAX_INSTANCES 8
static Filt g[MAX_INSTANCES];
static int g_next = 0;

static void clear_state(Filt* s){ s->lpL=s->bpL=s->lpR=s->bpR=0.0f; }

__attribute__((export_name("dsp_create")))
int32_t dsp_create(double sr, int32_t mbs){
    (void)mbs;
    if (g_next>=MAX_INSTANCES) return -1;
    int h=g_next++;
    g[h].p_cut=0.7f; g[h].p_res=0.2f; g[h].p_type=0.0f;
    g[h].sample_rate=sr; g[h].active=1;
    clear_state(&g[h]);
    return h;
}
__attribute__((export_name("dsp_destroy")))
void dsp_destroy(int32_t h){ (void)h; }

__attribute__((export_name("dsp_reset")))
void dsp_reset(int32_t h, double sr, int32_t mbs){
    (void)mbs;
    if (h<0||h>=MAX_INSTANCES) return;
    g[h].sample_rate=sr; clear_state(&g[h]);
}

__attribute__((export_name("dsp_set_param")))
void dsp_set_param(int32_t h, int32_t id, double val){
    if (h<0||h>=MAX_INSTANCES) return;
    float v=clampf((float)val,0.0f,1.0f);
    if      (id==0) g[h].p_cut=v;
    else if (id==1) g[h].p_res=v;
    else if (id==2) g[h].p_type=v;
}
__attribute__((export_name("dsp_get_param")))
double dsp_get_param(int32_t h, int32_t id){
    if (h<0||h>=MAX_INSTANCES) return 0.0;
    if (id==0) return g[h].p_cut;
    if (id==1) return g[h].p_res;
    if (id==2) return g[h].p_type;
    return 0.0;
}

__attribute__((export_name("dsp_process")))
void dsp_process(int32_t h, int32_t in_ptr, int32_t out_ptr,
                 int32_t frames, int32_t in_ch, int32_t out_ch){
    if (h<0||h>=MAX_INSTANCES) return;
    Filt* s=&g[h];
    float* in =(float*)(uintptr_t)in_ptr;
    float* out=(float*)(uintptr_t)out_ptr;

    float fc = cutoff_hz(s->p_cut);
    float sr = (float)s->sample_rate;
    /* Chamberlin coefficient f = 2 sin(pi fc / sr), clamp for stability */
    float f = 2.0f*fast_sin(PI_F*fc/sr);
    f = clampf(f, 0.0f, 1.5f);
    /* q: higher res -> lower damping */
    float q = 1.0f - s->p_res*0.97f;
    if (q < 0.03f) q = 0.03f;
    int type = (int)(s->p_type*2.999f);
    if (type<0) type=0; if (type>2) type=2;

    for (int n=0;n<frames;n++){
        float xL, xR;
        if (in_ch>=2){ xL=in[n*in_ch+0]; xR=in[n*in_ch+1]; }
        else { xL=in[n*in_ch+0]; xR=xL; }

        /* left */
        s->lpL += f*s->bpL;
        float hpL = xL - s->lpL - q*s->bpL;
        s->bpL += f*hpL;
        float bpL = s->bpL;
        float yL = type==0? s->lpL : (type==1? hpL : bpL);

        /* right */
        s->lpR += f*s->bpR;
        float hpR = xR - s->lpR - q*s->bpR;
        s->bpR += f*hpR;
        float bpR = s->bpR;
        float yR = type==0? s->lpR : (type==1? hpR : bpR);

        if (out_ch>=2){ out[n*out_ch+0]=yL; out[n*out_ch+1]=yR; }
        else { out[n*out_ch+0]=(yL+yR)*0.5f; }
    }
}

__attribute__((export_name("dsp_get_latency")))
int32_t dsp_get_latency(int32_t h){ (void)h; return 0; }
__attribute__((export_name("dsp_get_tail")))
int32_t dsp_get_tail(int32_t h){ (void)h; return 0; }

__attribute__((export_name("dsp_save_state")))
int32_t dsp_save_state(int32_t h, int32_t ptr, int32_t max_bytes){
    if (h<0||h>=MAX_INSTANCES) return 0;
    if (max_bytes < (int32_t)(3*sizeof(float))) return 0;
    float* d=(float*)(uintptr_t)ptr;
    d[0]=g[h].p_cut; d[1]=g[h].p_res; d[2]=g[h].p_type;
    return (int32_t)(3*sizeof(float));
}
__attribute__((export_name("dsp_load_state")))
void dsp_load_state(int32_t h, int32_t ptr, int32_t bytes){
    if (h<0||h>=MAX_INSTANCES) return;
    if (bytes < (int32_t)(3*sizeof(float))) return;
    float* s=(float*)(uintptr_t)ptr;
    g[h].p_cut=s[0]; g[h].p_res=s[1]; g[h].p_type=s[2];
}

static uint8_t heap[1<<20];
static int heap_top=0;
__attribute__((export_name("casc_alloc")))
int32_t casc_alloc(int32_t n){
    int a=(n+7)&~7;
    if (heap_top+a > (int)sizeof(heap)) return 0;
    int p=heap_top; heap_top+=a;
    return (int32_t)((uintptr_t)heap+p);
}
__attribute__((export_name("casc_free")))
void casc_free(int32_t ptr){ (void)ptr; }
