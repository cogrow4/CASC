add_library(casc-clap-bridge MODULE
    casc_clap_bridge.c
)

# CLAP plugins use .clap extension
set_target_properties(casc-clap-bridge PROPERTIES
    PREFIX ""
    SUFFIX ".clap"
    C_VISIBILITY_PRESET hidden
)

target_link_libraries(casc-clap-bridge
    PRIVATE casc clap miniz cjson
)

# On macOS, produce a bundle-compatible .clap
if(APPLE)
    set_target_properties(casc-clap-bridge PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "clap"
    )
endif()
