/*
 * synth.c — CASC plugin: CASC Poly (polyphonic subtractive synth)
 *
 * 16-voice polyphonic synth. MIDI notes arrive via dsp_send_midi (called by
 * the host before each dsp_process block). Per-voice oscillator (sine/saw/
 * square/triangle) + linear ADSR; a shared state-variable lowpass shapes the
 * summed output. Ignores audio input.
 *
 * Build:
 *   clang --target=wasm32 -O3 -nostdlib -Wl,--no-entry -Wl,--export-dynamic \
 *         synth.c -o dsp.wasm
 *
 * MIDI event layout (8 bytes each, packed in wasm memory):
 *   [0..3] int32 frame_offset (LE)   [4] status   [5] data1   [6] data2   [7] pad
 */
#include <stdint.h>

/* -------------------------------------------------------------------------- */
/*  Mini math (no libc)                                                        */
/* -------------------------------------------------------------------------- */
#define PI_F  3.14159265358979f
#define TWO_PI_F 6.28318530717959f

static inline float clampf(float x,float lo,float hi){return x<lo?lo:(x>hi?hi:x);}
static inline float fabsf_(float x){return x<0.0f?-x:x;}

/* sine on [-PI,PI], parabolic approx with refinement */
static float fast_sin(float x){
    while (x >  PI_F) x -= TWO_PI_F;
    while (x < -PI_F) x += TWO_PI_F;
    float y = 1.27323954f*x - 0.405284735f*x*fabsf_(x);
    y = 0.225f*(y*fabsf_(y) - y) + y;
    return y;
}

/* 2^x for general x>=0 (used for both note->freq and cutoff mapping). */
static float pow2f(float x){
    if (x < 0.0f) x = 0.0f;
    int i = (int)x;
    float f = x - (float)i;
    float p = 1.0f + f*(0.6931472f + f*(0.2402265f + f*(0.0555041f + f*0.0096181f)));
    float r = p;
    for (int k=0;k<i;k++) r *= 2.0f;
    return r;
}

/* MIDI note -> Hz: 440 * 2^((note-69)/12) */
static float note_to_hz(int note){
    return 440.0f * pow2f(((float)note - 69.0f) * (1.0f/12.0f));
}

/* cutoff normalised -> Hz, 20 .. ~20000 ; 1000^v = 2^(v*9.96578) */
static float cutoff_hz(float v){ return 20.0f * pow2f(v * 9.96578f); }

/* -------------------------------------------------------------------------- */
/*  Voice + instance                                                           */
/* -------------------------------------------------------------------------- */
enum { ENV_IDLE=0, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };

typedef struct {
    int   active;
    int   note;
    float phase;     /* 0..1 */
    float phase_inc; /* per sample */
    float vel;       /* 0..1 */
    int   stage;
    float env;       /* current envelope level 0..1 */
    uint32_t age;    /* for voice stealing */
} Voice;

#define NUM_VOICES 16

typedef struct {
    Voice v[NUM_VOICES];
    /* params (normalised) */
    float p_wave, p_cut, p_res, p_att, p_dec, p_sus, p_rel, p_gain;
    /* shared output lowpass (SVF) */
    float lp, bp;
    double sample_rate;
    uint32_t clock;
    int active;
} Synth;

#define MAX_INSTANCES 4
static Synth g[MAX_INSTANCES];
static int g_next = 0;

static void synth_init(Synth* s, double sr){
    s->sample_rate = sr;
    s->lp = s->bp = 0.0f;
    s->clock = 0;
    for (int i=0;i<NUM_VOICES;i++){
        s->v[i].active=0; s->v[i].stage=ENV_IDLE; s->v[i].env=0.0f;
        s->v[i].phase=0.0f; s->v[i].phase_inc=0.0f; s->v[i].note=-1; s->v[i].vel=0.0f; s->v[i].age=0;
    }
}

/* -------------------------------------------------------------------------- */
/*  Exports                                                                    */
/* -------------------------------------------------------------------------- */
__attribute__((export_name("dsp_create")))
int32_t dsp_create(double sr, int32_t mbs){
    (void)mbs;
    if (g_next>=MAX_INSTANCES) return -1;
    int h=g_next++;
    Synth* s=&g[h];
    s->p_wave=0.0f; s->p_cut=0.6f; s->p_res=0.2f;
    s->p_att=0.05f; s->p_dec=0.3f; s->p_sus=0.7f; s->p_rel=0.3f; s->p_gain=0.8f;
    s->active=1;
    synth_init(s, sr);
    return h;
}

__attribute__((export_name("dsp_destroy")))
void dsp_destroy(int32_t h){ (void)h; }

__attribute__((export_name("dsp_reset")))
void dsp_reset(int32_t h, double sr, int32_t mbs){
    (void)mbs;
    if (h<0||h>=MAX_INSTANCES) return;
    synth_init(&g[h], sr);
}

__attribute__((export_name("dsp_set_param")))
void dsp_set_param(int32_t h, int32_t id, double val){
    if (h<0||h>=MAX_INSTANCES) return;
    float v=clampf((float)val,0.0f,1.0f);
    Synth* s=&g[h];
    switch(id){
        case 0: s->p_wave=v; break;
        case 1: s->p_cut=v;  break;
        case 2: s->p_res=v;  break;
        case 3: s->p_att=v;  break;
        case 4: s->p_dec=v;  break;
        case 5: s->p_sus=v;  break;
        case 6: s->p_rel=v;  break;
        case 7: s->p_gain=v; break;
        default: break;
    }
}

__attribute__((export_name("dsp_get_param")))
double dsp_get_param(int32_t h, int32_t id){
    if (h<0||h>=MAX_INSTANCES) return 0.0;
    Synth* s=&g[h];
    switch(id){
        case 0: return s->p_wave; case 1: return s->p_cut;  case 2: return s->p_res;
        case 3: return s->p_att;  case 4: return s->p_dec;  case 5: return s->p_sus;
        case 6: return s->p_rel;  case 7: return s->p_gain;
    }
    return 0.0;
}

/* ---- voice allocation ---- */
static int alloc_voice(Synth* s){
    /* prefer an idle voice */
    for (int i=0;i<NUM_VOICES;i++) if (!s->v[i].active) return i;
    /* else steal the oldest */
    int oldest=0; uint32_t best=s->v[0].age;
    for (int i=1;i<NUM_VOICES;i++) if (s->v[i].age < best){best=s->v[i].age;oldest=i;}
    return oldest;
}

static void note_on(Synth* s, int note, int vel){
    int i=alloc_voice(s);
    Voice* v=&s->v[i];
    v->active=1; v->note=note; v->vel=(float)vel/127.0f;
    v->phase=0.0f;
    v->phase_inc = note_to_hz(note) / (float)s->sample_rate;
    v->stage=ENV_ATTACK; v->env=0.0f;
    v->age = ++s->clock;
}

static void note_off(Synth* s, int note){
    for (int i=0;i<NUM_VOICES;i++){
        if (s->v[i].active && s->v[i].note==note && s->v[i].stage!=ENV_RELEASE)
            s->v[i].stage=ENV_RELEASE;
    }
}

/* ---- MIDI input (required for instruments) ---- */
__attribute__((export_name("dsp_send_midi")))
void dsp_send_midi(int32_t h, int32_t events_ptr, int32_t count){
    if (h<0||h>=MAX_INSTANCES) return;
    Synth* s=&g[h];
    uint8_t* p=(uint8_t*)(uintptr_t)events_ptr;
    for (int i=0;i<count;i++){
        uint8_t status=p[i*8+4];
        uint8_t d1=p[i*8+5];
        uint8_t d2=p[i*8+6];
        uint8_t hi = status & 0xF0;
        if (hi==0x90){ if (d2>0) note_on(s,d1,d2); else note_off(s,d1); }
        else if (hi==0x80){ note_off(s,d1); }
        /* other messages ignored */
    }
}

/* ---- oscillator ---- */
static inline float osc(int wave, float phase){
    /* phase 0..1 */
    switch(wave){
        case 0: return fast_sin(phase*TWO_PI_F - PI_F); /* sine */
        case 1: return 2.0f*phase - 1.0f;               /* saw */
        case 2: return phase<0.5f ? 1.0f : -1.0f;       /* square */
        default: { /* triangle */
            float t = phase<0.5f ? (phase*2.0f) : (2.0f-phase*2.0f); /* 0..1..0 */
            return t*2.0f - 1.0f;
        }
    }
}

__attribute__((export_name("dsp_process")))
void dsp_process(int32_t h, int32_t in_ptr, int32_t out_ptr,
                 int32_t frames, int32_t in_ch, int32_t out_ch){
    (void)in_ptr; (void)in_ch;
    if (h<0||h>=MAX_INSTANCES) return;
    Synth* s=&g[h];
    float* out=(float*)(uintptr_t)out_ptr;
    float sr=(float)s->sample_rate;

    int wave = (int)(s->p_wave*3.999f); if (wave<0)wave=0; if (wave>3)wave=3;

    /* ADSR per-sample increments (times 1ms..~3s, quadratic feel) */
    float aT = 0.001f + s->p_att*s->p_att*3.0f;
    float dT = 0.001f + s->p_dec*s->p_dec*3.0f;
    float rT = 0.001f + s->p_rel*s->p_rel*3.0f;
    float aInc = 1.0f/(aT*sr);
    float dInc = 1.0f/(dT*sr);
    float rInc = 1.0f/(rT*sr);
    float sus  = s->p_sus;
    float gain = s->p_gain;

    /* shared lowpass coeffs */
    float fc = cutoff_hz(s->p_cut);
    float f = 2.0f*fast_sin(PI_F*fc/sr); f=clampf(f,0.0f,1.5f);
    float q = 1.0f - s->p_res*0.97f; if (q<0.03f) q=0.03f;

    for (int n=0;n<frames;n++){
        float mix=0.0f;
        for (int i=0;i<NUM_VOICES;i++){
            Voice* v=&s->v[i];
            if (!v->active) continue;

            /* envelope */
            switch(v->stage){
                case ENV_ATTACK:
                    v->env += aInc;
                    if (v->env>=1.0f){v->env=1.0f; v->stage=ENV_DECAY;}
                    break;
                case ENV_DECAY:
                    v->env -= dInc*(1.0f - sus);
                    if (v->env<=sus){v->env=sus; v->stage=ENV_SUSTAIN;}
                    break;
                case ENV_SUSTAIN:
                    v->env=sus;
                    if (sus<=0.0001f){v->active=0; v->stage=ENV_IDLE;}
                    break;
                case ENV_RELEASE:
                    v->env -= rInc;
                    if (v->env<=0.0f){v->env=0.0f; v->active=0; v->stage=ENV_IDLE;}
                    break;
                default: v->active=0; break;
            }
            if (!v->active) continue;

            float sample = osc(wave, v->phase) * v->env * v->vel;
            mix += sample;

            v->phase += v->phase_inc;
            if (v->phase>=1.0f) v->phase -= 1.0f;
        }

        /* scale down for polyphony headroom */
        mix *= 0.25f;

        /* shared SVF lowpass on the mix */
        s->lp += f*s->bp;
        float hp = mix - s->lp - q*s->bp;
        s->bp += f*hp;
        float y = s->lp * gain;

        if (out_ch>=2){ out[n*out_ch+0]=y; out[n*out_ch+1]=y; }
        else { out[n*out_ch+0]=y; }
    }
}

__attribute__((export_name("dsp_get_latency")))
int32_t dsp_get_latency(int32_t h){ (void)h; return 0; }
__attribute__((export_name("dsp_get_tail")))
int32_t dsp_get_tail(int32_t h){
    if (h<0||h>=MAX_INSTANCES) return 0;
    return (int32_t)(g[h].sample_rate * 1.0); /* ~1s release tail */
}

/* ---- state: 8 params ---- */
__attribute__((export_name("dsp_save_state")))
int32_t dsp_save_state(int32_t h, int32_t ptr, int32_t max_bytes){
    if (h<0||h>=MAX_INSTANCES) return 0;
    if (max_bytes < (int32_t)(8*sizeof(float))) return 0;
    float* d=(float*)(uintptr_t)ptr;
    Synth* s=&g[h];
    d[0]=s->p_wave; d[1]=s->p_cut; d[2]=s->p_res; d[3]=s->p_att;
    d[4]=s->p_dec;  d[5]=s->p_sus; d[6]=s->p_rel; d[7]=s->p_gain;
    return (int32_t)(8*sizeof(float));
}
__attribute__((export_name("dsp_load_state")))
void dsp_load_state(int32_t h, int32_t ptr, int32_t bytes){
    if (h<0||h>=MAX_INSTANCES) return;
    if (bytes < (int32_t)(8*sizeof(float))) return;
    float* s2=(float*)(uintptr_t)ptr;
    Synth* s=&g[h];
    s->p_wave=s2[0]; s->p_cut=s2[1]; s->p_res=s2[2]; s->p_att=s2[3];
    s->p_dec=s2[4];  s->p_sus=s2[5]; s->p_rel=s2[6]; s->p_gain=s2[7];
}

/* ---- bump allocator ---- */
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
