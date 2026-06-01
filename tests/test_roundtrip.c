/*
 * test_roundtrip.c — End-to-end test
 *
 * Usage: test_roundtrip <path-to-gain.casc>
 *
 * Tests: load → instantiate → set param → process → save state →
 *        destroy → re-instantiate → load state → verify
 */

#include "libcasc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>

#define FRAMES 256

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: test_roundtrip <gain.casc>\n");
        return 1;
    }
    const char* path = argv[1];
    printf("test_roundtrip (%s):\n", path);

    /* Step 1: Load plugin */
    printf("  [1] Loading plugin...          ");
    char err[512] = {0};
    casc_plugin_t* plugin = casc_load(path, err, sizeof(err));
    assert(plugin != NULL);
    printf("OK\n");

    /* Step 2: Instantiate */
    printf("  [2] Instantiating...           ");
    casc_instance_t* inst = casc_instantiate(plugin, 44100.0, FRAMES);
    assert(inst != NULL);
    printf("OK\n");

    /* Step 3: Set param and process */
    printf("  [3] Set gain=0.75, process...  ");
    casc_set_param(inst, 0, 0.75);

    float in_l[FRAMES], in_r[FRAMES], out_l[FRAMES], out_r[FRAMES];
    for (int i = 0; i < FRAMES; i++) { in_l[i] = 1.0f; in_r[i] = 1.0f; }

    const float* inputs[2] = { in_l, in_r };
    float* outputs[2] = { out_l, out_r };
    casc_process(inst, inputs, outputs, FRAMES);

    for (int i = 0; i < FRAMES; i++) {
        assert(fabsf(out_l[i] - 0.75f) < 1e-4f);
    }
    printf("OK\n");

    /* Step 4: Save state */
    printf("  [4] Saving state...            ");
    size_t state_size = 0;
    void* state = casc_save_state(inst, &state_size);
    assert(state != NULL);
    assert(state_size > 0);
    printf("OK (%zu bytes)\n", state_size);

    /* Step 5: Destroy instance */
    printf("  [5] Destroying instance...     ");
    casc_destroy_instance(inst);
    inst = NULL;
    printf("OK\n");

    /* Step 6: Re-instantiate */
    printf("  [6] Re-instantiating...        ");
    inst = casc_instantiate(plugin, 44100.0, FRAMES);
    assert(inst != NULL);
    printf("OK\n");

    /* Step 7: Verify default param before load */
    printf("  [7] Default param check...     ");
    double val = casc_get_param(inst, 0);
    assert(fabs(val - 1.0) < 1e-6); /* default is 1.0 */
    printf("OK (gain=%.2f)\n", val);

    /* Step 8: Load state */
    printf("  [8] Loading state...           ");
    int rc = casc_load_state(inst, state, state_size);
    assert(rc == CASC_OK);
    printf("OK\n");

    /* Step 9: Verify restored param */
    printf("  [9] Restored param check...    ");
    val = casc_get_param(inst, 0);
    assert(fabs(val - 0.75) < 1e-4);
    printf("OK (gain=%.2f)\n", val);

    /* Step 10: Process again with restored state */
    printf("  [10] Process with restored...  ");
    casc_process(inst, inputs, outputs, FRAMES);
    for (int i = 0; i < FRAMES; i++) {
        assert(fabsf(out_l[i] - 0.75f) < 1e-4f);
    }
    printf("OK\n");

    /* Cleanup */
    free(state);
    casc_destroy_instance(inst);
    casc_unload(plugin);

    printf("All roundtrip tests passed.\n");
    return 0;
}
