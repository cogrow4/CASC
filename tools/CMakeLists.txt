add_executable(casc-pack casc_pack.c)
target_link_libraries(casc-pack PRIVATE casc miniz cjson)

add_executable(casc-validate casc_validate.c)
target_link_libraries(casc-validate PRIVATE casc miniz cjson)
