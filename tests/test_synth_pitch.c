/*
 * test_synth_pitch.c — Verify the synth plays the CORRECT pitch per MIDI note.
 *
 * Regression test for "every key plays the same note (C4)". Plays note 60 and
 * note 72 (one octave apart -> frequency should double) through the real
 * libcasc MIDI path, estimates each output frequency by counting zero
 * crossings, and asserts they differ by ~2x.
 *
 * Usage: test_synth_pitch <path-to-synth.casc>
 */
#include "libcasc.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

/* Estimate fundamental frequency via positive-going zero crossings. */
static double estimate_hz(casc_instance_t* inst, double sr, int note) {
    const int block = 512;
    float left[512], right[512];
    float* outs[2] = { left, right };

    /* sustain so frequency is stable; use a plain wave (param 0 = sine default) */
    casc_midi_event_t on = { 0, 0x90, (unsigned char)note, 100, 0 };
    casc_send_midi(inst, &on, 1);

    /* warm up: let attack settle */
    for (int b = 0; b < 6; b++) {
        for (int i = 0; i < block; i++) left[i] = right[i] = 0.0f;
        casc_process(inst, NULL, outs, block);
    }

    /* measure over ~0.25s */
    int meas_blocks = (int)((sr * 0.25) / block);
    long crossings = 0;
    long samples = 0;
    float prev = 0.0f;
    int have_prev = 0;
    for (int b = 0; b < meas_blocks; b++) {
        for (int i = 0; i < block; i++) left[i] = right[i] = 0.0f;
        casc_process(inst, NULL, outs, block);
        for (int i = 0; i < block; i++) {
            float s = left[i];
            if (have_prev && prev <= 0.0f && s > 0.0f) crossings++;
            prev = s; have_prev = 1;
            samples++;
        }
    }
    /* note off + flush so next note starts clean */
    casc_midi_event_t off = { 0, 0x80, (unsigned char)note, 0, 0 };
    casc_send_midi(inst, &off, 1);
    for (int b = 0; b < 200; b++) {
        for (int i = 0; i < block; i++) left[i] = right[i] = 0.0f;
        casc_process(inst, NULL, outs, block);
    }

    double seconds = (double)samples / sr;
    return (double)crossings / seconds;
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s synth.casc\n", argv[0]); return 2; }
    char err[512] = {0};
    casc_plugin_t* plugin = casc_load(argv[1], err, sizeof(err));
    if (!plugin) { fprintf(stderr, "load failed: %s\n", err); return 1; }

    const double sr = 48000.0;
    casc_instance_t* inst = casc_instantiate(plugin, sr, 512);
    if (!inst) { fprintf(stderr, "instantiate failed\n"); return 1; }

    double hz60 = estimate_hz(inst, sr, 60); /* C4  ~261.63 Hz */
    double hz72 = estimate_hz(inst, sr, 72); /* C5  ~523.25 Hz */
    double hz48 = estimate_hz(inst, sr, 48); /* C3  ~130.81 Hz */

    printf("note 48 -> %.1f Hz (expect ~130.8)\n", hz48);
    printf("note 60 -> %.1f Hz (expect ~261.6)\n", hz60);
    printf("note 72 -> %.1f Hz (expect ~523.3)\n", hz72);

    int fail = 0;
    /* note 72 should be ~2x note 60 */
    double ratio = (hz60 > 1.0) ? hz72 / hz60 : 0.0;
    printf("ratio 72/60 = %.3f (expect ~2.0)\n", ratio);
    if (ratio < 1.8 || ratio > 2.2) {
        fprintf(stderr, "FAIL: octave ratio wrong (%.3f) — notes not tracking pitch\n", ratio);
        fail = 1;
    }
    /* absolute sanity: note 60 within 15%% of 261.6 Hz */
    if (hz60 < 222.0 || hz60 > 301.0) {
        fprintf(stderr, "FAIL: note 60 frequency %.1f Hz far from 261.6 Hz\n", hz60);
        fail = 1;
    }

    casc_destroy_instance(inst);
    casc_unload(plugin);
    if (fail) return 1;
    printf("PASS: synth tracks pitch per MIDI note\n");
    return 0;
}
