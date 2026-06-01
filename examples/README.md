# CASC Example Plugins

Seven reference plugins, each a self-contained folder with C DSP source, a
manifest, a polished WebView UI, and presets. CMake compiles and packs them all
automatically (see the top-level build); this README documents the layout and
how to build one by hand.

| Folder | Name | Type | Parameters |
|--------|------|------|-----------|
| `gain/` | CASC Gain | Utility | Gain |
| `reverb/` | CASC Reverb | Reverb | Size, Damping, Width, Mix |
| `delay/` | CASC Delay | Delay | Time, Feedback, Tone, Mix |
| `chorus/` | CASC Chorus | Modulation | Rate, Depth, Mix |
| `distortion/` | CASC Distortion | Distortion | Drive, Tone, Output, Mix |
| `filter/` | CASC Filter | Filter | Cutoff, Resonance, Type (LP/HP/BP) |
| `synth/` | CASC Poly | Instrument (MIDI) | Wave, Cutoff, Reso, A, D, S, R, Gain |

## Anatomy of a plugin folder

```
reverb/
├── manifest.json      # Plugin metadata + parameter / port declarations
├── reverb.c           # DSP source (C, compiled to wasm32)
├── dsp.wasm           # Compiled DSP (build output — gitignored)
├── ui.html            # Self-contained WebView GUI (knobs + value readouts)
└── presets/
    ├── default.json
    └── large_hall.json
```

## Building one by hand

The DSP is compiled to a **bare `wasm32`** module with **no standard library**.
You need a wasm-capable Clang and the `wasm-ld` linker on `PATH`
(`brew install llvm lld` on macOS):

```bash
clang --target=wasm32 -O3 \
      -nostdlib -Wl,--no-entry -Wl,--export-dynamic \
      reverb/reverb.c -o reverb/dsp.wasm
```

> Because it's `-nostdlib`, the DSP source must **not** `#include` standard
> headers (`<string.h>`, `<math.h>`, …) or call libc functions. Everything the
> DSP needs (math helpers, memory) is defined inline or provided by the host.

## Packaging & validating

```bash
casc-pack     reverb/ reverb.casc
casc-validate reverb.casc
```

## The UI

Each `ui.html` is a single self-contained file: dark theme, rotary knobs with
live value readouts, drag-to-edit, double-click-to-reset. It communicates with
the DSP through a `window.casc` parameter bridge:

```js
casc.setParam(id, value);          // push a normalized 0..1 value to the DSP
casc.subscribe((id, value) => {}); // receive host/automation updates
```

This matches the WebView GUI model in the
[format spec](../docs/format-spec-v0.1.md). The current CLAP bridge does not yet
host the WebView (DAWs show their generic parameter UI); rendering `ui.html` via
`CLAP_EXT_GUI` is planned future work.

## The synth (MIDI)

`synth/` declares `"midi_input": true` and exports `dsp_send_midi`. The CLAP
bridge advertises a note input port for it, so a DAW routes MIDI notes straight
into the wasm voice allocator. It's a 16-voice polyphonic subtractive synth
(sine / saw / square / triangle → ADSR → state-variable lowpass).
