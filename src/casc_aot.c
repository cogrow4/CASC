/*
 * casc_aot.c — AOT compilation cache
 *
 * Computes SHA-256 of dsp.wasm, caches compiled modules to disk,
 * and loads them back on subsequent runs.
 */

#include "casc_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* -------------------------------------------------------------------------- */
/*  Minimal SHA-256 implementation (public domain)                            */
/*  Adapted from Brad Conte's crypto-algorithms                              */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint8_t  data[64];
    uint32_t datalen;
    uint64_t bitlen;
    uint32_t state[8];
} sha256_ctx;

static const uint32_t sha256_k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

#define ROTR(x,n)   (((x)>>(n))|((x)<<(32-(n))))
#define CH(x,y,z)   (((x)&(y))^(~(x)&(z)))
#define MAJ(x,y,z)  (((x)&(y))^((x)&(z))^((y)&(z)))
#define EP0(x)       (ROTR(x,2)^ROTR(x,13)^ROTR(x,22))
#define EP1(x)       (ROTR(x,6)^ROTR(x,11)^ROTR(x,25))
#define SIG0(x)      (ROTR(x,7)^ROTR(x,18)^((x)>>3))
#define SIG1(x)      (ROTR(x,17)^ROTR(x,19)^((x)>>10))

static void sha256_transform(sha256_ctx* ctx, const uint8_t data[]) {
    uint32_t a, b, c, d, e, f, g, h, t1, t2, m[64];
    int i;
    for (i = 0; i < 16; i++)
        m[i] = ((uint32_t)data[i*4]<<24)|((uint32_t)data[i*4+1]<<16)|
               ((uint32_t)data[i*4+2]<<8)|((uint32_t)data[i*4+3]);
    for (; i < 64; i++)
        m[i] = SIG1(m[i-2]) + m[i-7] + SIG0(m[i-15]) + m[i-16];
    a=ctx->state[0]; b=ctx->state[1]; c=ctx->state[2]; d=ctx->state[3];
    e=ctx->state[4]; f=ctx->state[5]; g=ctx->state[6]; h=ctx->state[7];
    for (i = 0; i < 64; i++) {
        t1 = h + EP1(e) + CH(e,f,g) + sha256_k[i] + m[i];
        t2 = EP0(a) + MAJ(a,b,c);
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    ctx->state[0]+=a; ctx->state[1]+=b; ctx->state[2]+=c; ctx->state[3]+=d;
    ctx->state[4]+=e; ctx->state[5]+=f; ctx->state[6]+=g; ctx->state[7]+=h;
}

static void sha256_init(sha256_ctx* ctx) {
    ctx->datalen = 0; ctx->bitlen = 0;
    ctx->state[0]=0x6a09e667; ctx->state[1]=0xbb67ae85;
    ctx->state[2]=0x3c6ef372; ctx->state[3]=0xa54ff53a;
    ctx->state[4]=0x510e527f; ctx->state[5]=0x9b05688c;
    ctx->state[6]=0x1f83d9ab; ctx->state[7]=0x5be0cd19;
}

static void sha256_update(sha256_ctx* ctx, const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        ctx->data[ctx->datalen++] = data[i];
        if (ctx->datalen == 64) {
            sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}

static void sha256_final(sha256_ctx* ctx, uint8_t hash[32]) {
    uint32_t i = ctx->datalen;
    ctx->data[i++] = 0x80;
    if (i > 56) {
        while (i < 64) ctx->data[i++] = 0;
        sha256_transform(ctx, ctx->data);
        i = 0;
    }
    while (i < 56) ctx->data[i++] = 0;
    ctx->bitlen += ctx->datalen * 8;
    for (int j = 7; j >= 0; j--)
        ctx->data[56 + (7-j)] = (uint8_t)(ctx->bitlen >> (j*8));
    sha256_transform(ctx, ctx->data);
    for (i = 0; i < 8; i++) {
        hash[i*4+0] = (ctx->state[i]>>24)&0xff;
        hash[i*4+1] = (ctx->state[i]>>16)&0xff;
        hash[i*4+2] = (ctx->state[i]>>8)&0xff;
        hash[i*4+3] = (ctx->state[i])&0xff;
    }
}

/* -------------------------------------------------------------------------- */
/*  Public functions                                                          */
/* -------------------------------------------------------------------------- */

void casc_aot_compute_sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
    sha256_ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, out);
}

static void hex_string(const uint8_t hash[32], char buf[65]) {
    for (int i = 0; i < 32; i++)
        snprintf(buf + i*2, 3, "%02x", hash[i]);
    buf[64] = '\0';
}

/* -------------------------------------------------------------------------- */
/*  Cache directory                                                           */
/* -------------------------------------------------------------------------- */

casc_error_t casc_aot_get_cache_dir(const char* plugin_id, char* buf, size_t buf_len) {
    const char* home = getenv("HOME");
    if (!home) home = "/tmp";

#if defined(__APPLE__)
    snprintf(buf, buf_len, "%s/Library/Caches/casc/%s", home, plugin_id);
#elif defined(__linux__)
    const char* xdg = getenv("XDG_CACHE_HOME");
    if (xdg && xdg[0])
        snprintf(buf, buf_len, "%s/casc/%s", xdg, plugin_id);
    else
        snprintf(buf, buf_len, "%s/.cache/casc/%s", home, plugin_id);
#elif defined(_WIN32)
    const char* appdata = getenv("LOCALAPPDATA");
    if (appdata)
        snprintf(buf, buf_len, "%s\\casc\\%s", appdata, plugin_id);
    else
        snprintf(buf, buf_len, "%s\\AppData\\Local\\casc\\%s", home, plugin_id);
#else
    snprintf(buf, buf_len, "/tmp/casc/%s", plugin_id);
#endif

    return CASC_OK;
}

static void mkdirs(const char* path) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            *p = '\0';
#ifdef _WIN32
            _mkdir(tmp);
#else
            mkdir(tmp, 0755);
#endif
            *p = '/';
        }
    }
#ifdef _WIN32
    _mkdir(tmp);
#else
    mkdir(tmp, 0755);
#endif
}

/* -------------------------------------------------------------------------- */
/*  Cache store/load                                                          */
/* -------------------------------------------------------------------------- */

casc_error_t casc_aot_cache_store(wasm_engine_t* engine,
                                   wasmtime_module_t* module,
                                   const char* cache_dir,
                                   const uint8_t sha256[32]) {
    (void)engine;

    mkdirs(cache_dir);

    /* Serialize module */
    wasm_byte_vec_t serialized;
    wasmtime_error_t* error = wasmtime_module_serialize(module, &serialized);
    if (error) {
        wasmtime_error_delete(error);
        return CASC_ERR_IO;
    }

    /* Write serialized module */
    char path[1100];
    snprintf(path, sizeof(path), "%s/module.bin", cache_dir);
    FILE* f = fopen(path, "wb");
    if (f) {
        fwrite(serialized.data, 1, serialized.size, f);
        fclose(f);
    }
    wasm_byte_vec_delete(&serialized);

    /* Write hash */
    snprintf(path, sizeof(path), "%s/hash.txt", cache_dir);
    f = fopen(path, "w");
    if (f) {
        char hex[65];
        hex_string(sha256, hex);
        fprintf(f, "%s\n", hex);
        fclose(f);
    }

    /* Write runtime version */
    snprintf(path, sizeof(path), "%s/runtime.txt", cache_dir);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "wasmtime %s\n", CASC_VERSION_STRING);
        fclose(f);
    }

    return CASC_OK;
}

casc_error_t casc_aot_cache_load(wasm_engine_t* engine,
                                  const char* cache_dir,
                                  const uint8_t sha256[32],
                                  wasmtime_module_t** out_module) {
    /* Check hash */
    char path[1100];
    snprintf(path, sizeof(path), "%s/hash.txt", cache_dir);
    FILE* f = fopen(path, "r");
    if (!f) return CASC_ERR_FILE_NOT_FOUND;

    char stored_hex[65] = {0};
    if (!fgets(stored_hex, sizeof(stored_hex), f)) {
        fclose(f);
        return CASC_ERR_FILE_NOT_FOUND;
    }
    fclose(f);

    /* Remove trailing newline */
    size_t slen = strlen(stored_hex);
    if (slen > 0 && stored_hex[slen-1] == '\n') stored_hex[slen-1] = '\0';

    char current_hex[65];
    hex_string(sha256, current_hex);

    if (strcmp(stored_hex, current_hex) != 0) {
        return CASC_ERR_FILE_NOT_FOUND; /* hash mismatch → recompile */
    }

    /* Read serialized module */
    snprintf(path, sizeof(path), "%s/module.bin", cache_dir);
    f = fopen(path, "rb");
    if (!f) return CASC_ERR_FILE_NOT_FOUND;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0) { fclose(f); return CASC_ERR_FILE_NOT_FOUND; }

    uint8_t* data = (uint8_t*)malloc((size_t)size);
    if (!data) { fclose(f); return CASC_ERR_OUT_OF_MEMORY; }

    fread(data, 1, (size_t)size, f);
    fclose(f);

    /* Deserialize */
    wasmtime_error_t* error = wasmtime_module_deserialize(
        engine, data, (size_t)size, out_module);
    free(data);

    if (error) {
        wasmtime_error_delete(error);
        return CASC_ERR_WASM_COMPILE; /* deserialization failure → recompile */
    }

    return CASC_OK;
}
