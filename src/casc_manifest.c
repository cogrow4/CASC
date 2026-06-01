/*
 * casc_manifest.c — Manifest JSON parsing and validation
 *
 * Parses manifest.json from a .casc archive using cJSON.
 * Validates all required fields per the CASC v0.1 specification.
 */

#include "casc_internal.h"
#include "cJSON.h"
#include <string.h>
#include <stdio.h>

/* -------------------------------------------------------------------------- */
/*  Helpers                                                                   */
/* -------------------------------------------------------------------------- */

static void safe_strcpy(char* dst, size_t dst_size, const char* src) {
    if (!src) { dst[0] = '\0'; return; }
    size_t len = strlen(src);
    if (len >= dst_size) len = dst_size - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static bool json_get_string(const cJSON* obj, const char* key, char* out, size_t out_size) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsString(item) || !item->valuestring) return false;
    safe_strcpy(out, out_size, item->valuestring);
    return true;
}

static bool json_get_int(const cJSON* obj, const char* key, int* out) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsNumber(item)) return false;
    *out = item->valueint;
    return true;
}

static bool json_get_double(const cJSON* obj, const char* key, double* out) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsNumber(item)) return false;
    *out = item->valuedouble;
    return true;
}

static bool json_get_bool(const cJSON* obj, const char* key, bool* out) {
    const cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsBool(item)) return false;
    *out = cJSON_IsTrue(item);
    return true;
}

/* -------------------------------------------------------------------------- */
/*  Category validation                                                       */
/* -------------------------------------------------------------------------- */

static const char* VALID_CATEGORIES[] = {
    "analyzer", "chorus", "compressor", "delay", "distortion",
    "drum-machine", "eq", "filter", "flanger", "generator",
    "limiter", "modulator", "phaser", "pitch-shifter", "reverb",
    "sampler", "sequencer", "synth", "transient-shaper", "utility", "other",
    NULL
};

static bool is_valid_category(const char* cat) {
    for (int i = 0; VALID_CATEGORIES[i]; i++) {
        if (strcmp(cat, VALID_CATEGORIES[i]) == 0) return true;
    }
    return false;
}

/* -------------------------------------------------------------------------- */
/*  Feature flag validation                                                   */
/* -------------------------------------------------------------------------- */

static const char* VALID_FEATURES[] = {
    "audio-effect", "instrument", "midi-effect",
    "stereo", "mono", "surround", "ambisonic",
    "tail", "hard-realtime", "requires-internet",
    NULL
};

static bool is_valid_feature(const char* feat) {
    for (int i = 0; VALID_FEATURES[i]; i++) {
        if (strcmp(feat, VALID_FEATURES[i]) == 0) return true;
    }
    return false;
}

/* -------------------------------------------------------------------------- */
/*  Param flag parsing                                                        */
/* -------------------------------------------------------------------------- */

static uint32_t parse_param_flags(const cJSON* flags_array) {
    uint32_t flags = 0;
    const cJSON* item;
    cJSON_ArrayForEach(item, flags_array) {
        if (!cJSON_IsString(item)) continue;
        const char* s = item->valuestring;
        if      (strcmp(s, "automatable") == 0) flags |= CASC_PARAM_FLAG_AUTOMATABLE;
        else if (strcmp(s, "modulatable") == 0) flags |= CASC_PARAM_FLAG_MODULATABLE;
        else if (strcmp(s, "hidden")      == 0) flags |= CASC_PARAM_FLAG_HIDDEN;
        else if (strcmp(s, "readonly")    == 0) flags |= CASC_PARAM_FLAG_READONLY;
        else if (strcmp(s, "stepped")     == 0) flags |= CASC_PARAM_FLAG_STEPPED;
        else if (strcmp(s, "bypass")      == 0) flags |= CASC_PARAM_FLAG_BYPASS;
    }
    return flags;
}

/* -------------------------------------------------------------------------- */
/*  Parse audio ports                                                         */
/* -------------------------------------------------------------------------- */

static casc_error_t parse_ports(const cJSON* arr, casc_manifest_port_t* ports,
                                 int* count, int max_ports) {
    *count = 0;
    if (!cJSON_IsArray(arr)) return CASC_ERR_INVALID_MANIFEST;

    const cJSON* item;
    cJSON_ArrayForEach(item, arr) {
        if (*count >= max_ports) break;
        casc_manifest_port_t* p = &ports[*count];
        if (!json_get_string(item, "name", p->name, sizeof(p->name)))
            return CASC_ERR_INVALID_MANIFEST;
        if (!json_get_int(item, "channels", &p->channels))
            return CASC_ERR_INVALID_MANIFEST;
        if (p->channels < 1 || p->channels > 64)
            return CASC_ERR_INVALID_MANIFEST;
        (*count)++;
    }
    return CASC_OK;
}

/* -------------------------------------------------------------------------- */
/*  Parse parameters                                                          */
/* -------------------------------------------------------------------------- */

static casc_error_t parse_params(const cJSON* arr, casc_manifest_param_t* params,
                                  int* count) {
    *count = 0;
    if (!cJSON_IsArray(arr)) return CASC_ERR_INVALID_MANIFEST;

    const cJSON* item;
    cJSON_ArrayForEach(item, arr) {
        if (*count >= CASC_MAX_PARAMS) break;
        casc_manifest_param_t* p = &params[*count];
        memset(p, 0, sizeof(*p));

        if (!json_get_int(item, "id", &p->id))
            return CASC_ERR_INVALID_MANIFEST;
        if (!json_get_string(item, "name", p->name, sizeof(p->name)))
            return CASC_ERR_INVALID_MANIFEST;

        /* Optional fields */
        json_get_string(item, "short_name", p->short_name, sizeof(p->short_name));
        json_get_string(item, "module", p->module, sizeof(p->module));
        json_get_string(item, "unit", p->unit, sizeof(p->unit));

        if (!json_get_double(item, "min", &p->min_value))
            return CASC_ERR_INVALID_MANIFEST;
        if (!json_get_double(item, "max", &p->max_value))
            return CASC_ERR_INVALID_MANIFEST;
        if (!json_get_double(item, "default", &p->default_value))
            return CASC_ERR_INVALID_MANIFEST;

        /* Parse flags array */
        const cJSON* flags = cJSON_GetObjectItemCaseSensitive(item, "flags");
        if (cJSON_IsArray(flags)) {
            p->flags = parse_param_flags(flags);
        }

        /* Parse steps (null = continuous) */
        const cJSON* steps = cJSON_GetObjectItemCaseSensitive(item, "steps");
        if (cJSON_IsNumber(steps)) {
            p->steps = steps->valueint;
            p->flags |= CASC_PARAM_FLAG_STEPPED;
        } else {
            p->steps = 0;
        }

        (*count)++;
    }
    return CASC_OK;
}

/* -------------------------------------------------------------------------- */
/*  Main parse function                                                       */
/* -------------------------------------------------------------------------- */

casc_error_t casc_manifest_parse(const char* json_str, size_t json_len,
                                  casc_manifest_t* out) {
    (void)json_len;
    memset(out, 0, sizeof(*out));

    cJSON* root = cJSON_Parse(json_str);
    if (!root) return CASC_ERR_INVALID_MANIFEST;

    casc_error_t err = CASC_OK;

    /* Required string fields */
    if (!json_get_string(root, "casc_version", out->casc_version, sizeof(out->casc_version))) {
        err = CASC_ERR_INVALID_MANIFEST; goto done;
    }
    if (!json_get_string(root, "id", out->id, sizeof(out->id))) {
        err = CASC_ERR_INVALID_MANIFEST; goto done;
    }
    if (!json_get_string(root, "name", out->name, sizeof(out->name))) {
        err = CASC_ERR_INVALID_MANIFEST; goto done;
    }
    if (!json_get_string(root, "version", out->version, sizeof(out->version))) {
        err = CASC_ERR_INVALID_MANIFEST; goto done;
    }
    if (!json_get_string(root, "vendor", out->vendor, sizeof(out->vendor))) {
        err = CASC_ERR_INVALID_MANIFEST; goto done;
    }

    /* Optional string fields */
    json_get_string(root, "url", out->url, sizeof(out->url));
    json_get_string(root, "description", out->description, sizeof(out->description));

    /* Category (required) */
    if (!json_get_string(root, "category", out->category, sizeof(out->category))) {
        err = CASC_ERR_INVALID_MANIFEST; goto done;
    }
    if (!is_valid_category(out->category)) {
        err = CASC_ERR_INVALID_MANIFEST; goto done;
    }

    /* Features (required, may be empty array) */
    const cJSON* features = cJSON_GetObjectItemCaseSensitive(root, "features");
    if (!cJSON_IsArray(features)) {
        err = CASC_ERR_INVALID_MANIFEST; goto done;
    }
    out->feature_count = 0;
    const cJSON* feat;
    cJSON_ArrayForEach(feat, features) {
        if (out->feature_count >= CASC_MAX_FEATURES) break;
        if (cJSON_IsString(feat) && is_valid_feature(feat->valuestring)) {
            safe_strcpy(out->features[out->feature_count],
                       sizeof(out->features[0]), feat->valuestring);
            out->feature_count++;
        }
    }

    /* Audio ports */
    const cJSON* ai = cJSON_GetObjectItemCaseSensitive(root, "audio_inputs");
    if (ai) {
        err = parse_ports(ai, out->audio_inputs, &out->audio_input_count, CASC_MAX_AUDIO_PORTS);
        if (err != CASC_OK) goto done;
    }
    const cJSON* ao = cJSON_GetObjectItemCaseSensitive(root, "audio_outputs");
    if (ao) {
        err = parse_ports(ao, out->audio_outputs, &out->audio_output_count, CASC_MAX_AUDIO_PORTS);
        if (err != CASC_OK) goto done;
    }

    /* MIDI / MPE */
    json_get_bool(root, "midi_input",  &out->midi_input);
    json_get_bool(root, "midi_output", &out->midi_output);
    json_get_bool(root, "mpe_support", &out->mpe_support);

    /* Required numeric fields */
    if (!json_get_int(root, "latency_frames", &out->latency_frames)) {
        err = CASC_ERR_INVALID_MANIFEST; goto done;
    }
    if (!json_get_double(root, "tail_seconds", &out->tail_seconds)) {
        err = CASC_ERR_INVALID_MANIFEST; goto done;
    }
    if (!json_get_bool(root, "hard_realtime", &out->hard_realtime)) {
        err = CASC_ERR_INVALID_MANIFEST; goto done;
    }

    /* Params (required, may be empty array) */
    const cJSON* params = cJSON_GetObjectItemCaseSensitive(root, "params");
    if (!cJSON_IsArray(params)) {
        err = CASC_ERR_INVALID_MANIFEST; goto done;
    }
    err = parse_params(params, out->params, &out->param_count);
    if (err != CASC_OK) goto done;

    /* GUI (optional) */
    const cJSON* gui = cJSON_GetObjectItemCaseSensitive(root, "gui");
    if (gui && !cJSON_IsNull(gui)) {
        out->gui.present = true;
        json_get_string(gui, "type",  out->gui.type,  sizeof(out->gui.type));
        json_get_string(gui, "entry", out->gui.entry, sizeof(out->gui.entry));
        json_get_int(gui, "width",  &out->gui.width);
        json_get_int(gui, "height", &out->gui.height);
        json_get_bool(gui, "resizable", &out->gui.resizable);
    }

done:
    cJSON_Delete(root);
    return err;
}
