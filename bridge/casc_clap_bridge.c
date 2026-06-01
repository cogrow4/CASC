/*
 * casc_clap_bridge.c — CLAP bridge for .casc plugins
 *
 * This is a CLAP plugin (.clap) that acts as an adapter. It scans for
 * installed .casc files and exposes each one to the DAW as a native CLAP plugin.
 *
 * Implements: clap_entry, clap_plugin_factory, clap_plugin
 * Extensions: params, audio-ports, state, latency, tail
 */

#include "libcasc.h"
#include <clap/clap.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>

/* -------------------------------------------------------------------------- */
/*  Maximum discovered plugins                                                */
/* -------------------------------------------------------------------------- */

#define MAX_PLUGINS 128

/* -------------------------------------------------------------------------- */
/*  Plugin discovery paths                                                    */
/* -------------------------------------------------------------------------- */

typedef struct {
    char path[1024];
    char manifest_json[8192];
    casc_plugin_t* loaded;

    /* Cached descriptor fields (stable pointers) */
    char id[256];
    char name[256];
    char vendor[256];
    char version[64];
    char description[256];
    const char* features[16];
    char features_storage[16][64];
    int feature_count;

    clap_plugin_descriptor_t descriptor;
} casc_discovered_t;

static casc_discovered_t g_discovered[MAX_PLUGINS];
static int g_discovered_count = 0;
static bool g_initialized = false;

/* -------------------------------------------------------------------------- */
/*  Per-instance data                                                         */
/* -------------------------------------------------------------------------- */

typedef struct {
    clap_plugin_t clap_plugin;
    const clap_host_t* host;
    casc_discovered_t* discovered;
    casc_plugin_t* plugin;
    casc_instance_t* instance;
    double sample_rate;
    int max_block_size;
    bool active;
    bool processing;
} casc_bridge_instance_t;

/* Forward declarations for extensions */
static const clap_plugin_params_t      s_ext_params;
static const clap_plugin_audio_ports_t s_ext_audio_ports;
static const clap_plugin_note_ports_t  s_ext_note_ports;
static const clap_plugin_state_t       s_ext_state;
static const clap_plugin_latency_t     s_ext_latency;
static const clap_plugin_tail_t        s_ext_tail;

/* -------------------------------------------------------------------------- */
/*  Scan directories for .casc files                                          */
/* -------------------------------------------------------------------------- */

static void scan_directory(const char* dir_path) {
    DIR* dir = opendir(dir_path);
    if (!dir) return;

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (g_discovered_count >= MAX_PLUGINS) break;

        const char* name = entry->d_name;
        size_t len = strlen(name);
        if (len < 6 || strcmp(name + len - 5, ".casc") != 0) continue;

        casc_discovered_t* d = &g_discovered[g_discovered_count];
        snprintf(d->path, sizeof(d->path), "%s/%s", dir_path, name);

        /* Try to read the manifest */
        char* manifest = casc_read_manifest(d->path);
        if (!manifest) continue;

        /* Try to load the plugin to validate and get metadata */
        char err_buf[512] = {0};
        d->loaded = casc_load(d->path, err_buf, sizeof(err_buf));
        if (!d->loaded) {
            free(manifest);
            continue;
        }

        /* Copy metadata for stable descriptor pointers */
        snprintf(d->id, sizeof(d->id), "%s", casc_plugin_get_id(d->loaded));
        snprintf(d->name, sizeof(d->name), "%s", casc_plugin_get_name(d->loaded));
        snprintf(d->vendor, sizeof(d->vendor), "%s", casc_plugin_get_vendor(d->loaded));
        snprintf(d->version, sizeof(d->version), "%s", casc_plugin_get_version(d->loaded));
        snprintf(d->description, sizeof(d->description), "%s",
                 casc_plugin_get_description(d->loaded));

        /* Map CASC features to CLAP features */
        d->feature_count = 0;
        int fc = casc_plugin_get_feature_count(d->loaded);
        for (int i = 0; i < fc && d->feature_count < 15; i++) {
            const char* feat = casc_plugin_get_feature(d->loaded, i);
            if (strcmp(feat, "audio-effect") == 0)
                snprintf(d->features_storage[d->feature_count], 64, "%s",
                         CLAP_PLUGIN_FEATURE_AUDIO_EFFECT);
            else if (strcmp(feat, "instrument") == 0)
                snprintf(d->features_storage[d->feature_count], 64, "%s",
                         CLAP_PLUGIN_FEATURE_INSTRUMENT);
            else
                snprintf(d->features_storage[d->feature_count], 64, "%s", feat);
            d->features[d->feature_count] = d->features_storage[d->feature_count];
            d->feature_count++;
        }
        d->features[d->feature_count] = NULL;

        /* Build CLAP descriptor */
        d->descriptor.clap_version = CLAP_VERSION;
        d->descriptor.id = d->id;
        d->descriptor.name = d->name;
        d->descriptor.vendor = d->vendor;
        d->descriptor.url = "";
        d->descriptor.manual_url = "";
        d->descriptor.support_url = "";
        d->descriptor.version = d->version;
        d->descriptor.description = d->description;
        d->descriptor.features = d->features;

        free(manifest);
        g_discovered_count++;
    }

    closedir(dir);
}

static void discover_plugins(void) {
    if (g_initialized) return;
    g_initialized = true;
    g_discovered_count = 0;

#if defined(__APPLE__)
    scan_directory("/Library/Audio/Plug-Ins/CASC");
    const char* home = getenv("HOME");
    if (home) {
        char buf[1024];
        snprintf(buf, sizeof(buf), "%s/Library/Audio/Plug-Ins/CASC", home);
        scan_directory(buf);
    }
#elif defined(__linux__)
    scan_directory("/usr/lib/casc");
    const char* home = getenv("HOME");
    if (home) {
        char buf[1024];
        snprintf(buf, sizeof(buf), "%s/.local/lib/casc", home);
        scan_directory(buf);
    }
    /* $CASC_PATH */
    const char* casc_path = getenv("CASC_PATH");
    if (casc_path) {
        char pathcopy[4096];
        snprintf(pathcopy, sizeof(pathcopy), "%s", casc_path);
        char* tok = strtok(pathcopy, ":");
        while (tok) {
            scan_directory(tok);
            tok = strtok(NULL, ":");
        }
    }
#elif defined(_WIN32)
    const char* cpf = getenv("COMMONPROGRAMFILES");
    if (cpf) {
        char buf[1024];
        snprintf(buf, sizeof(buf), "%s\\CASC", cpf);
        scan_directory(buf);
    }
    const char* appdata = getenv("APPDATA");
    if (appdata) {
        char buf[1024];
        snprintf(buf, sizeof(buf), "%s\\CASC", appdata);
        scan_directory(buf);
    }
#endif
}

/* -------------------------------------------------------------------------- */
/*  CLAP plugin methods                                                       */
/* -------------------------------------------------------------------------- */

static bool bridge_init(const clap_plugin_t* plugin) {
    (void)plugin;
    return true;
}

static void bridge_destroy(const clap_plugin_t* plugin) {
    casc_bridge_instance_t* bi = (casc_bridge_instance_t*)plugin->plugin_data;
    if (bi->instance) casc_destroy_instance(bi->instance);
    /* Don't unload the plugin — it's shared in g_discovered */
    free(bi);
}

static bool bridge_activate(const clap_plugin_t* plugin, double sr,
                              uint32_t min_frames, uint32_t max_frames) {
    (void)min_frames;
    casc_bridge_instance_t* bi = (casc_bridge_instance_t*)plugin->plugin_data;
    bi->sample_rate = sr;
    bi->max_block_size = (int)max_frames;
    bi->instance = casc_instantiate(bi->plugin, sr, (int)max_frames);
    if (!bi->instance) return false;
    bi->active = true;
    return true;
}

static void bridge_deactivate(const clap_plugin_t* plugin) {
    casc_bridge_instance_t* bi = (casc_bridge_instance_t*)plugin->plugin_data;
    if (bi->instance) {
        casc_destroy_instance(bi->instance);
        bi->instance = NULL;
    }
    bi->active = false;
}

static bool bridge_start_processing(const clap_plugin_t* plugin) {
    casc_bridge_instance_t* bi = (casc_bridge_instance_t*)plugin->plugin_data;
    bi->processing = true;
    return true;
}

static void bridge_stop_processing(const clap_plugin_t* plugin) {
    casc_bridge_instance_t* bi = (casc_bridge_instance_t*)plugin->plugin_data;
    bi->processing = false;
}

static void bridge_reset(const clap_plugin_t* plugin) {
    casc_bridge_instance_t* bi = (casc_bridge_instance_t*)plugin->plugin_data;
    if (bi->instance)
        casc_reset(bi->instance, bi->sample_rate, bi->max_block_size);
}

static clap_process_status bridge_process(const clap_plugin_t* plugin,
                                            const clap_process_t* process) {
    casc_bridge_instance_t* bi = (casc_bridge_instance_t*)plugin->plugin_data;
    if (!bi->instance) return CLAP_PROCESS_ERROR;

    /* Handle input parameter events + collect note/MIDI events for instruments */
    casc_midi_event_t midi_events[1024];
    int midi_count = 0;

    uint32_t event_count = process->in_events->size(process->in_events);
    for (uint32_t i = 0; i < event_count; i++) {
        const clap_event_header_t* hdr = process->in_events->get(process->in_events, i);
        if (hdr->space_id != CLAP_CORE_EVENT_SPACE_ID) continue;
        if (hdr->type == CLAP_EVENT_PARAM_VALUE) {
            const clap_event_param_value_t* ev = (const clap_event_param_value_t*)hdr;
            casc_set_param(bi->instance, (int)ev->param_id, ev->value);
        } else if (hdr->type == CLAP_EVENT_NOTE_ON || hdr->type == CLAP_EVENT_NOTE_OFF) {
            const clap_event_note_t* ev = (const clap_event_note_t*)hdr;
            if (midi_count < 1024) {
                int note = ev->key & 0x7F;
                int vel  = (int)(ev->velocity * 127.0 + 0.5);
                if (vel > 127) vel = 127;
                casc_midi_event_t* me = &midi_events[midi_count++];
                me->frame_offset = (int)hdr->time;
                if (hdr->type == CLAP_EVENT_NOTE_ON) {
                    me->status = 0x90; me->data1 = (uint8_t)note;
                    me->data2 = (uint8_t)(vel > 0 ? vel : 1);
                } else {
                    me->status = 0x80; me->data1 = (uint8_t)note; me->data2 = 0;
                }
            }
        } else if (hdr->type == CLAP_EVENT_MIDI) {
            const clap_event_midi_t* ev = (const clap_event_midi_t*)hdr;
            uint8_t status = ev->data[0];
            uint8_t hi = status & 0xF0;
            /* forward note + CC-class messages; ignore realtime/sysex */
            if ((hi == 0x80 || hi == 0x90 || hi == 0xA0 || hi == 0xB0 ||
                 hi == 0xC0 || hi == 0xD0 || hi == 0xE0) && midi_count < 1024) {
                casc_midi_event_t* me = &midi_events[midi_count++];
                me->frame_offset = (int)hdr->time;
                me->status = status;
                me->data1 = ev->data[1];
                me->data2 = ev->data[2];
            }
        }
    }

    /* Deliver collected MIDI to the DSP module before audio processing */
    if (midi_count > 0)
        casc_send_midi(bi->instance, midi_events, midi_count);

    uint32_t frames = process->frames_count;
    if (frames == 0) return CLAP_PROCESS_CONTINUE;

    /* Set up audio buffer pointers */
    uint32_t in_channels = 0;
    if (process->audio_inputs_count > 0 && process->audio_inputs)
        in_channels = process->audio_inputs[0].channel_count;

    uint32_t out_channels = 0;
    if (process->audio_outputs_count > 0 && process->audio_outputs)
        out_channels = process->audio_outputs[0].channel_count;

    /* Process. Instruments have no audio input — pass NULL safely. */
    const float** in_data = NULL;
    if (process->audio_inputs_count > 0 && process->audio_inputs)
        in_data = (const float**)process->audio_inputs[0].data32;

    casc_process(bi->instance,
                  in_data,
                  process->audio_outputs[0].data32,
                  (int)frames);

    return CLAP_PROCESS_CONTINUE;
}

static const void* bridge_get_extension(const clap_plugin_t* plugin, const char* id) {
    casc_bridge_instance_t* bi = (casc_bridge_instance_t*)plugin->plugin_data;
    if (strcmp(id, CLAP_EXT_PARAMS) == 0)      return &s_ext_params;
    if (strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0)  return &s_ext_audio_ports;
    if (strcmp(id, CLAP_EXT_NOTE_PORTS) == 0) {
        /* Only advertise note ports for instruments that accept MIDI input */
        if (bi && bi->plugin && casc_plugin_has_midi_input(bi->plugin))
            return &s_ext_note_ports;
        return NULL;
    }
    if (strcmp(id, CLAP_EXT_STATE) == 0)        return &s_ext_state;
    if (strcmp(id, CLAP_EXT_LATENCY) == 0)     return &s_ext_latency;
    if (strcmp(id, CLAP_EXT_TAIL) == 0)        return &s_ext_tail;
    return NULL;
}

static void bridge_on_main_thread(const clap_plugin_t* plugin) {
    (void)plugin;
}

/* -------------------------------------------------------------------------- */
/*  Extension: params                                                         */
/* -------------------------------------------------------------------------- */

static uint32_t ext_params_count(const clap_plugin_t* plugin) {
    casc_bridge_instance_t* bi = (casc_bridge_instance_t*)plugin->plugin_data;
    return (uint32_t)casc_plugin_get_param_count(bi->plugin);
}

static bool ext_params_get_info(const clap_plugin_t* plugin, uint32_t index,
                                  clap_param_info_t* info) {
    casc_bridge_instance_t* bi = (casc_bridge_instance_t*)plugin->plugin_data;
    casc_param_info_t pi;
    if (casc_plugin_get_param_info(bi->plugin, (int)index, &pi) != CASC_OK)
        return false;

    memset(info, 0, sizeof(*info));
    info->id = (clap_id)pi.id;
    strncpy(info->name, pi.name, CLAP_NAME_SIZE - 1);
    strncpy(info->module, pi.module, CLAP_PATH_SIZE - 1);
    info->min_value = pi.min_value;
    info->max_value = pi.max_value;
    info->default_value = pi.default_value;

    info->flags = 0;
    if (pi.flags & CASC_PARAM_FLAG_AUTOMATABLE)
        info->flags |= CLAP_PARAM_IS_AUTOMATABLE;
    if (pi.flags & CASC_PARAM_FLAG_MODULATABLE)
        info->flags |= CLAP_PARAM_IS_MODULATABLE;
    if (pi.flags & CASC_PARAM_FLAG_READONLY)
        info->flags |= CLAP_PARAM_IS_READONLY;
    if (pi.flags & CASC_PARAM_FLAG_HIDDEN)
        info->flags |= CLAP_PARAM_IS_HIDDEN;
    if (pi.flags & CASC_PARAM_FLAG_STEPPED)
        info->flags |= CLAP_PARAM_IS_STEPPED;
    if (pi.flags & CASC_PARAM_FLAG_BYPASS)
        info->flags |= CLAP_PARAM_IS_BYPASS;

    return true;
}

static bool ext_params_get_value(const clap_plugin_t* plugin, clap_id param_id,
                                   double* value) {
    casc_bridge_instance_t* bi = (casc_bridge_instance_t*)plugin->plugin_data;
    if (!bi->instance) return false;
    *value = casc_get_param(bi->instance, (int)param_id);
    return true;
}

static bool ext_params_value_to_text(const clap_plugin_t* plugin, clap_id param_id,
                                       double value, char* display, uint32_t size) {
    (void)plugin; (void)param_id;
    snprintf(display, size, "%.3f", value);
    return true;
}

static bool ext_params_text_to_value(const clap_plugin_t* plugin, clap_id param_id,
                                       const char* display, double* value) {
    (void)plugin; (void)param_id;
    *value = atof(display);
    return true;
}

static void ext_params_flush(const clap_plugin_t* plugin,
                               const clap_input_events_t* in,
                               const clap_output_events_t* out) {
    (void)out;
    casc_bridge_instance_t* bi = (casc_bridge_instance_t*)plugin->plugin_data;
    if (!bi->instance) return;

    uint32_t count = in->size(in);
    for (uint32_t i = 0; i < count; i++) {
        const clap_event_header_t* hdr = in->get(in, i);
        if (hdr->space_id != CLAP_CORE_EVENT_SPACE_ID) continue;
        if (hdr->type == CLAP_EVENT_PARAM_VALUE) {
            const clap_event_param_value_t* ev = (const clap_event_param_value_t*)hdr;
            casc_set_param(bi->instance, (int)ev->param_id, ev->value);
        }
    }
}

static const clap_plugin_params_t s_ext_params = {
    .count          = ext_params_count,
    .get_info       = ext_params_get_info,
    .get_value      = ext_params_get_value,
    .value_to_text  = ext_params_value_to_text,
    .text_to_value  = ext_params_text_to_value,
    .flush          = ext_params_flush,
};

/* -------------------------------------------------------------------------- */
/*  Extension: audio-ports                                                    */
/* -------------------------------------------------------------------------- */

static uint32_t ext_audio_ports_count(const clap_plugin_t* plugin, bool is_input) {
    casc_bridge_instance_t* bi = (casc_bridge_instance_t*)plugin->plugin_data;
    return (uint32_t)(is_input ? casc_plugin_get_audio_input_count(bi->plugin)
                                : casc_plugin_get_audio_output_count(bi->plugin));
}

static bool ext_audio_ports_get(const clap_plugin_t* plugin, uint32_t index,
                                  bool is_input, clap_audio_port_info_t* info) {
    casc_bridge_instance_t* bi = (casc_bridge_instance_t*)plugin->plugin_data;
    casc_audio_port_info_t api;

    int err;
    if (is_input)
        err = casc_plugin_get_audio_input_info(bi->plugin, (int)index, &api);
    else
        err = casc_plugin_get_audio_output_info(bi->plugin, (int)index, &api);
    if (err != CASC_OK) return false;

    memset(info, 0, sizeof(*info));
    info->id = index;
    strncpy(info->name, api.name, CLAP_NAME_SIZE - 1);
    info->channel_count = (uint32_t)api.channels;
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    if (api.channels == 1)
        info->port_type = CLAP_PORT_MONO;
    else if (api.channels == 2)
        info->port_type = CLAP_PORT_STEREO;
    else
        info->port_type = NULL;
    info->in_place_pair = CLAP_INVALID_ID;

    return true;
}

static const clap_plugin_audio_ports_t s_ext_audio_ports = {
    .count = ext_audio_ports_count,
    .get   = ext_audio_ports_get,
};

/* -------------------------------------------------------------------------- */
/*  Extension: note-ports (input only, for instruments)                       */
/* -------------------------------------------------------------------------- */

static uint32_t ext_note_ports_count(const clap_plugin_t* plugin, bool is_input) {
    casc_bridge_instance_t* bi = (casc_bridge_instance_t*)plugin->plugin_data;
    if (!is_input) return 0;
    return (bi && bi->plugin && casc_plugin_has_midi_input(bi->plugin)) ? 1u : 0u;
}

static bool ext_note_ports_get(const clap_plugin_t* plugin, uint32_t index,
                                 bool is_input, clap_note_port_info_t* info) {
    (void)index;
    casc_bridge_instance_t* bi = (casc_bridge_instance_t*)plugin->plugin_data;
    if (!is_input) return false;
    if (!bi || !bi->plugin || !casc_plugin_has_midi_input(bi->plugin)) return false;

    memset(info, 0, sizeof(*info));
    info->id = 0;
    /* Accept both the CLAP note dialect and raw MIDI; prefer CLAP, fall back to MIDI */
    info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
    info->preferred_dialect  = CLAP_NOTE_DIALECT_CLAP;
    strncpy(info->name, "notes", CLAP_NAME_SIZE - 1);
    return true;
}

static const clap_plugin_note_ports_t s_ext_note_ports = {
    .count = ext_note_ports_count,
    .get   = ext_note_ports_get,
};

/* -------------------------------------------------------------------------- */
/*  Extension: state                                                          */
/* -------------------------------------------------------------------------- */

static bool ext_state_save(const clap_plugin_t* plugin,
                             const clap_ostream_t* stream) {
    casc_bridge_instance_t* bi = (casc_bridge_instance_t*)plugin->plugin_data;
    if (!bi->instance) return false;

    size_t size = 0;
    void* data = casc_save_state(bi->instance, &size);
    if (!data || size == 0) return false;

    int64_t written = stream->write(stream, data, size);
    free(data);
    return written == (int64_t)size;
}

static bool ext_state_load(const clap_plugin_t* plugin,
                             const clap_istream_t* stream) {
    casc_bridge_instance_t* bi = (casc_bridge_instance_t*)plugin->plugin_data;
    if (!bi->instance) return false;

    /* Read all data from stream */
    uint8_t buf[1 << 20]; /* 1 MB max */
    size_t total = 0;
    while (total < sizeof(buf)) {
        int64_t n = stream->read(stream, buf + total, sizeof(buf) - total);
        if (n <= 0) break;
        total += (size_t)n;
    }
    if (total == 0) return false;

    return casc_load_state(bi->instance, buf, total) == CASC_OK;
}

static const clap_plugin_state_t s_ext_state = {
    .save = ext_state_save,
    .load = ext_state_load,
};

/* -------------------------------------------------------------------------- */
/*  Extension: latency                                                        */
/* -------------------------------------------------------------------------- */

static uint32_t ext_latency_get(const clap_plugin_t* plugin) {
    casc_bridge_instance_t* bi = (casc_bridge_instance_t*)plugin->plugin_data;
    return (uint32_t)casc_plugin_get_latency_frames(bi->plugin);
}

static const clap_plugin_latency_t s_ext_latency = {
    .get = ext_latency_get,
};

/* -------------------------------------------------------------------------- */
/*  Extension: tail                                                           */
/* -------------------------------------------------------------------------- */

static uint32_t ext_tail_get(const clap_plugin_t* plugin) {
    casc_bridge_instance_t* bi = (casc_bridge_instance_t*)plugin->plugin_data;
    double tail_sec = casc_plugin_get_tail_seconds(bi->plugin);
    if (tail_sec <= 0.0) return 0;
    return (uint32_t)(tail_sec * bi->sample_rate);
}

static const clap_plugin_tail_t s_ext_tail = {
    .get = ext_tail_get,
};

/* -------------------------------------------------------------------------- */
/*  Plugin factory                                                            */
/* -------------------------------------------------------------------------- */

static uint32_t factory_get_plugin_count(const clap_plugin_factory_t* factory) {
    (void)factory;
    return (uint32_t)g_discovered_count;
}

static const clap_plugin_descriptor_t* factory_get_plugin_descriptor(
    const clap_plugin_factory_t* factory, uint32_t index)
{
    (void)factory;
    if (index >= (uint32_t)g_discovered_count) return NULL;
    return &g_discovered[index].descriptor;
}

static const clap_plugin_t* factory_create_plugin(
    const clap_plugin_factory_t* factory,
    const clap_host_t* host,
    const char* plugin_id)
{
    (void)factory;
    if (!clap_version_is_compatible(host->clap_version)) return NULL;

    /* Find the discovered plugin matching this ID */
    casc_discovered_t* found = NULL;
    for (int i = 0; i < g_discovered_count; i++) {
        if (strcmp(g_discovered[i].id, plugin_id) == 0) {
            found = &g_discovered[i];
            break;
        }
    }
    if (!found || !found->loaded) return NULL;

    /* Allocate bridge instance */
    casc_bridge_instance_t* bi = (casc_bridge_instance_t*)calloc(1, sizeof(*bi));
    if (!bi) return NULL;

    bi->host = host;
    bi->discovered = found;
    bi->plugin = found->loaded;

    /* Fill in clap_plugin_t */
    bi->clap_plugin.desc             = &found->descriptor;
    bi->clap_plugin.plugin_data      = bi;
    bi->clap_plugin.init             = bridge_init;
    bi->clap_plugin.destroy          = bridge_destroy;
    bi->clap_plugin.activate         = bridge_activate;
    bi->clap_plugin.deactivate       = bridge_deactivate;
    bi->clap_plugin.start_processing = bridge_start_processing;
    bi->clap_plugin.stop_processing  = bridge_stop_processing;
    bi->clap_plugin.reset            = bridge_reset;
    bi->clap_plugin.process          = bridge_process;
    bi->clap_plugin.get_extension    = bridge_get_extension;
    bi->clap_plugin.on_main_thread   = bridge_on_main_thread;

    return &bi->clap_plugin;
}

static const clap_plugin_factory_t s_plugin_factory = {
    .get_plugin_count      = factory_get_plugin_count,
    .get_plugin_descriptor = factory_get_plugin_descriptor,
    .create_plugin         = factory_create_plugin,
};

/* -------------------------------------------------------------------------- */
/*  CLAP entry point                                                          */
/* -------------------------------------------------------------------------- */

static bool entry_init(const char* plugin_path) {
    (void)plugin_path;
    discover_plugins();
    return true;
}

static void entry_deinit(void) {
    for (int i = 0; i < g_discovered_count; i++) {
        if (g_discovered[i].loaded) {
            casc_unload(g_discovered[i].loaded);
            g_discovered[i].loaded = NULL;
        }
    }
    g_discovered_count = 0;
    g_initialized = false;
}

static const void* entry_get_factory(const char* factory_id) {
    if (strcmp(factory_id, CLAP_PLUGIN_FACTORY_ID) == 0)
        return &s_plugin_factory;
    return NULL;
}

CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    .clap_version = CLAP_VERSION_INIT,
    .init         = entry_init,
    .deinit       = entry_deinit,
    .get_factory  = entry_get_factory,
};
