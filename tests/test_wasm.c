/*
 * test_wasm.c — Integration tests: load .casc, instantiate, process audio
 *
 * Usage: test_wasm <path-to-gain.casc>
 */

#include "libcasc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>

#define FRAMES 512
#define EPSILON 1e-5f

#define TEST(name) static void name(const char* p)
#define RUN(name) do { printf("  %-40s ", #name); name(path); printf("PASS\n"); } while(0)

TEST(test_load_instantiate) {
    char e[512]={0};
    casc_plugin_t* pl = casc_load(p, e, sizeof(e));
    assert(pl);
    assert(strcmp(casc_plugin_get_id(pl), "org.casc.examples.gain") == 0);
    casc_instance_t* i = casc_instantiate(pl, 48000.0, FRAMES);
    assert(i);
    casc_destroy_instance(i);
    casc_unload(pl);
}

TEST(test_silence) {
    char e[512]={0};
    casc_plugin_t* pl = casc_load(p, e, sizeof(e));
    assert(pl);
    casc_instance_t* i = casc_instantiate(pl, 48000.0, FRAMES);
    assert(i);
    float il[FRAMES]={0}, ir[FRAMES]={0}, ol[FRAMES], or_[FRAMES];
    const float* in[2]={il,ir}; float* out[2]={ol,or_};
    casc_process(i, in, out, FRAMES);
    for(int f=0;f<FRAMES;f++){assert(fabsf(ol[f])<EPSILON);assert(fabsf(or_[f])<EPSILON);}
    casc_destroy_instance(i); casc_unload(pl);
}

TEST(test_gain_half) {
    char e[512]={0};
    casc_plugin_t* pl = casc_load(p, e, sizeof(e));
    assert(pl);
    casc_instance_t* i = casc_instantiate(pl, 48000.0, FRAMES);
    assert(i);
    casc_set_param(i, 0, 0.5);
    assert(fabs(casc_get_param(i, 0) - 0.5) < 1e-6);
    float il[FRAMES], ir[FRAMES], ol[FRAMES], or_[FRAMES];
    for(int f=0;f<FRAMES;f++){il[f]=1.0f;ir[f]=0.8f;}
    const float* in[2]={il,ir}; float* out[2]={ol,or_};
    casc_process(i, in, out, FRAMES);
    for(int f=0;f<FRAMES;f++){assert(fabsf(ol[f]-0.5f)<EPSILON);assert(fabsf(or_[f]-0.4f)<EPSILON);}
    casc_destroy_instance(i); casc_unload(pl);
}

TEST(test_latency_tail) {
    char e[512]={0};
    casc_plugin_t* pl = casc_load(p, e, sizeof(e));
    assert(pl);
    casc_instance_t* i = casc_instantiate(pl, 48000.0, FRAMES);
    assert(i);
    assert(casc_get_latency(i)==0);
    assert(casc_get_tail(i)==0);
    casc_destroy_instance(i); casc_unload(pl);
}

int main(int argc, char* argv[]) {
    if (argc < 2) { fprintf(stderr, "Usage: test_wasm <gain.casc>\n"); return 1; }
    const char* path = argv[1];
    printf("test_wasm (%s):\n", path);
    RUN(test_load_instantiate);
    RUN(test_silence);
    RUN(test_gain_half);
    RUN(test_latency_tail);
    printf("All wasm tests passed.\n");
    return 0;
}
