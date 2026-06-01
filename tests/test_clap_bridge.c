/*
 * test_clap_bridge.c — End-to-end integration test for the CASC CLAP bridge.
 *
 * This acts as a minimal CLAP host:
 *   1. dlopen() the bridge .clap binary and read the `clap_entry` symbol.
 *   2. Point the bridge's plugin-discovery at a private directory by setting
 *      HOME (macOS/Linux) / CASC_PATH (Linux) to a temp tree that contains the
 *      freshly-built gain.casc — so the test never touches the user's real
 *      plugin folders.
 *   3. Enumerate the plugin factory, find org.casc.examples.gain.
 *   4. create_plugin() -> init() -> activate() -> start_processing().
 *   5. Query the params + audio-ports extensions.
 *   6. process() a block of audio and verify default-gain (1.0) passthrough.
 *
 * Usage: test_clap_bridge <bridge-binary> <gain.casc>
 * Returns 0 on success, non-zero on any failure.
 */
#include <clap/clap.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include <unistd.h>

#define FRAMES 128

static uint32_t in_size(const clap_input_events_t* l){(void)l;return 0;}
static const clap_event_header_t* in_get(const clap_input_events_t* l,uint32_t i){(void)l;(void)i;return NULL;}
static bool out_try_push(const clap_output_events_t* l,const clap_event_header_t* e){(void)l;(void)e;return true;}

static int copy_file(const char* src, const char* dst) {
    FILE* in = fopen(src, "rb");
    if (!in) return -1;
    FILE* out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }
    char buf[8192]; size_t n;
    while ((n = fread(buf,1,sizeof(buf),in)) > 0) {
        if (fwrite(buf,1,n,out) != n) { fclose(in); fclose(out); return -1; }
    }
    fclose(in); fclose(out);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr,"usage: %s <bridge-binary> <gain.casc>\n",argv[0]);
        return 2;
    }
    const char* bridge_bin = argv[1];
    const char* casc_src   = argv[2];

    /* --- Build a private discovery tree under a temp HOME --- */
    char tmpl[] = "/tmp/casc_clap_test_XXXXXX";
    char* tmpdir = mkdtemp(tmpl);
    if (!tmpdir) { fprintf(stderr,"mkdtemp failed\n"); return 1; }

    char dir1[1024], dir2[1024], dir3[1024], dst[1100];
    snprintf(dir1,sizeof(dir1),"%s/Library",tmpdir);
    snprintf(dir2,sizeof(dir2),"%s/Library/Audio",tmpdir);
    char dir2b[1100]; snprintf(dir2b,sizeof(dir2b),"%s/Library/Audio/Plug-Ins",tmpdir);
    snprintf(dir3,sizeof(dir3),"%s/Library/Audio/Plug-Ins/CASC",tmpdir);
    mkdir(dir1,0755); mkdir(dir2,0755); mkdir(dir2b,0755); mkdir(dir3,0755);
    snprintf(dst,sizeof(dst),"%s/gain.casc",dir3);
    if (copy_file(casc_src, dst) != 0) {
        fprintf(stderr,"failed to stage gain.casc into %s\n", dir3);
        return 1;
    }
    setenv("HOME", tmpdir, 1);
    /* Linux discovery also honours CASC_PATH */
    setenv("CASC_PATH", dir3, 1);

    /* --- Load the bridge as a CLAP host would --- */
    void* h = dlopen(bridge_bin, RTLD_NOW|RTLD_LOCAL);
    if (!h) { fprintf(stderr,"dlopen failed: %s\n", dlerror()); return 1; }

    const clap_plugin_entry_t* entry =
        (const clap_plugin_entry_t*)dlsym(h, "clap_entry");
    if (!entry) { fprintf(stderr,"no clap_entry symbol\n"); return 1; }

    if (!entry->init(bridge_bin)) { fprintf(stderr,"entry->init failed\n"); return 1; }

    const clap_plugin_factory_t* factory =
        (const clap_plugin_factory_t*)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    if (!factory) { fprintf(stderr,"no plugin factory\n"); return 1; }

    uint32_t n = factory->get_plugin_count(factory);
    printf("Discovered %u plugin(s) via CLAP factory\n", n);
    if (n == 0) { fprintf(stderr,"FAIL: no .casc plugins discovered\n"); return 1; }

    const char* want = "org.casc.examples.gain";
    const clap_plugin_descriptor_t* desc = NULL;
    for (uint32_t i=0;i<n;i++){
        const clap_plugin_descriptor_t* d = factory->get_plugin_descriptor(factory,i);
        printf("  [%u] %s  (%s)\n", i, d->id, d->name);
        if (strcmp(d->id, want)==0) desc = d;
    }
    if (!desc) { fprintf(stderr,"FAIL: gain plugin not found in factory\n"); return 1; }

    clap_host_t host; memset(&host,0,sizeof(host));
    host.clap_version = CLAP_VERSION;
    host.name = "casc-test-host"; host.version = "1.0";

    const clap_plugin_t* p = factory->create_plugin(factory, &host, want);
    if (!p) { fprintf(stderr,"FAIL: create_plugin returned NULL\n"); return 1; }
    if (!p->init(p)) { fprintf(stderr,"FAIL: plugin init\n"); return 1; }
    if (!p->activate(p, 48000.0, 1, FRAMES)) { fprintf(stderr,"FAIL: activate\n"); return 1; }
    p->start_processing(p);

    const clap_plugin_audio_ports_t* aports =
        (const clap_plugin_audio_ports_t*)p->get_extension(p, CLAP_EXT_AUDIO_PORTS);
    const clap_plugin_params_t* params =
        (const clap_plugin_params_t*)p->get_extension(p, CLAP_EXT_PARAMS);
    if (!aports || !params) { fprintf(stderr,"FAIL: missing extensions\n"); return 1; }
    printf("Audio in ports:  %u\n", aports->count(p, true));
    printf("Audio out ports: %u\n", aports->count(p, false));
    printf("Param count:     %u\n", params->count(p));
    if (aports->count(p,true) < 1 || aports->count(p,false) < 1) {
        fprintf(stderr,"FAIL: expected >=1 audio in/out port\n"); return 1;
    }
    if (params->count(p) < 1) { fprintf(stderr,"FAIL: expected >=1 param\n"); return 1; }

    float inL[FRAMES], inR[FRAMES], outL[FRAMES], outR[FRAMES];
    for (int i=0;i<FRAMES;i++){ inL[i]=1.0f; inR[i]=0.8f; }
    float* in_ch[2]  = { inL, inR };
    float* out_ch[2] = { outL, outR };

    clap_audio_buffer_t abin  = {0}; abin.data32=in_ch;   abin.channel_count=2;
    clap_audio_buffer_t about = {0}; about.data32=out_ch; about.channel_count=2;
    clap_input_events_t  ines = { .ctx=NULL, .size=in_size, .get=in_get };
    clap_output_events_t oues = { .ctx=NULL, .try_push=out_try_push };

    clap_process_t proc; memset(&proc,0,sizeof(proc));
    proc.frames_count = FRAMES;
    proc.audio_inputs = &abin;   proc.audio_inputs_count = 1;
    proc.audio_outputs = &about; proc.audio_outputs_count = 1;
    proc.in_events = &ines; proc.out_events = &oues;

    clap_process_status st = p->process(p, &proc);
    if (st == CLAP_PROCESS_ERROR) { fprintf(stderr,"FAIL: process error\n"); return 1; }

    /* Default gain = 1.0 -> output equals input */
    int ok = (fabsf(outL[0]-1.0f)<1e-4f) && (fabsf(outR[0]-0.8f)<1e-4f)
          && (fabsf(outL[FRAMES-1]-1.0f)<1e-4f) && (fabsf(outR[FRAMES-1]-0.8f)<1e-4f);
    printf("Process out L=%.3f R=%.3f (expect 1.000 / 0.800): %s\n",
           outL[0], outR[0], ok?"OK":"FAIL");
    if (!ok) { fprintf(stderr,"FAIL: unexpected process output\n"); return 1; }

    p->stop_processing(p);
    p->deactivate(p);
    p->destroy(p);
    entry->deinit();
    dlclose(h);

    /* best-effort cleanup */
    unlink(dst); rmdir(dir3); rmdir(dir2b); rmdir(dir2); rmdir(dir1); rmdir(tmpdir);

    printf("CLAP bridge integration test: PASS\n");
    return 0;
}
