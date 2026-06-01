/*
 * casc_validate.c — CLI tool to validate .casc files
 *
 * Usage: casc-validate <file.casc>
 *
 * Checks:
 *   1. ZIP integrity
 *   2. Required files present (manifest.json, dsp.wasm)
 *   3. Manifest JSON schema validation
 *   4. Wasm module compiles and has all required exports
 *   5. Security (no path traversal)
 *
 * Exit code: 0 = valid, 1 = errors found
 */

#include "libcasc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char* argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: casc-validate <file.casc>\n");
        fprintf(stderr, "\nValidates a .casc plugin file:\n");
        fprintf(stderr, "  - ZIP archive integrity\n");
        fprintf(stderr, "  - Required files (manifest.json, dsp.wasm)\n");
        fprintf(stderr, "  - Manifest schema validation\n");
        fprintf(stderr, "  - Wasm export validation\n");
        fprintf(stderr, "  - Security checks\n");
        return 1;
    }

    const char* path = argv[1];
    int errors = 0;
    int warnings = 0;

    printf("Validating: %s\n", path);
    printf("─────────────────────────────────────\n");

    /* Step 1: Try to read manifest */
    printf("\n[1/4] Checking manifest... ");
    char* manifest = casc_read_manifest(path);
    if (!manifest) {
        printf("FAIL\n");
        fprintf(stderr, "  ✗ Cannot read manifest.json from archive\n");
        errors++;
    } else {
        printf("OK\n");
        printf("  ✓ manifest.json found and readable\n");

        /* Print some info */
        /* We'll rely on the full load to validate the manifest */
        free(manifest);
    }

    /* Step 2: Full load (validates archive, manifest, wasm compilation, exports) */
    printf("\n[2/4] Loading plugin... ");
    char err_buf[512] = {0};
    casc_plugin_t* plugin = casc_load(path, err_buf, sizeof(err_buf));
    if (!plugin) {
        printf("FAIL\n");
        fprintf(stderr, "  ✗ %s\n", err_buf);
        errors++;
        goto done;
    }
    printf("OK\n");

    /* Print plugin info */
    printf("  ✓ Plugin ID:      %s\n", casc_plugin_get_id(plugin));
    printf("  ✓ Name:           %s\n", casc_plugin_get_name(plugin));
    printf("  ✓ Vendor:         %s\n", casc_plugin_get_vendor(plugin));
    printf("  ✓ Version:        %s\n", casc_plugin_get_version(plugin));
    printf("  ✓ Category:       %s\n", casc_plugin_get_category(plugin));
    printf("  ✓ Parameters:     %d\n", casc_plugin_get_param_count(plugin));
    printf("  ✓ Audio inputs:   %d port(s)\n", casc_plugin_get_audio_input_count(plugin));
    printf("  ✓ Audio outputs:  %d port(s)\n", casc_plugin_get_audio_output_count(plugin));
    printf("  ✓ MIDI input:     %s\n", casc_plugin_has_midi_input(plugin) ? "yes" : "no");
    printf("  ✓ Latency:        %d frames\n", casc_plugin_get_latency_frames(plugin));
    printf("  ✓ Tail:           %.2f seconds\n", casc_plugin_get_tail_seconds(plugin));

    int feat_count = casc_plugin_get_feature_count(plugin);
    if (feat_count > 0) {
        printf("  ✓ Features:       ");
        for (int i = 0; i < feat_count; i++) {
            printf("%s%s", casc_plugin_get_feature(plugin, i),
                   i < feat_count - 1 ? ", " : "");
        }
        printf("\n");
    }

    /* Step 3: Try to instantiate */
    printf("\n[3/4] Instantiating... ");
    casc_instance_t* inst = casc_instantiate(plugin, 48000.0, 512);
    if (!inst) {
        printf("FAIL\n");
        fprintf(stderr, "  ✗ Failed to create instance (48000 Hz, 512 frames)\n");
        errors++;
    } else {
        printf("OK\n");
        printf("  ✓ Instance created at 48000 Hz / 512 frames\n");

        /* Step 4: Quick smoke test — process silence */
        printf("\n[4/4] Smoke test... ");

        int out_ch = casc_plugin_get_audio_output_count(plugin) > 0 ? 2 : 2;
        int in_ch = casc_plugin_get_audio_input_count(plugin) > 0 ? 2 : 2;

        float in_l[512] = {0};
        float in_r[512] = {0};
        float out_l[512] = {0};
        float out_r[512] = {0};
        const float* inputs[2] = { in_l, in_r };
        float* outputs[2] = { out_l, out_r };

        casc_process(inst, inputs, outputs, 512);
        printf("OK\n");
        printf("  ✓ Processed 512 frames of silence without crash\n");

        /* Check latency/tail exports */
        int lat = casc_get_latency(inst);
        int tail = casc_get_tail(inst);
        printf("  ✓ Runtime latency: %d frames\n", lat);
        printf("  ✓ Runtime tail:    %d frames\n", tail);

        casc_destroy_instance(inst);
    }

done:
    if (plugin) casc_unload(plugin);

    printf("\n─────────────────────────────────────\n");
    if (errors == 0) {
        printf("✓ VALID — %s passed all checks", path);
        if (warnings > 0) printf(" (%d warning(s))", warnings);
        printf("\n");
        return 0;
    } else {
        printf("✗ INVALID — %d error(s), %d warning(s)\n", errors, warnings);
        return 1;
    }
}
