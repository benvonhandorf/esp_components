# The project's config walker is generated from config/app_config_schema.json.
# Every section there $refs a component's own schema, which each component
# published under a logical name -- so the aggregate names "demo_net/..." rather
# than a path that depends on where that component happens to live.
#
# The guard is mandatory: IDF re-runs this file in `cmake -P` script mode to
# collect requirements, where js2c_generate_sections() does not exist.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    js2c_generate_sections("${CMAKE_CURRENT_LIST_DIR}/../config/app_config_schema.json"
                           BASENAME app_config
                           OUT_C GEN_C)
    # The generated parser, its header and the sections X-macro all land together.
    get_filename_component(GEN_DIR "${GEN_C}" DIRECTORY)
endif()

# No REQUIRES: project.cmake grants main an implicit dependency on every
# component only while REQUIRES and PRIV_REQUIRES are both unset.
idf_component_register(
    SRCS         "main.c" "${GEN_C}"
    INCLUDE_DIRS "." "${GEN_DIR}")

set_source_files_properties("${GEN_C}" TARGET_DIRECTORY ${COMPONENT_LIB}
    PROPERTIES COMPILE_OPTIONS "-include;js2c_error_capture.h")
