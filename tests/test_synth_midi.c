/*
 * test_synth_midi.c — Verify the synth instrument responds to MIDI.
 *
 * Loads synth.casc, instantiates it, sends a Note On via casc_send_midi,
 * processes a block, and asserts the output is non-silent. Then sends a
 * Note Off and processes enough blocks for the release tail to decay, and
 * asserts the output eventually returns to (near) silence.
 *
 * Usage: test_synth_midi <path-to-synth.casc>
 */
#include "libcasc.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s synth.casc\n", argv[0]); return 2; }

    char err[512] = {0};
    casc_plugin_t* plugin = casc_load(argv[1], err, sizeof(err));
    if (!plugin) { fprintf(stderr, "load failed: %s\n", err); return 1; }

    if (!casc_plugin_has_midi_input(plugin)) {
        fprintf(stderr, "FAIL: synth does not report midi_input\n");
        return 1;
    }

    const double sr = 48000.0;
    const int    block = 512;
    casc_instance_t* inst = casc_instantiate(plugin, sr, block);
    if (!inst) { fprintf(stderr, "instantiate failed\n"); return 1; }

    /* stereo output buffers */
    float left[512], right[512];
    float* outs[2] = { left, right };

    /* --- Note On: middle C (60), velocity 100 --- */
    casc_midi_event_t on = { 0, 0x90, 60, 100, 0 };
    casc_send_midi(inst, &on, 1);

    /* Process a few blocks so the attack ramps up. */
    float peak = 0.0f;
    for (int b = 0; b < 8; b++) {
        for (int i = 0; i < block; i++) { left[i] = 0.0f; right[i] = 0.0f; }
        casc_process(inst, NULL, outs, block);
        for (int i = 0; i < block; i++) {
            float a = fabsf(left[i]);
            if (a > peak) peak = a;
        }
    }
    printf("note-on peak amplitude: %.4f\n", peak);
    if (peak < 0.01f) {
        fprintf(stderr, "FAIL: synth produced silence after Note On (peak %.5f)\n", peak);
        return 1;
    }

    /* --- Note Off: let the release tail decay --- */
    casc_midi_event_t off = { 0, 0x80, 60, 0, 0 };
    casc_send_midi(inst, &off, 1);

    float tail_peak = 0.0f;
    /* process ~2 seconds worth of blocks */
    int blocks = (int)((sr * 2.0) / block);
    for (int b = 0; b < blocks; b++) {
        for (int i = 0; i < block; i++) { left[i] = 0.0f; right[i] = 0.0f; }
        casc_process(inst, NULL, outs, block);
        /* track the peak of the LAST block only */
        if (b == blocks - 1) {
            for (int i = 0; i < block; i++) {
                float a = fabsf(left[i]);
                if (a > tail_peak) tail_peak = a;
            }
        }
    }
    printf("post-release tail peak: %.6f\n", tail_peak);
    if (tail_peak > 0.02f) {
        fprintf(stderr, "FAIL: synth did not decay after Note Off (peak %.5f)\n", tail_peak);
        return 1;
    }

    casc_destroy_instance(inst);
    casc_unload(plugin);
    printf("PASS: synth responds to MIDI (note on -> sound, note off -> decay)\n");
    return 0;
}
