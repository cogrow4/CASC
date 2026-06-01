# --- miniz (ZIP read/write) ---
add_library(miniz STATIC
    miniz/miniz.c
)
target_include_directories(miniz PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/miniz")

# --- cJSON (JSON parser) ---
add_library(cjson STATIC
    cjson/cJSON.c
)
target_include_directories(cjson PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/cjson")

# --- CLAP headers (header-only) ---
add_library(clap INTERFACE)
target_include_directories(clap INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/clap")
