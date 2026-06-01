cmake_minimum_required(VERSION 3.20)
project(CASC
    VERSION 0.1.0
    DESCRIPTION "Community Audio Source Container"
    LANGUAGES C
)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build options
option(CASC_BUILD_BRIDGE "Build the CLAP bridge plugin" ON)
option(CASC_BUILD_TOOLS  "Build CLI tools (casc-pack, casc-validate)" ON)
option(CASC_BUILD_TESTS  "Build tests" ON)
option(CASC_BUILD_EXAMPLES "Build the bundled example plugins (needs emcc)" ON)

# Platform detection
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set(CASC_PLATFORM "macos")
    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64" OR CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
        set(CASC_ARCH "aarch64")
    else()
        set(CASC_ARCH "x86_64")
    endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(CASC_PLATFORM "linux")
    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
        set(CASC_ARCH "aarch64")
    else()
        set(CASC_ARCH "x86_64")
    endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    set(CASC_PLATFORM "windows")
    set(CASC_ARCH "x86_64")
else()
    message(FATAL_ERROR "Unsupported platform: ${CMAKE_SYSTEM_NAME}")
endif()

message(STATUS "CASC: Platform=${CASC_PLATFORM} Arch=${CASC_ARCH}")

# --- Wasmtime C API ---
# Download the prebuilt wasmtime C API for the current platform.
set(WASMTIME_VERSION "v29.0.1")

if(CASC_PLATFORM STREQUAL "macos")
    set(WASMTIME_ARCHIVE "wasmtime-${WASMTIME_VERSION}-${CASC_ARCH}-macos-c-api.tar.xz")
elseif(CASC_PLATFORM STREQUAL "linux")
    set(WASMTIME_ARCHIVE "wasmtime-${WASMTIME_VERSION}-${CASC_ARCH}-linux-c-api.tar.xz")
elseif(CASC_PLATFORM STREQUAL "windows")
    set(WASMTIME_ARCHIVE "wasmtime-${WASMTIME_VERSION}-${CASC_ARCH}-windows-c-api.zip")
endif()

set(WASMTIME_URL "https://github.com/bytecodealliance/wasmtime/releases/download/${WASMTIME_VERSION}/${WASMTIME_ARCHIVE}")
set(WASMTIME_DIR "${CMAKE_BINARY_DIR}/_deps/wasmtime")

if(NOT EXISTS "${WASMTIME_DIR}/lib")
    message(STATUS "Downloading Wasmtime C API ${WASMTIME_VERSION} for ${CASC_PLATFORM}-${CASC_ARCH}...")
    file(DOWNLOAD "${WASMTIME_URL}" "${CMAKE_BINARY_DIR}/_deps/${WASMTIME_ARCHIVE}"
         STATUS WASMTIME_DL_STATUS
         SHOW_PROGRESS)
    list(GET WASMTIME_DL_STATUS 0 WASMTIME_DL_CODE)
    if(NOT WASMTIME_DL_CODE EQUAL 0)
        message(FATAL_ERROR "Failed to download Wasmtime: ${WASMTIME_DL_STATUS}")
    endif()
    file(ARCHIVE_EXTRACT INPUT "${CMAKE_BINARY_DIR}/_deps/${WASMTIME_ARCHIVE}"
         DESTINATION "${CMAKE_BINARY_DIR}/_deps/wasmtime_extract")
    # The archive extracts into a subdirectory; move contents up
    file(GLOB WASMTIME_EXTRACTED_DIRS "${CMAKE_BINARY_DIR}/_deps/wasmtime_extract/wasmtime-*")
    list(GET WASMTIME_EXTRACTED_DIRS 0 WASMTIME_EXTRACTED_DIR)
    file(RENAME "${WASMTIME_EXTRACTED_DIR}" "${WASMTIME_DIR}")
endif()

# Create wasmtime imported target
add_library(wasmtime STATIC IMPORTED GLOBAL)
if(CASC_PLATFORM STREQUAL "windows")
    set_target_properties(wasmtime PROPERTIES
        IMPORTED_LOCATION "${WASMTIME_DIR}/lib/wasmtime.lib"
    )
else()
    set_target_properties(wasmtime PROPERTIES
        IMPORTED_LOCATION "${WASMTIME_DIR}/lib/libwasmtime.a"
    )
endif()
target_include_directories(wasmtime INTERFACE "${WASMTIME_DIR}/include")

# Platform-specific link dependencies for wasmtime
if(CASC_PLATFORM STREQUAL "linux")
    find_package(Threads REQUIRED)
    target_link_libraries(wasmtime INTERFACE Threads::Threads dl m)
elseif(CASC_PLATFORM STREQUAL "macos")
    # macOS needs no extra libs for wasmtime typically
endif()

# --- Subdirectories ---
add_subdirectory(third_party)
add_subdirectory(src)

if(CASC_BUILD_BRIDGE)
    add_subdirectory(bridge)
endif()

if(CASC_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(CASC_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(CASC_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
