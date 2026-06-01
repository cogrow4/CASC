/*
 * casc_pack.c — CLI tool to create .casc archives
 *
 * Usage: casc-pack <input-dir> <output.casc>
 *
 * Validates the directory structure, manifest, then creates a ZIP archive.
 */

#include "libcasc.h"
#include "miniz.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#define MAX_FILES 256

typedef struct {
    char rel_path[512];   /* path inside the archive */
    char full_path[1024]; /* path on disk */
} pack_entry_t;

static int g_entry_count = 0;
static pack_entry_t g_entries[MAX_FILES];

static void add_entry(const char* base_dir, const char* rel_prefix, const char* name) {
    if (g_entry_count >= MAX_FILES) return;
    pack_entry_t* e = &g_entries[g_entry_count++];
    if (rel_prefix[0])
        snprintf(e->rel_path, sizeof(e->rel_path), "%s/%s", rel_prefix, name);
    else
        snprintf(e->rel_path, sizeof(e->rel_path), "%s", name);
    snprintf(e->full_path, sizeof(e->full_path), "%s/%s", base_dir, e->rel_path);
}

static void scan_dir_recursive(const char* base_dir, const char* rel_prefix) {
    char full[1024];
    if (rel_prefix[0])
        snprintf(full, sizeof(full), "%s/%s", base_dir, rel_prefix);
    else
        snprintf(full, sizeof(full), "%s", base_dir);

    DIR* dir = opendir(full);
    if (!dir) return;

    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char child_full[1024];
        snprintf(child_full, sizeof(child_full), "%s/%s", full, ent->d_name);

        struct stat st;
        if (stat(child_full, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            char child_rel[512];
            if (rel_prefix[0])
                snprintf(child_rel, sizeof(child_rel), "%s/%s", rel_prefix, ent->d_name);
            else
                snprintf(child_rel, sizeof(child_rel), "%s", ent->d_name);
            scan_dir_recursive(base_dir, child_rel);
        } else {
            add_entry(base_dir, rel_prefix, ent->d_name);
        }
    }
    closedir(dir);
}

static int validate_manifest(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Error: Cannot open manifest.json at %s\n", path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* json = (char*)malloc((size_t)size + 1);
    fread(json, 1, (size_t)size, f);
    json[size] = '\0';
    fclose(f);

    cJSON* root = cJSON_Parse(json);
    free(json);
    if (!root) {
        fprintf(stderr, "Error: manifest.json is not valid JSON\n");
        return 1;
    }

    /* Check required fields */
    const char* required[] = {
        "casc_version", "id", "name", "version", "vendor",
        "category", "features", "latency_frames", "tail_seconds",
        "hard_realtime", "params", NULL
    };
    for (int i = 0; required[i]; i++) {
        if (!cJSON_GetObjectItemCaseSensitive(root, required[i])) {
            fprintf(stderr, "Error: manifest.json missing required field '%s'\n", required[i]);
            cJSON_Delete(root);
            return 1;
        }
    }

    cJSON_Delete(root);
    printf("  ✓ manifest.json is valid\n");
    return 0;
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: casc-pack <input-dir> <output.casc>\n");
        fprintf(stderr, "\nCreates a .casc archive from a directory containing:\n");
        fprintf(stderr, "  manifest.json   (required)\n");
        fprintf(stderr, "  dsp.wasm        (required)\n");
        fprintf(stderr, "  presets/         (optional)\n");
        fprintf(stderr, "  assets/          (optional)\n");
        return 1;
    }

    const char* input_dir = argv[1];
    const char* output_file = argv[2];

    /* Check input directory exists */
    struct stat st;
    if (stat(input_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Error: '%s' is not a directory\n", input_dir);
        return 1;
    }

    /* Check required files exist */
    char manifest_path[1024], wasm_path[1024];
    snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.json", input_dir);
    snprintf(wasm_path, sizeof(wasm_path), "%s/dsp.wasm", input_dir);

    if (stat(manifest_path, &st) != 0) {
        fprintf(stderr, "Error: manifest.json not found in '%s'\n", input_dir);
        return 1;
    }
    if (stat(wasm_path, &st) != 0) {
        fprintf(stderr, "Error: dsp.wasm not found in '%s'\n", input_dir);
        return 1;
    }

    printf("Packing %s → %s\n", input_dir, output_file);

    /* Validate manifest */
    if (validate_manifest(manifest_path) != 0) return 1;

    /* Scan all files */
    g_entry_count = 0;
    scan_dir_recursive(input_dir, "");

    printf("  Found %d files\n", g_entry_count);

    /* Create ZIP archive */
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_writer_init_file(&zip, output_file, 0)) {
        fprintf(stderr, "Error: Cannot create output file '%s'\n", output_file);
        return 1;
    }

    for (int i = 0; i < g_entry_count; i++) {
        pack_entry_t* e = &g_entries[i];

        /* Security: reject any path traversal */
        if (strstr(e->rel_path, "..") != NULL) {
            fprintf(stderr, "Error: Unsafe path '%s'\n", e->rel_path);
            mz_zip_writer_end(&zip);
            return 1;
        }

        if (!mz_zip_writer_add_file(&zip, e->rel_path, e->full_path, NULL, 0,
                                      MZ_BEST_COMPRESSION)) {
            fprintf(stderr, "Error: Failed to add '%s' to archive\n", e->rel_path);
            mz_zip_writer_end(&zip);
            return 1;
        }
        printf("  + %s\n", e->rel_path);
    }

    if (!mz_zip_writer_finalize_archive(&zip)) {
        fprintf(stderr, "Error: Failed to finalize archive\n");
        mz_zip_writer_end(&zip);
        return 1;
    }
    mz_zip_writer_end(&zip);

    printf("✓ Created %s (%d files)\n", output_file, g_entry_count);
    return 0;
}
