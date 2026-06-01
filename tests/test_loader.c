/*
 * test_loader.c — Unit tests for ZIP archive loading
 *
 * Creates temporary .casc files in-memory to test the loader.
 */

#include "casc_internal.h"
#include "miniz.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define TEST(name) static void name(void)
#define RUN(name) do { printf("  %-40s ", #name); name(); printf("PASS\n"); } while(0)

/* Minimal valid manifest JSON */
static const char* MANIFEST_JSON =
    "{\"casc_version\":\"0.1\",\"id\":\"test.loader\",\"name\":\"Test\","
    "\"version\":\"1.0.0\",\"vendor\":\"Test\",\"category\":\"utility\","
    "\"features\":[],\"latency_frames\":0,\"tail_seconds\":0.0,"
    "\"hard_realtime\":true,\"params\":[]}";

/* Minimal valid wasm (magic + version only, not a real module) */
static const uint8_t FAKE_WASM[] = {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};

static const char* TEST_FILE = "test_loader_temp.casc";
static const char* TEST_FILE_NO_MANIFEST = "test_loader_no_manifest.casc";
static const char* TEST_FILE_NO_WASM = "test_loader_no_wasm.casc";
static const char* TEST_FILE_TRAVERSAL = "test_loader_traversal.casc";

/* Helper: create a ZIP file with given entries */
static void create_test_casc(const char* filename,
                              const char* manifest, size_t manifest_len,
                              const uint8_t* wasm, size_t wasm_len,
                              int include_manifest, int include_wasm,
                              const char* extra_name) {
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    mz_zip_writer_init_file(&zip, filename, 0);

    if (include_manifest && manifest) {
        mz_zip_writer_add_mem(&zip, "manifest.json", manifest, manifest_len,
                               MZ_BEST_COMPRESSION);
    }
    if (include_wasm && wasm) {
        mz_zip_writer_add_mem(&zip, "dsp.wasm", wasm, wasm_len,
                               MZ_BEST_COMPRESSION);
    }
    if (extra_name) {
        mz_zip_writer_add_mem(&zip, extra_name, "data", 4, MZ_BEST_COMPRESSION);
    }

    mz_zip_writer_finalize_archive(&zip);
    mz_zip_writer_end(&zip);
}

/* -------------------------------------------------------------------------- */
/*  Tests                                                                     */
/* -------------------------------------------------------------------------- */

TEST(test_load_valid) {
    create_test_casc(TEST_FILE,
                      MANIFEST_JSON, strlen(MANIFEST_JSON),
                      FAKE_WASM, sizeof(FAKE_WASM),
                      1, 1, NULL);

    uint8_t* wasm = NULL;
    size_t wasm_len = 0;
    char* manifest = NULL;
    size_t manifest_len = 0;
    char err[256] = {0};

    casc_error_t err_code = casc_loader_extract(TEST_FILE,
        &wasm, &wasm_len, &manifest, &manifest_len, err, sizeof(err));

    assert(err_code == CASC_OK);
    assert(manifest != NULL);
    assert(manifest_len > 0);
    assert(wasm != NULL);
    assert(wasm_len == sizeof(FAKE_WASM));
    assert(memcmp(wasm, FAKE_WASM, sizeof(FAKE_WASM)) == 0);

    free(manifest);
    free(wasm);
    remove(TEST_FILE);
}

TEST(test_load_missing_manifest) {
    create_test_casc(TEST_FILE_NO_MANIFEST,
                      NULL, 0,
                      FAKE_WASM, sizeof(FAKE_WASM),
                      0, 1, NULL);

    uint8_t* wasm = NULL;
    size_t wasm_len = 0;
    char* manifest = NULL;
    size_t manifest_len = 0;
    char err[256] = {0};

    casc_error_t err_code = casc_loader_extract(TEST_FILE_NO_MANIFEST,
        &wasm, &wasm_len, &manifest, &manifest_len, err, sizeof(err));

    assert(err_code == CASC_ERR_INVALID_ARCHIVE);

    remove(TEST_FILE_NO_MANIFEST);
}

TEST(test_load_missing_wasm) {
    create_test_casc(TEST_FILE_NO_WASM,
                      MANIFEST_JSON, strlen(MANIFEST_JSON),
                      NULL, 0,
                      1, 0, NULL);

    uint8_t* wasm = NULL;
    size_t wasm_len = 0;
    char* manifest = NULL;
    size_t manifest_len = 0;
    char err[256] = {0};

    casc_error_t err_code = casc_loader_extract(TEST_FILE_NO_WASM,
        &wasm, &wasm_len, &manifest, &manifest_len, err, sizeof(err));

    assert(err_code == CASC_ERR_INVALID_ARCHIVE);
    if (manifest) free(manifest);

    remove(TEST_FILE_NO_WASM);
}

TEST(test_load_path_traversal) {
    create_test_casc(TEST_FILE_TRAVERSAL,
                      MANIFEST_JSON, strlen(MANIFEST_JSON),
                      FAKE_WASM, sizeof(FAKE_WASM),
                      1, 1, "../evil.txt");

    uint8_t* wasm = NULL;
    size_t wasm_len = 0;
    char* manifest = NULL;
    size_t manifest_len = 0;
    char err[256] = {0};

    casc_error_t err_code = casc_loader_extract(TEST_FILE_TRAVERSAL,
        &wasm, &wasm_len, &manifest, &manifest_len, err, sizeof(err));

    assert(err_code == CASC_ERR_SECURITY);

    remove(TEST_FILE_TRAVERSAL);
}

TEST(test_load_nonexistent) {
    uint8_t* wasm = NULL;
    size_t wasm_len = 0;
    char* manifest = NULL;
    size_t manifest_len = 0;
    char err[256] = {0};

    casc_error_t err_code = casc_loader_extract("nonexistent.casc",
        &wasm, &wasm_len, &manifest, &manifest_len, err, sizeof(err));

    assert(err_code == CASC_ERR_INVALID_ARCHIVE);
}

TEST(test_read_manifest_only) {
    create_test_casc(TEST_FILE,
                      MANIFEST_JSON, strlen(MANIFEST_JSON),
                      FAKE_WASM, sizeof(FAKE_WASM),
                      1, 1, NULL);

    char* manifest = casc_loader_read_manifest_only(TEST_FILE);
    assert(manifest != NULL);
    assert(strstr(manifest, "test.loader") != NULL);
    free(manifest);

    remove(TEST_FILE);
}

/* -------------------------------------------------------------------------- */
/*  Main                                                                      */
/* -------------------------------------------------------------------------- */

int main(void) {
    printf("test_loader:\n");
    RUN(test_load_valid);
    RUN(test_load_missing_manifest);
    RUN(test_load_missing_wasm);
    RUN(test_load_path_traversal);
    RUN(test_load_nonexistent);
    RUN(test_read_manifest_only);
    printf("All loader tests passed.\n");
    return 0;
}
