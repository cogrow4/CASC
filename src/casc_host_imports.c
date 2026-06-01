/*
 * casc_host_imports.c — Host-provided Wasm imports
 *
 * Defines the `casc_host` import namespace that plugins may call:
 *   - casc_host.log(ptr, len)          — Log a message to stderr
 *   - casc_host.get_time_seconds()     — Get monotonic time as f64
 */

#include "casc_internal.h"
#include <stdio.h>
#include <time.h>

#ifdef __APPLE__
#include <mach/mach_time.h>
#endif

/* -------------------------------------------------------------------------- */
/*  casc_host.log                                                             */
/* -------------------------------------------------------------------------- */

static wasm_trap_t* host_log_callback(
    void* env,
    wasmtime_caller_t* caller,
    const wasmtime_val_t* args,
    size_t nargs,
    wasmtime_val_t* results,
    size_t nresults)
{
    (void)env; (void)results; (void)nresults;
    if (nargs < 2) return NULL;

    int32_t ptr = args[0].of.i32;
    int32_t len = args[1].of.i32;

    /* Get memory from caller */
    wasmtime_extern_t mem_ext;
    bool ok = wasmtime_caller_export_get(caller, "memory", 6, &mem_ext);
    if (!ok || mem_ext.kind != WASMTIME_EXTERN_MEMORY) return NULL;

    wasmtime_context_t* ctx = wasmtime_caller_context(caller);
    uint8_t* mem_data = wasmtime_memory_data(ctx, &mem_ext.of.memory);
    size_t mem_size = wasmtime_memory_data_size(ctx, &mem_ext.of.memory);

    if (ptr < 0 || len < 0 || (size_t)(ptr + len) > mem_size) return NULL;

    fprintf(stderr, "[casc-plugin] %.*s\n", len, (const char*)(mem_data + ptr));
    return NULL;
}

/* -------------------------------------------------------------------------- */
/*  casc_host.get_time_seconds                                                */
/* -------------------------------------------------------------------------- */

static double get_monotonic_time(void) {
#ifdef __APPLE__
    static mach_timebase_info_data_t timebase = {0};
    if (timebase.denom == 0) mach_timebase_info(&timebase);
    uint64_t t = mach_absolute_time();
    return (double)(t * timebase.numer / timebase.denom) / 1e9;
#elif defined(CLOCK_MONOTONIC)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#else
    return (double)clock() / (double)CLOCKS_PER_SEC;
#endif
}

static wasm_trap_t* host_get_time_callback(
    void* env,
    wasmtime_caller_t* caller,
    const wasmtime_val_t* args,
    size_t nargs,
    wasmtime_val_t* results,
    size_t nresults)
{
    (void)env; (void)caller; (void)args; (void)nargs;
    if (nresults >= 1) {
        results[0].kind = WASMTIME_F64;
        results[0].of.f64 = get_monotonic_time();
    }
    return NULL;
}

/* -------------------------------------------------------------------------- */
/*  Define imports on linker                                                  */
/* -------------------------------------------------------------------------- */

casc_error_t casc_host_imports_define(wasmtime_linker_t* linker) {
    wasmtime_error_t* error;

    /* casc_host.log(i32 ptr, i32 len) → void */
    wasm_functype_t* log_type;
    {
        wasm_valtype_t* params[2] = {
            wasm_valtype_new(WASM_I32),
            wasm_valtype_new(WASM_I32)
        };
        wasm_valtype_vec_t params_vec, results_vec;
        wasm_valtype_vec_new(&params_vec, 2, params);
        wasm_valtype_vec_new_empty(&results_vec);
        log_type = wasm_functype_new(&params_vec, &results_vec);
    }

    error = wasmtime_linker_define_func(
        linker, "casc_host", 9, "log", 3,
        log_type, host_log_callback, NULL, NULL);
    wasm_functype_delete(log_type);
    if (error) { wasmtime_error_delete(error); return CASC_ERR_WASM_LINK; }

    /* casc_host.get_time_seconds() → f64 */
    wasm_functype_t* time_type;
    {
        wasm_valtype_t* results[1] = { wasm_valtype_new(WASM_F64) };
        wasm_valtype_vec_t params_vec, results_vec;
        wasm_valtype_vec_new_empty(&params_vec);
        wasm_valtype_vec_new(&results_vec, 1, results);
        time_type = wasm_functype_new(&params_vec, &results_vec);
    }

    error = wasmtime_linker_define_func(
        linker, "casc_host", 9, "get_time_seconds", 16,
        time_type, host_get_time_callback, NULL, NULL);
    wasm_functype_delete(time_type);
    if (error) { wasmtime_error_delete(error); return CASC_ERR_WASM_LINK; }

    return CASC_OK;
}
