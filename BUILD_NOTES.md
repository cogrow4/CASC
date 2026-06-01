# Build Notes

Practical notes for getting CASC building and tested from a clean checkout.
These complement the Quick Start in the README.

## What "finished" means here

A clean build produces, with a single `cmake .. && cmake --build .`:

- `libcasc` — the host runtime (static lib)
- `casc-pack`, `casc-validate` — CLI tools
- `casc-clap-bridge.clap` — the CLAP bridge (a loadable `.clap` bundle on macOS)
- `examples/gain/dsp.wasm` — the gain DSP compiled to WebAssembly
- `gain.casc` — the packed example plugin
- 5 tests, all passing under `ctest`

Verified end-to-end:
- `ctest` → 5/5 pass (manifest, loader, wasm, roundtrip, clap bridge)
- `./tools/casc-validate gain.casc` → VALID
- The CLAP bridge discovers and runs `gain.casc` through the plugin factory
  and processes audio correctly (covered by `test_clap_bridge`).

## macOS toolchain requirement (important)

The example plugin is cross-compiled to the bare `wasm32` target. This needs:

1. A Clang with the **WebAssembly backend** — Apple's system Clang does NOT
   have it. Use Homebrew's `llvm`.
2. The **`wasm-ld`** linker — Homebrew's `llvm` formula does NOT ship it; it
   lives in the separate `lld` formula.

So:

```bash
brew install cmake llvm lld
```

Both `llvm` and `lld` are keg-only (not symlinked into `/opt/homebrew/bin`).
The build **auto-discovers** them at their keg paths
(`/opt/homebrew/opt/llvm/bin/clang` and `/opt/homebrew/opt/lld/bin/wasm-ld`)
via `examples/CMakeLists.txt`, so you do NOT need to touch `PATH`. If those
tools are absent, the gain example (and the `test_clap_bridge` test) are
skipped gracefully and the rest of the project still builds.

`examples/CMakeLists.txt` verifies the toolchain by actually compiling AND
linking a tiny probe `.wasm` — `clang --print-supported-cpus` is unreliable
(Apple Clang prints to stderr and exit codes vary), so a real compile+link is
the only trustworthy detection.

## `-nostdlib` and DSP source constraints

The gain example is compiled with `--target=wasm32 -nostdlib`. There is no
libc / standard headers for the bare wasm32 target, so DSP sources must not
`#include <string.h>` (etc.) or call libc functions. `gain.c` was adjusted to
drop an unused `<string.h>` include for this reason.

## Reconfiguring after installing the toolchain

`find_program` results are cached in `CMakeCache.txt`. If you configured the
project *before* installing `llvm`/`lld`, clear the cached entries (or wipe the
build dir) so the wasm-capable clang is picked up:

```bash
cmake -UCLANG_WASM -UWASM_LD .. -DCMAKE_BUILD_TYPE=Release
# or simply: rm -rf build && mkdir build && cd build && cmake ..
```

## Linux

Install an `llvm`/`clang` with the wasm backend plus `lld` (which provides
`wasm-ld`) from your distro, or build/download an LLVM that includes them.
Discovery additionally honours `$CASC_PATH` (colon-separated) for installed
`.casc` files. Otherwise the steps are identical.
