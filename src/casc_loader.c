/*
 * casc_loader.c — ZIP archive loading and extraction
 *
 * Opens .casc files (ZIP archives), validates structure,
 * extracts manifest.json and dsp.wasm into memory.
 */

#include "casc_internal.h"
#include "miniz.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/*  Security checks                                                           */
/* -------------------------------------------------------------------------- */

static bool path_is_safe(const char* filename) {
    /* Reject absolute paths */
    if (filename[0] == '/' || filename[0] == '\\') return false;

    /* Reject path traversal */
    if (strstr(filename, "../") != NULL) return false;
    if (strstr(filename, "..\\") != NULL) return false;
    if (strcmp(filename, "..") == 0) return false;

    /* Reject Windows drive letters */
    if (strlen(filename) >= 2 && filename[1] == ':') return false;

    return true;
}

/* -------------------------------------------------------------------------- */
/*  Extract a single file from a ZIP archive to heap                          */
/* -------------------------------------------------------------------------- */

static void* extract_file(mz_zip_archive* zip, const char* name,
                           size_t* out_size, char* err_buf, size_t err_buf_len) {
    int idx = mz_zip_reader_locate_file(zip, name, NULL, 0);
    if (idx < 0) {
        if (err_buf) snprintf(err_buf, err_buf_len, "File '%s' not found in archive", name);
        return NULL;
    }

    mz_zip_archive_file_stat stat;
    if (!mz_zip_reader_file_stat(zip, idx, &stat)) {
        if (err_buf) snprintf(err_buf, err_buf_len, "Cannot stat '%s' in archive", name);
        return NULL;
    }

    /* Security: check filename in archive */
    if (!path_is_safe(stat.m_filename)) {
        if (err_buf) snprintf(err_buf, err_buf_len, "Unsafe path in archive: '%s'", stat.m_filename);
        return NULL;
    }

    size_t size = 0;
    void* data = mz_zip_reader_extract_to_heap(zip, idx, &size, 0);
    if (!data) {
        if (err_buf) snprintf(err_buf, err_buf_len, "Failed to extract '%s'", name);
        return NULL;
    }

    *out_size = size;
    return data;
}

/* -------------------------------------------------------------------------- */
/*  Full extraction (manifest + wasm)                                         */
/* -------------------------------------------------------------------------- */

casc_error_t casc_loader_extract(const char* path,
                                  uint8_t** out_wasm, size_t* out_wasm_len,
                                  char** out_manifest_json, size_t* out_manifest_len,
                                  char* err_buf, size_t err_buf_len) {
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));

    if (!mz_zip_reader_init_file(&zip, path, 0)) {
        if (err_buf) snprintf(err_buf, err_buf_len, "Cannot open ZIP archive: %s", path);
        return CASC_ERR_INVALID_ARCHIVE;
    }

    /* Scan all entries for security */
    int n = (int)mz_zip_reader_get_num_files(&zip);
    for (int i = 0; i < n; i++) {
        mz_zip_archive_file_stat stat;
        if (mz_zip_reader_file_stat(&zip, i, &stat)) {
            if (!path_is_safe(stat.m_filename)) {
                if (err_buf) snprintf(err_buf, err_buf_len,
                    "Security: unsafe path '%s' in archive", stat.m_filename);
                mz_zip_reader_end(&zip);
                return CASC_ERR_SECURITY;
            }
        }
    }

    /* Extract manifest.json */
    size_t manifest_size = 0;
    char* manifest = (char*)extract_file(&zip, "manifest.json", &manifest_size, err_buf, err_buf_len);
    if (!manifest) {
        mz_zip_reader_end(&zip);
        return CASC_ERR_INVALID_ARCHIVE;
    }

    /* Null-terminate the manifest JSON */
    char* manifest_z = (char*)realloc(manifest, manifest_size + 1);
    if (!manifest_z) {
        free(manifest);
        mz_zip_reader_end(&zip);
        return CASC_ERR_OUT_OF_MEMORY;
    }
    manifest_z[manifest_size] = '\0';

    /* Extract dsp.wasm */
    size_t wasm_size = 0;
    uint8_t* wasm = (uint8_t*)extract_file(&zip, "dsp.wasm", &wasm_size, err_buf, err_buf_len);
    if (!wasm) {
        free(manifest_z);
        mz_zip_reader_end(&zip);
        return CASC_ERR_INVALID_ARCHIVE;
    }

    mz_zip_reader_end(&zip);

    *out_manifest_json = manifest_z;
    *out_manifest_len = manifest_size;
    *out_wasm = wasm;
    *out_wasm_len = wasm_size;
    return CASC_OK;
}

/* -------------------------------------------------------------------------- */
/*  Manifest-only read                                                        */
/* -------------------------------------------------------------------------- */

char* casc_loader_read_manifest_only(const char* path) {
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));

    if (!mz_zip_reader_init_file(&zip, path, 0)) return NULL;

    size_t size = 0;
    char* data = (char*)extract_file(&zip, "manifest.json", &size, NULL, 0);
    mz_zip_reader_end(&zip);

    if (!data) return NULL;

    /* Null-terminate */
    char* result = (char*)realloc(data, size + 1);
    if (!result) { free(data); return NULL; }
    result[size] = '\0';
    return result;
}

/* -------------------------------------------------------------------------- */
/*  Generic single-entry extraction (used for ui.html, assets, etc.)          */
/* -------------------------------------------------------------------------- */

void* casc_loader_extract_entry(const char* path, const char* entry_name,
                                 size_t* out_len) {
    if (out_len) *out_len = 0;
    if (!path || !entry_name) return NULL;

    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_file(&zip, path, 0)) return NULL;

    size_t size = 0;
    void* data = extract_file(&zip, entry_name, &size, NULL, 0);
    mz_zip_reader_end(&zip);
    if (!data) return NULL;

    /* Null-terminate so callers can treat text entries as C strings. */
    uint8_t* result = (uint8_t*)realloc(data, size + 1);
    if (!result) { free(data); return NULL; }
    result[size] = '\0';

    if (out_len) *out_len = size;
    return result;
}
