/*
 * test_manifest.c — Unit tests for manifest parsing
 */

#include "casc_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define TEST(name) static void name(void)
#define RUN(name) do { printf("  %-40s ", #name); name(); printf("PASS\n"); } while(0)

/* -------------------------------------------------------------------------- */
/*  Valid manifest                                                            */
/* -------------------------------------------------------------------------- */

static const char* VALID_MANIFEST =
"{"
"  \"casc_version\": \"0.1\","
"  \"id\": \"org.casc.test.gain\","
"  \"name\": \"Test Gain\","
"  \"version\": \"1.0.0\","
"  \"vendor\": \"CASC Tests\","
"  \"url\": \"https://example.com\","
"  \"description\": \"A test plugin\","
"  \"category\": \"utility\","
"  \"features\": [\"audio-effect\", \"stereo\"],"
"  \"audio_inputs\": [{\"name\": \"main\", \"channels\": 2}],"
"  \"audio_outputs\": [{\"name\": \"main\", \"channels\": 2}],"
"  \"midi_input\": false,"
"  \"midi_output\": false,"
"  \"mpe_support\": false,"
"  \"latency_frames\": 0,"
"  \"tail_seconds\": 0.0,"
"  \"hard_realtime\": true,"
"  \"params\": ["
"    {"
"      \"id\": 0,"
"      \"name\": \"Gain\","
"      \"short_name\": \"Gain\","
"      \"module\": \"Main\","
"      \"min\": 0.0,"
"      \"max\": 1.0,"
"      \"default\": 1.0,"
"      \"unit\": \"\","
"      \"flags\": [\"automatable\", \"modulatable\"],"
"      \"steps\": null"
"    }"
"  ]"
"}";

TEST(test_valid_manifest) {
    casc_manifest_t m;
    casc_error_t err = casc_manifest_parse(VALID_MANIFEST, strlen(VALID_MANIFEST), &m);
    assert(err == CASC_OK);
    assert(strcmp(m.casc_version, "0.1") == 0);
    assert(strcmp(m.id, "org.casc.test.gain") == 0);
    assert(strcmp(m.name, "Test Gain") == 0);
    assert(strcmp(m.version, "1.0.0") == 0);
    assert(strcmp(m.vendor, "CASC Tests") == 0);
    assert(strcmp(m.category, "utility") == 0);
    assert(m.feature_count == 2);
    assert(strcmp(m.features[0], "audio-effect") == 0);
    assert(strcmp(m.features[1], "stereo") == 0);
    assert(m.audio_input_count == 1);
    assert(m.audio_inputs[0].channels == 2);
    assert(m.audio_output_count == 1);
    assert(m.audio_outputs[0].channels == 2);
    assert(m.midi_input == false);
    assert(m.latency_frames == 0);
    assert(m.tail_seconds == 0.0);
    assert(m.hard_realtime == true);
    assert(m.param_count == 1);
    assert(m.params[0].id == 0);
    assert(strcmp(m.params[0].name, "Gain") == 0);
    assert(m.params[0].min_value == 0.0);
    assert(m.params[0].max_value == 1.0);
    assert(m.params[0].default_value == 1.0);
    assert(m.params[0].flags & CASC_PARAM_FLAG_AUTOMATABLE);
    assert(m.params[0].flags & CASC_PARAM_FLAG_MODULATABLE);
}

/* -------------------------------------------------------------------------- */
/*  Missing required fields                                                   */
/* -------------------------------------------------------------------------- */

TEST(test_missing_id) {
    const char* json = "{\"casc_version\":\"0.1\",\"name\":\"X\",\"version\":\"1.0.0\","
        "\"vendor\":\"V\",\"category\":\"utility\",\"features\":[],\"latency_frames\":0,"
        "\"tail_seconds\":0.0,\"hard_realtime\":true,\"params\":[]}";
    casc_manifest_t m;
    casc_error_t err = casc_manifest_parse(json, strlen(json), &m);
    assert(err == CASC_ERR_INVALID_MANIFEST);
}

TEST(test_missing_version) {
    const char* json = "{\"casc_version\":\"0.1\",\"id\":\"x.y.z\",\"name\":\"X\","
        "\"vendor\":\"V\",\"category\":\"utility\",\"features\":[],\"latency_frames\":0,"
        "\"tail_seconds\":0.0,\"hard_realtime\":true,\"params\":[]}";
    casc_manifest_t m;
    casc_error_t err = casc_manifest_parse(json, strlen(json), &m);
    assert(err == CASC_ERR_INVALID_MANIFEST);
}

/* -------------------------------------------------------------------------- */
/*  Invalid category                                                          */
/* -------------------------------------------------------------------------- */

TEST(test_invalid_category) {
    const char* json = "{\"casc_version\":\"0.1\",\"id\":\"x.y.z\",\"name\":\"X\","
        "\"version\":\"1.0.0\",\"vendor\":\"V\",\"category\":\"notacategory\","
        "\"features\":[],\"latency_frames\":0,\"tail_seconds\":0.0,"
        "\"hard_realtime\":true,\"params\":[]}";
    casc_manifest_t m;
    casc_error_t err = casc_manifest_parse(json, strlen(json), &m);
    assert(err == CASC_ERR_INVALID_MANIFEST);
}

/* -------------------------------------------------------------------------- */
/*  Empty params array is OK                                                  */
/* -------------------------------------------------------------------------- */

TEST(test_empty_params) {
    const char* json = "{\"casc_version\":\"0.1\",\"id\":\"x.y.z\",\"name\":\"X\","
        "\"version\":\"1.0.0\",\"vendor\":\"V\",\"category\":\"utility\","
        "\"features\":[],\"latency_frames\":0,\"tail_seconds\":0.0,"
        "\"hard_realtime\":true,\"params\":[]}";
    casc_manifest_t m;
    casc_error_t err = casc_manifest_parse(json, strlen(json), &m);
    assert(err == CASC_OK);
    assert(m.param_count == 0);
}

/* -------------------------------------------------------------------------- */
/*  Invalid JSON                                                              */
/* -------------------------------------------------------------------------- */

TEST(test_invalid_json) {
    const char* json = "this is not json";
    casc_manifest_t m;
    casc_error_t err = casc_manifest_parse(json, strlen(json), &m);
    assert(err == CASC_ERR_INVALID_MANIFEST);
}

/* -------------------------------------------------------------------------- */
/*  Multiple params                                                           */
/* -------------------------------------------------------------------------- */

TEST(test_multiple_params) {
    const char* json =
        "{\"casc_version\":\"0.1\",\"id\":\"x.y.z\",\"name\":\"X\","
        "\"version\":\"1.0.0\",\"vendor\":\"V\",\"category\":\"reverb\","
        "\"features\":[\"audio-effect\",\"tail\"],"
        "\"audio_inputs\":[{\"name\":\"in\",\"channels\":2}],"
        "\"audio_outputs\":[{\"name\":\"out\",\"channels\":2}],"
        "\"latency_frames\":64,\"tail_seconds\":2.5,"
        "\"hard_realtime\":true,"
        "\"params\":["
        "  {\"id\":0,\"name\":\"Size\",\"min\":0,\"max\":1,\"default\":0.5,"
        "   \"flags\":[\"automatable\"],\"steps\":null},"
        "  {\"id\":1,\"name\":\"Damp\",\"min\":0,\"max\":1,\"default\":0.3,"
        "   \"flags\":[\"automatable\",\"modulatable\"],\"steps\":null},"
        "  {\"id\":2,\"name\":\"Mix\",\"min\":0,\"max\":1,\"default\":0.5,"
        "   \"unit\":\"%\",\"flags\":[\"automatable\"],\"steps\":null}"
        "]}";
    casc_manifest_t m;
    casc_error_t err = casc_manifest_parse(json, strlen(json), &m);
    assert(err == CASC_OK);
    assert(m.param_count == 3);
    assert(m.params[0].id == 0);
    assert(m.params[1].id == 1);
    assert(m.params[2].id == 2);
    assert(m.latency_frames == 64);
    assert(m.tail_seconds == 2.5);
    assert(strcmp(m.category, "reverb") == 0);
    assert(m.feature_count == 2);
}

/* -------------------------------------------------------------------------- */
/*  Main                                                                      */
/* -------------------------------------------------------------------------- */

int main(void) {
    printf("test_manifest:\n");
    RUN(test_valid_manifest);
    RUN(test_missing_id);
    RUN(test_missing_version);
    RUN(test_invalid_category);
    RUN(test_empty_params);
    RUN(test_invalid_json);
    RUN(test_multiple_params);
    printf("All manifest tests passed.\n");
    return 0;
}
