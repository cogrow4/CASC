/*
 * audio_sanity.c — Feed a sine into an FX plugin and check the output is
 * finite, non-silent, and bounded. Catches NaN/Inf, runaway feedback, and
 * dead plugins. Usage: audio_sanity <plugin.casc>
 */
#include "libcasc.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s plugin.casc\n", argv[0]); return 2; }
    char err[512] = {0};
    casc_plugin_t* p = casc_load(argv[1], err, sizeof(err));
    if (!p) { fprintf(stderr, "load failed: %s\n", err); return 1; }

    const double sr = 48000.0;
    const int block = 256;
    casc_instance_t* inst = casc_instantiate(p, sr, block);
    if (!inst) { fprintf(stderr, "instantiate failed\n"); return 1; }

    int nparams = casc_plugin_get_param_count(p);
    /* exercise a couple of param settings (defaults + a tweak) */
    for (int i = 0; i < nparams; i++) casc_set_param(inst, i, 0.6);

    float inL[256], inR[256], outL[256], outR[256];
    const float* ins[2]  = { inL, inR };
    float* outs[2] = { outL, outR };

    double phase = 0.0, dphi = 2.0 * M_PI * 220.0 / sr;
    float maxabs = 0.0f;
    int nan_seen = 0, nonzero = 0;
    int total_blocks = (int)(sr * 1.0 / block); /* 1 second */

    for (int b = 0; b < total_blocks; b++) {
        for (int i = 0; i < block; i++) {
            float s = (float)sin(phase); phase += dphi;
            if (phase > 2.0*M_PI) phase -= 2.0*M_PI;
            inL[i] = s * 0.5f; inR[i] = s * 0.5f;
            outL[i] = outR[i] = 0.0f;
        }
        casc_process(inst, ins, outs, block);
        for (int i = 0; i < block; i++) {
            for (int ch = 0; ch < 2; ch++) {
                float v = ch ? outR[i] : outL[i];
                if (isnan(v) || isinf(v)) nan_seen = 1;
                float a = fabsf(v);
                if (a > maxabs) maxabs = a;
                if (a > 1e-6f) nonzero = 1;
            }
        }
    }

    printf("  params=%d  max|out|=%.4f  nonzero=%d  nan/inf=%d\n",
           nparams, maxabs, nonzero, nan_seen);

    int ok = 1;
    if (nan_seen)         { fprintf(stderr, "  FAIL: NaN/Inf in output\n"); ok = 0; }
    if (!nonzero)         { fprintf(stderr, "  FAIL: output is silent\n"); ok = 0; }
    if (maxabs > 8.0f)    { fprintf(stderr, "  FAIL: runaway output (%.2f)\n", maxabs); ok = 0; }

    casc_destroy_instance(inst);
    casc_unload(p);
    if (ok) { printf("  PASS\n"); return 0; }
    return 1;
}
