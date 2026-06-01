add_library(casc STATIC
    casc_manifest.c
    casc_loader.c
    casc_wasm.c
    casc_aot.c
    casc_instance.c
    casc_host_imports.c
)

target_include_directories(casc
    PUBLIC  "${PROJECT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(casc
    PUBLIC  wasmtime
    PRIVATE miniz cjson
)

# Platform-specific libraries
if(CASC_PLATFORM STREQUAL "macos")
    find_library(CORE_FOUNDATION CoreFoundation)
    target_link_libraries(casc PRIVATE ${CORE_FOUNDATION})
elseif(CASC_PLATFORM STREQUAL "linux")
    target_link_libraries(casc PRIVATE m)
endif()
