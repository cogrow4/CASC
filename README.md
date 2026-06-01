# CASC — Community Audio Source Container

**One file. All platforms. Native performance. Safe by default.**

CASC (`.casc`) is an open, DAW-agnostic audio plugin format. A single `.casc` file works on Windows, macOS, and Linux — no per-platform builds, no installers, no registry entries.

DSP code is compiled to WebAssembly, then AOT-compiled to native machine code on first load. Plugins run in a memory-isolated sandbox: a crashing plugin cannot corrupt the host.

CASC bridges into the existing ecosystem via **[CLAP](https://cleveraudio.org/)** — the only major plugin standard that is fully open source (MIT).

> **Status: v0.1 Alpha** — Core format, libcasc runtime, CLAP bridge (with note-port/MIDI routing for instruments), and CLI tools are implemented and building. The full pipeline is verified end-to-end: seven example plugins (gain, reverb, delay, chorus, distortion, filter, and a polyphonic synth) each compile to `dsp.wasm`, pack to `.casc`, pass `casc-validate`, and pass the `ctest` suite — including a CLAP-host integration test and a synth MIDI test. The CLAP bridge loads `.casc` plugins into a CLAP host and processes audio (and live MIDI) correctly. Not yet production-ready.

---

## Quick Start

### Prerequisites

- **CMake** ≥ 3.20
- **C compiler** with C17 support (Clang 14+, GCC 11+, MSVC 2019+)
- **Clang with the wasm32 target + `wasm-ld`** (for building example plugins)

On macOS, Apple's system Clang does **not** include the WebAssembly backend,
and Homebrew's `llvm` formula does **not** ship the `wasm-ld` linker (that lives
in the separate `lld` formula). Install both:
```bash
brew install cmake llvm lld
```
The build auto-discovers the Homebrew `llvm`/`lld` toolchain (in
`/opt/homebrew/opt/{llvm,lld}/bin`) — you do **not** need to add them to `PATH`.
If you only want the host runtime, bridge, and CLI tools (no example plugin),
just `cmake` + `lld`/`llvm` are optional and the gain example is skipped
gracefully.

### Build

```bash
git clone https://github.com/casc-format/casc.git
cd casc
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc 2>/dev/null || sysctl -n hw.logicalcpu)
```

CMake will automatically download the **wasmtime** C API for your platform.

### Run Tests

```bash
cd build
ctest --output-on-failure
```

The suite covers manifest parsing, ZIP loading, Wasm instantiation/processing,
state save-load roundtripping, and a full **CLAP bridge integration test** that
loads the packed `gain.casc` through the bridge's plugin factory and processes
audio — exactly as a DAW would. (The bridge test is included automatically
whenever the wasm toolchain is present to build the gain example.)

### Build the Example Plugin

If you have Clang with wasm32 support (plus `wasm-ld`), the gain example is built automatically. To build it by hand, make sure a wasm-capable `clang` and `wasm-ld` are on `PATH`, then:

```bash
export PATH="/opt/homebrew/opt/llvm/bin:/opt/homebrew/opt/lld/bin:$PATH"  # macOS Homebrew
clang --target=wasm32 -O3 \
      -nostdlib -Wl,--no-entry -Wl,--export-dynamic \
      examples/gain/gain.c -o examples/gain/dsp.wasm
```

> Note: the example is compiled `-nostdlib` (no libc/stdlib headers are
> available for the bare `wasm32` target), so the DSP source must not include
> standard headers such as `<string.h>` or call libc functions.

### Pack & Validate

```bash
./build/casc-pack examples/gain/ gain.casc
./build/casc-validate gain.casc
```

---

## Plugin Catalog

Seven example plugins ship in [`examples/`](examples/). Each is a self-contained
folder (`<name>.c` DSP source, `manifest.json`, a polished `ui.html`, and
preset JSONs) and is built + packed automatically by CMake into
`build/<name>.casc`.

| Plugin | Type | Parameters | Notes |
|--------|------|-----------|-------|
| **gain** | Utility | Gain | Reference plugin — simplest possible DSP |
| **reverb** | Reverb | Size, Damping, Width, Mix | Freeverb-style: 8 parallel combs + 4 series allpasses per channel |
| **delay** | Delay | Time, Feedback, Tone, Mix | Stereo delay with feedback and a one-pole tone (lowpass) in the loop |
| **chorus** | Modulation | Rate, Depth, Mix | Modulated short delay lines for stereo widening / thickening |
| **distortion** | Distortion | Drive, Tone, Output, Mix | Waveshaper with pre-tone and dry/wet blend |
| **filter** | Filter | Cutoff, Resonance, Type | Chamberlin state-variable filter, switchable LP / HP / BP |
| **synth** | Instrument | Wave, Cutoff, Reso, A, D, S, R, Gain | 16-voice polyphonic subtractive synth — **MIDI in** |

Each plugin includes a clean, dark, self-contained `ui.html` (rotary knobs,
live value readouts, drag-to-edit, double-click-to-reset). The UI talks to the
DSP through a `window.casc` parameter bridge (`casc.setParam(id, value)` /
`casc.subscribe(cb)`), matching the format spec's WebView GUI model.

> **GUI note:** `ui.html` panels are authored per the v0.1 format spec. The
> current CLAP bridge surfaces parameters, audio ports, state, latency, tail,
> and (for instruments) note ports — it does **not** yet host the WebView GUI,
> so a DAW shows its generic parameter UI. Rendering `ui.html` via
> `CLAP_EXT_GUI` is planned future work.

### Install all plugins (macOS)

```bash
# The bridge scans these folders for .casc files:
mkdir -p ~/Library/Audio/Plug-Ins/CASC ~/Library/Audio/Plug-Ins/CLAP
cp build/*.casc                       ~/Library/Audio/Plug-Ins/CASC/
cp -R build/bridge/casc-clap-bridge.clap ~/Library/Audio/Plug-Ins/CLAP/
```

Then rescan plugins in your DAW. Effects appear as audio FX; **CASC Poly**
appears as an instrument you can play with MIDI.

---

## Architecture

```
DAW (CLAP host)
  └── casc-clap-bridge.clap       ← CLAP plugin (one binary, all plugins)
        └── libcasc                ← Host runtime library
              └── myplugin.casc   ← ZIP archive
                    ├── manifest.json
                    ├── dsp.wasm  ← WebAssembly DSP
                    └── presets/
```

### Components

| Component | Description |
|-----------|-------------|
| **libcasc** | C library for loading, instantiating, and processing `.casc` plugins |
| **casc-clap-bridge** | CLAP plugin that exposes `.casc` files to any CLAP-compatible DAW |
| **casc-pack** | CLI tool to create `.casc` archives |
| **casc-validate** | CLI tool to validate `.casc` files |

---

## Project Structure

```
CASC/
├── include/libcasc.h          # Public API (single header)
├── src/                       # libcasc implementation
│   ├── casc_manifest.c        # JSON manifest parsing
│   ├── casc_loader.c          # ZIP archive handling
│   ├── casc_wasm.c            # Wasmtime integration
│   ├── casc_aot.c             # AOT compilation cache
│   ├── casc_instance.c        # Plugin lifecycle & processing
│   └── casc_host_imports.c    # Host Wasm imports
├── bridge/                    # CLAP bridge plugin
├── tools/                     # CLI tools
├── examples/gain/             # Example gain plugin
├── tests/                     # Unit & integration tests
├── third_party/               # miniz, cJSON, CLAP headers
└── docs/                      # Format specification
```

---

## Format Specification

See [docs/format-spec-v0.1.md](docs/format-spec-v0.1.md) for the complete `.casc` format specification.

**Key points:**
- A `.casc` file is a ZIP archive containing `manifest.json` and `dsp.wasm`
- DSP is compiled to WebAssembly (MVP + SIMD)
- Parameters are normalised 0.0–1.0
- Plugins run in a memory-isolated Wasm sandbox
- AOT compilation caches native code for subsequent loads

---

## Writing a Plugin

1. Write DSP code in C (or any language targeting wasm32)
2. Implement the [required exports](docs/format-spec-v0.1.md#3-dspwasm--the-dsp-module)
3. Create a `manifest.json` describing your plugin
4. Compile to WebAssembly
5. Pack into a `.casc` file

See [examples/gain/](examples/) for a complete working example.

---

## License

[MIT](LICENSE)

---

## Dependencies

| Library | Purpose | License |
|---------|---------|---------|
| [wasmtime](https://wasmtime.dev/) | WebAssembly runtime | Apache-2.0 |
| [miniz](https://github.com/richgel999/miniz) | ZIP read/write | MIT |
| [cJSON](https://github.com/DaveGamble/cJSON) | JSON parsing | MIT |
| [CLAP](https://github.com/free-audio/clap) | Audio plugin API | MIT |
