/*
 * libcasc.h — CASC Host Runtime Public API
 *
 * Community Audio Source Container (CASC) v0.1
 * https://github.com/casc-format/casc
 *
 * SPDX-License-Identifier: MIT
 *
 * This is the single public header for libcasc. DAW and host developers include
 * only this file to load, instantiate, and process .casc plugins.
 */

#ifndef LIBCASC_H
#define LIBCASC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/*  Version                                                                   */
/* -------------------------------------------------------------------------- */

#define CASC_VERSION_MAJOR 0
#define CASC_VERSION_MINOR 1
#define CASC_VERSION_PATCH 0
#define CASC_VERSION_STRING "0.1.0"

/** Returns the library version string (e.g. "0.1.0"). */
const char* casc_version(void);

/* -------------------------------------------------------------------------- */
/*  Opaque types                                                              */
/* -------------------------------------------------------------------------- */

/** A loaded .casc plugin (archive + compiled Wasm). */
typedef struct casc_plugin casc_plugin_t;

/** A running instance of a plugin (one per DAW track/slot). */
typedef struct casc_instance casc_instance_t;

/* -------------------------------------------------------------------------- */
/*  MIDI event                                                                */
/* -------------------------------------------------------------------------- */

typedef struct casc_midi_event {
    int32_t  frame_offset;   /**< Sample offset within the current block. */
    uint8_t  status;         /**< MIDI status byte. */
    uint8_t  data1;          /**< MIDI data byte 1. */
    uint8_t  data2;          /**< MIDI data byte 2. */
    uint8_t  _pad;           /**< Padding for alignment. */
} casc_midi_event_t;

/* -------------------------------------------------------------------------- */
/*  Error codes                                                               */
/* -------------------------------------------------------------------------- */

typedef enum casc_error {
    CASC_OK                  =  0,
    CASC_ERR_INVALID_ARG     = -1,
    CASC_ERR_FILE_NOT_FOUND  = -2,
    CASC_ERR_INVALID_ARCHIVE = -3,
    CASC_ERR_INVALID_MANIFEST= -4,
    CASC_ERR_WASM_COMPILE    = -5,
    CASC_ERR_WASM_LINK       = -6,
    CASC_ERR_WASM_RUNTIME    = -7,
    CASC_ERR_MISSING_EXPORT  = -8,
    CASC_ERR_OUT_OF_MEMORY   = -9,
    CASC_ERR_IO              = -10,
    CASC_ERR_SECURITY        = -11,
} casc_error_t;

/** Returns a human-readable string for an error code. */
const char* casc_error_string(casc_error_t err);

/* -------------------------------------------------------------------------- */
/*  Loading                                                                   */
/* -------------------------------------------------------------------------- */

/**
 * Load a .casc file. Returns NULL on error.
 * @param path       Path to the .casc file.
 * @param err_buf    Buffer to receive a human-readable error message (may be NULL).
 * @param err_buf_len Length of err_buf.
 * @return Loaded plugin, or NULL on failure.
 */
casc_plugin_t* casc_load(const char* path, char* err_buf, size_t err_buf_len);

/**
 * Unload a plugin and free all resources.
 * All instances must be destroyed before calling this.
 */
void casc_unload(casc_plugin_t* plugin);

/**
 * Read the manifest from a .casc file without fully loading the plugin.
 * @param path  Path to the .casc file.
 * @return Heap-allocated JSON string. Caller must free() it. NULL on error.
 */
char* casc_read_manifest(const char* path);

/* -------------------------------------------------------------------------- */
/*  Lifecycle                                                                 */
/* -------------------------------------------------------------------------- */

/**
 * Create a new instance of a loaded plugin.
 * @param plugin         A loaded plugin.
 * @param sample_rate    Audio sample rate (e.g. 44100, 48000, 96000).
 * @param max_block_size Maximum frames per process() call.
 * @return Instance, or NULL on error.
 */
casc_instance_t* casc_instantiate(
    casc_plugin_t* plugin,
    double sample_rate,
    int max_block_size
);

/** Destroy an instance and free all resources. */
void casc_destroy_instance(casc_instance_t* inst);

/**
 * Reset the instance (e.g. on sample rate change).
 * Clears all internal DSP state. Parameter values are preserved.
 */
void casc_reset(casc_instance_t* inst, double sample_rate, int max_block_size);

/* -------------------------------------------------------------------------- */
/*  Processing                                                                */
/* -------------------------------------------------------------------------- */

/**
 * Process one block of audio.
 * @param inst    Plugin instance.
 * @param inputs  Array of input channel pointers: inputs[channel][frame].
 * @param outputs Array of output channel pointers: outputs[channel][frame].
 * @param frames  Number of frames to process (≤ max_block_size).
 */
void casc_process(
    casc_instance_t* inst,
    const float** inputs,
    float** outputs,
    int frames
);

/**
 * Send MIDI events to the plugin (before calling casc_process).
 * @param inst   Plugin instance.
 * @param events Array of MIDI events.
 * @param count  Number of events.
 */
void casc_send_midi(
    casc_instance_t* inst,
    const casc_midi_event_t* events,
    int count
);

/* -------------------------------------------------------------------------- */
/*  Parameters                                                                */
/* -------------------------------------------------------------------------- */

/**
 * Set a parameter value (normalised 0.0–1.0).
 * Thread-safe: may be called from any thread.
 */
void casc_set_param(casc_instance_t* inst, int param_id, double value);

/**
 * Get the current value of a parameter (normalised 0.0–1.0).
 * Thread-safe: may be called from any thread.
 */
double casc_get_param(casc_instance_t* inst, int param_id);

/** Returns the plugin's reported latency in frames. */
int casc_get_latency(casc_instance_t* inst);

/** Returns the plugin's reported tail length in frames. */
int casc_get_tail(casc_instance_t* inst);

/* -------------------------------------------------------------------------- */
/*  State                                                                     */
/* -------------------------------------------------------------------------- */

/**
 * Serialise the plugin's state.
 * @param inst     Plugin instance.
 * @param out_size Receives the size of the returned blob.
 * @return Heap-allocated blob. Caller must free(). NULL on error.
 */
void* casc_save_state(casc_instance_t* inst, size_t* out_size);

/**
 * Restore plugin state from a previously saved blob.
 * @param inst  Plugin instance.
 * @param data  State blob.
 * @param size  Size of the blob in bytes.
 * @return CASC_OK on success, negative error code on failure.
 */
int casc_load_state(casc_instance_t* inst, const void* data, size_t size);

/* -------------------------------------------------------------------------- */
/*  Manifest query (after loading)                                            */
/* -------------------------------------------------------------------------- */

/** Returns the plugin's unique ID string (e.g. "com.yourname.myreverb"). */
const char* casc_plugin_get_id(const casc_plugin_t* plugin);

/** Returns the plugin's display name. */
const char* casc_plugin_get_name(const casc_plugin_t* plugin);

/** Returns the plugin's vendor name. */
const char* casc_plugin_get_vendor(const casc_plugin_t* plugin);

/** Returns the plugin's version string (SemVer). */
const char* casc_plugin_get_version(const casc_plugin_t* plugin);

/** Returns the plugin's description. */
const char* casc_plugin_get_description(const casc_plugin_t* plugin);

/** Returns the plugin's category string. */
const char* casc_plugin_get_category(const casc_plugin_t* plugin);

/** Returns the number of parameters. */
int casc_plugin_get_param_count(const casc_plugin_t* plugin);

/** Parameter info struct (returned by casc_plugin_get_param_info). */
typedef struct casc_param_info {
    int         id;
    const char* name;
    const char* short_name;
    const char* module;
    double      min_value;
    double      max_value;
    double      default_value;
    const char* unit;
    uint32_t    flags;        /**< Bitmask of CASC_PARAM_FLAG_* */
    int         steps;        /**< 0 if continuous */
} casc_param_info_t;

/* Parameter flag bits */
#define CASC_PARAM_FLAG_AUTOMATABLE  (1u << 0)
#define CASC_PARAM_FLAG_MODULATABLE  (1u << 1)
#define CASC_PARAM_FLAG_HIDDEN       (1u << 2)
#define CASC_PARAM_FLAG_READONLY     (1u << 3)
#define CASC_PARAM_FLAG_STEPPED      (1u << 4)
#define CASC_PARAM_FLAG_BYPASS       (1u << 5)

/**
 * Get parameter info by index (0-based).
 * @return CASC_OK on success, negative on error.
 */
int casc_plugin_get_param_info(const casc_plugin_t* plugin, int index,
                                casc_param_info_t* info);

/** Returns the number of audio input ports. */
int casc_plugin_get_audio_input_count(const casc_plugin_t* plugin);

/** Returns the number of audio output ports. */
int casc_plugin_get_audio_output_count(const casc_plugin_t* plugin);

/** Audio port info. */
typedef struct casc_audio_port_info {
    const char* name;
    int         channels;
} casc_audio_port_info_t;

/** Get audio input port info by index. */
int casc_plugin_get_audio_input_info(const casc_plugin_t* plugin, int index,
                                      casc_audio_port_info_t* info);

/** Get audio output port info by index. */
int casc_plugin_get_audio_output_info(const casc_plugin_t* plugin, int index,
                                       casc_audio_port_info_t* info);

/** Returns true if the plugin accepts MIDI input. */
int casc_plugin_has_midi_input(const casc_plugin_t* plugin);

/** Returns the manifest's latency_frames value. */
int casc_plugin_get_latency_frames(const casc_plugin_t* plugin);

/** Returns the manifest's tail_seconds value. */
double casc_plugin_get_tail_seconds(const casc_plugin_t* plugin);

/** Returns the number of features. */
int casc_plugin_get_feature_count(const casc_plugin_t* plugin);

/** Returns the feature string at the given index. */
const char* casc_plugin_get_feature(const casc_plugin_t* plugin, int index);

#ifdef __cplusplus
}
#endif

#endif /* LIBCASC_H */
