add_library(casc STATIC
    casc_manifest.c
    casc_loader.c
    casc_wasm.c
    casc_aot.c
    casc_instance.c
    casc_host_imports.c
    casc_ui.c
)

target_include_directories(casc
    PUBLIC  "${PROJECT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(casc
    PUBLIC  wasmtime
    PRIVATE miniz cjson
)

# ---------------------------------------------------------------------------
# GUI hosting backend selection (one file works anywhere).
#
# Every platform compiles the portable coordinator (casc_ui.c, above) plus
# exactly one backend that implements the casc_ui_backend_* hooks:
#   macOS   -> casc_ui_mac.m   (WKWebView,  always available)
#   Windows -> casc_ui_win.cpp (WebView2,   if SDK found) else null
#   Linux   -> casc_ui_gtk.c   (WebKitGTK,  if found)     else null
# The null backend keeps libcasc linkable everywhere and makes casc_open_ui
# return CASC_ERR_IO so hosts fall back to a generic parameter panel.
# ---------------------------------------------------------------------------
set(CASC_UI_BACKEND "")

if(CASC_PLATFORM STREQUAL "macos")
    find_library(CORE_FOUNDATION CoreFoundation)
    find_library(COCOA_LIB Cocoa)
    find_library(WEBKIT_LIB WebKit)
    target_sources(casc PRIVATE casc_ui_mac.m)
    target_link_libraries(casc PRIVATE ${CORE_FOUNDATION} ${COCOA_LIB} ${WEBKIT_LIB})
    set(CASC_UI_BACKEND "WKWebView (macOS)")

elseif(CASC_PLATFORM STREQUAL "linux")
    target_link_libraries(casc PRIVATE m)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        # Prefer WebKitGTK 6.0 (GTK4); fall back to 4.1 (GTK3).
        pkg_check_modules(WEBKITGTK QUIET webkitgtk-6.0)
        if(NOT WEBKITGTK_FOUND)
            pkg_check_modules(WEBKITGTK QUIET webkit2gtk-4.1)
        endif()
    endif()
    if(WEBKITGTK_FOUND)
        target_sources(casc PRIVATE casc_ui_gtk.c)
        target_compile_definitions(casc PRIVATE CASC_UI_HAVE_WEBKITGTK=1)
        target_include_directories(casc PRIVATE ${WEBKITGTK_INCLUDE_DIRS})
        target_link_libraries(casc PRIVATE ${WEBKITGTK_LIBRARIES})
        set(CASC_UI_BACKEND "WebKitGTK (Linux)")
    else()
        target_sources(casc PRIVATE casc_ui_null.c)
        set(CASC_UI_BACKEND "null (Linux — WebKitGTK not found; generic panel fallback)")
    endif()

elseif(CASC_PLATFORM STREQUAL "windows")
    # WebView2 SDK location can be supplied via -DCASC_WEBVIEW2_DIR=...
    find_path(WEBVIEW2_INCLUDE_DIR WebView2.h
        HINTS "${CASC_WEBVIEW2_DIR}/build/native/include"
              "$ENV{WEBVIEW2_DIR}/build/native/include")
    if(WEBVIEW2_INCLUDE_DIR)
        target_sources(casc PRIVATE casc_ui_win.cpp)
        target_compile_definitions(casc PRIVATE CASC_UI_HAVE_WEBVIEW2=1)
        target_include_directories(casc PRIVATE ${WEBVIEW2_INCLUDE_DIR})
        # WebView2Loader + COM runtime
        target_link_libraries(casc PRIVATE WebView2Loader ole32 oleaut32 shlwapi)
        set(CASC_UI_BACKEND "WebView2 (Windows)")
    else()
        target_sources(casc PRIVATE casc_ui_null.c)
        set(CASC_UI_BACKEND "null (Windows — WebView2 SDK not found; set CASC_WEBVIEW2_DIR)")
    endif()

else()
    target_sources(casc PRIVATE casc_ui_null.c)
    set(CASC_UI_BACKEND "null (unknown platform)")
endif()

message(STATUS "CASC GUI backend: ${CASC_UI_BACKEND}")
