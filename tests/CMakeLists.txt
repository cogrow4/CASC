# Test executables
# Each test is a standalone C program that returns 0 on success.

add_executable(test_manifest test_manifest.c)
target_link_libraries(test_manifest PRIVATE casc miniz cjson)
target_include_directories(test_manifest PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME test_manifest COMMAND test_manifest)

add_executable(test_loader test_loader.c)
target_link_libraries(test_loader PRIVATE casc miniz cjson)
target_include_directories(test_loader PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_test(NAME test_loader COMMAND test_loader)

# Wasm integration tests require the gain example to be built
add_executable(test_wasm test_wasm.c)
target_link_libraries(test_wasm PRIVATE casc miniz cjson)
add_test(NAME test_wasm COMMAND test_wasm "${CMAKE_BINARY_DIR}/gain.casc")

add_executable(test_roundtrip test_roundtrip.c)
target_link_libraries(test_roundtrip PRIVATE casc miniz cjson)
add_test(NAME test_roundtrip COMMAND test_roundtrip
    "${CMAKE_BINARY_DIR}/gain.casc")

# End-to-end CLAP bridge integration test.
# Acts as a minimal CLAP host: dlopen()s the bridge .clap, discovers the
# packed gain.casc through the plugin factory, instantiates it, and processes
# audio. Only registered when the gain example (and thus gain.casc) is built,
# since it needs a real .casc to load.
if(TARGET gain-casc)
    add_executable(test_clap_bridge test_clap_bridge.c)
    target_link_libraries(test_clap_bridge PRIVATE clap ${CMAKE_DL_LIBS})
    # Ensure the bridge binary and the packed plugin exist before this runs.
    add_dependencies(test_clap_bridge casc-clap-bridge gain-casc)
    add_test(NAME test_clap_bridge
        COMMAND test_clap_bridge
            "$<TARGET_FILE:casc-clap-bridge>"
            "${CMAKE_BINARY_DIR}/gain.casc")
endif()

# Synth MIDI test: sends Note On/Off through libcasc and asserts the
# instrument produces sound then decays. Needs the packed synth.casc.
if(TARGET synth-casc)
    add_executable(test_synth_midi test_synth_midi.c)
    target_link_libraries(test_synth_midi PRIVATE casc miniz cjson)
    add_dependencies(test_synth_midi synth-casc)
    add_test(NAME test_synth_midi
        COMMAND test_synth_midi "${CMAKE_BINARY_DIR}/synth.casc")
endif()
