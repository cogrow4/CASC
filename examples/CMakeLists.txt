# Examples build support
# Compiles each example's C source to dsp.wasm (if a wasm32-capable clang plus
# wasm-ld is available) and packs it into a .casc archive in the build dir.
#
# Apple's system clang does NOT ship the WebAssembly backend, and the Homebrew
# `llvm` formula does not include the `wasm-ld` linker (that lives in the
# separate `lld` formula). So we:
#   1. Search for a wasm-capable clang (preferring Homebrew llvm).
#   2. Search for wasm-ld (preferring Homebrew lld).
#   3. Verify by actually compiling+linking a tiny wasm module — the only
#      reliable test, since --print-supported-cpus lies on some toolchains.

# Prefer a Homebrew llvm clang if present, else fall back to PATH clang.
find_program(CLANG_WASM
    NAMES clang
    HINTS
        /opt/homebrew/opt/llvm/bin
        /usr/local/opt/llvm/bin
    NO_DEFAULT_PATH
)
if(NOT CLANG_WASM)
    find_program(CLANG_WASM clang)
endif()

# wasm-ld is required by clang's driver to link wasm32 objects.
find_program(WASM_LD
    NAMES wasm-ld
    HINTS
        /opt/homebrew/opt/lld/bin
        /usr/local/opt/lld/bin
        /opt/homebrew/opt/llvm/bin
        /usr/local/opt/llvm/bin
)

set(WASM_BUILDABLE FALSE)

if(CLANG_WASM AND WASM_LD)
    get_filename_component(WASM_LD_DIR "${WASM_LD}" DIRECTORY)
    get_filename_component(CLANG_WASM_DIR "${CLANG_WASM}" DIRECTORY)

    # Real verification: try to compile AND link a tiny wasm32 module.
    set(_wasm_probe "${CMAKE_CURRENT_BINARY_DIR}/wasm_probe.c")
    file(WRITE "${_wasm_probe}"
        "__attribute__((export_name(\"probe\"))) int probe(int x){return x+1;}\n")
    set(_wasm_probe_out "${CMAKE_CURRENT_BINARY_DIR}/wasm_probe.wasm")

    execute_process(
        COMMAND ${CMAKE_COMMAND} -E env
            "PATH=${WASM_LD_DIR}:${CLANG_WASM_DIR}:$ENV{PATH}"
            ${CLANG_WASM} --target=wasm32 -O3
                -nostdlib -Wl,--no-entry -Wl,--export-dynamic
                "${_wasm_probe}" -o "${_wasm_probe_out}"
        RESULT_VARIABLE WASM_PROBE_RESULT
        OUTPUT_VARIABLE WASM_PROBE_OUT
        ERROR_VARIABLE WASM_PROBE_ERR
    )

    if(WASM_PROBE_RESULT EQUAL 0 AND EXISTS "${_wasm_probe_out}")
        set(WASM_BUILDABLE TRUE)
    else()
        message(STATUS "CASC: wasm probe failed, skipping examples")
        message(STATUS "  ${WASM_PROBE_ERR}")
    endif()
endif()

# ----------------------------------------------------------------------------
# casc_add_example(<dir> <src>)
#   <dir> : subdirectory name under examples/ (also the .casc base name)
#   <src> : C source filename within that directory (e.g. reverb.c)
#
# Compiles <dir>/<src> -> <dir>/dsp.wasm, then packs the whole <dir> into
# ${CMAKE_BINARY_DIR}/<dir>.casc. Repacks whenever the manifest, the wasm, or
# any preset JSON changes. Exposes target <dir>-casc and appends the output
# path to the global CASC_BUILT_PLUGINS list (in the parent scope).
# ----------------------------------------------------------------------------
set(CASC_BUILT_PLUGINS "" CACHE INTERNAL "List of built .casc files")

function(casc_add_example dir src)
    set(_srcpath  "${CMAKE_CURRENT_SOURCE_DIR}/${dir}/${src}")
    set(_wasm     "${CMAKE_CURRENT_SOURCE_DIR}/${dir}/dsp.wasm")
    set(_casc     "${CMAKE_BINARY_DIR}/${dir}.casc")
    set(_manifest "${CMAKE_CURRENT_SOURCE_DIR}/${dir}/manifest.json")

    # Collect preset JSONs (if any) as repack dependencies.
    file(GLOB _presets "${CMAKE_CURRENT_SOURCE_DIR}/${dir}/presets/*.json")

    add_custom_command(
        OUTPUT "${_wasm}"
        COMMAND ${CMAKE_COMMAND} -E env
            "PATH=${WASM_LD_DIR}:${CLANG_WASM_DIR}:$ENV{PATH}"
            ${CLANG_WASM}
                --target=wasm32 -O3
                -nostdlib -Wl,--no-entry -Wl,--export-dynamic
                "${_srcpath}" -o "${_wasm}"
        DEPENDS "${_srcpath}"
        COMMENT "Compiling ${dir} example to WebAssembly"
        VERBATIM
    )
    add_custom_target(${dir}-example ALL DEPENDS "${_wasm}")

    add_custom_command(
        OUTPUT "${_casc}"
        COMMAND $<TARGET_FILE:casc-pack>
            "${CMAKE_CURRENT_SOURCE_DIR}/${dir}"
            "${_casc}"
        DEPENDS ${dir}-example casc-pack "${_manifest}" ${_presets}
        COMMENT "Packing ${dir}.casc"
        VERBATIM
    )
    add_custom_target(${dir}-casc ALL DEPENDS "${_casc}")

    # Append to the global list (propagate to parent scope).
    set(CASC_BUILT_PLUGINS "${CASC_BUILT_PLUGINS};${_casc}" CACHE INTERNAL "List of built .casc files")
endfunction()

if(WASM_BUILDABLE)
    message(STATUS "CASC: Building example plugins")
    message(STATUS "  clang:   ${CLANG_WASM}")
    message(STATUS "  wasm-ld: ${WASM_LD}")

    # dir            source file
    casc_add_example(gain       gain.c)
    casc_add_example(reverb     reverb.c)
    casc_add_example(delay      delay.c)
    casc_add_example(chorus     chorus.c)
    casc_add_example(distortion distortion.c)
    casc_add_example(filter     filter.c)
    casc_add_example(synth      synth.c)

    # Back-compat: keep the original gain.casc location/target name expected
    # by the test suite (gain-casc already created by casc_add_example(gain)).
    message(STATUS "CASC: 7 example plugins will be built and packed")
else()
    message(STATUS "CASC: Skipping examples (no wasm32-capable clang + wasm-ld)")
    message(STATUS "  Install wasm support: brew install llvm lld")
endif()
