# Stands in for a real config-owning component (wifi_manager, mqtt_manager, ...).
#
# The guard is mandatory, not defensive: IDF re-runs every component CMakeLists.txt
# in `cmake -P` script mode to collect requirements, and js2c_generate() does not
# exist in that pass. Without it the build dies with "Unknown CMake command".
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    js2c_generate("${CMAKE_CURRENT_SOURCE_DIR}/net_config_schema.json"
                  BASENAME net_config
                  OUT_C GEN_C OUT_H GEN_H)
    js2c_publish_schema(demo_net "${CMAKE_CURRENT_SOURCE_DIR}/net_config_schema.json")
    get_filename_component(GEN_DIR "${GEN_C}" DIRECTORY)
endif()

idf_component_register(
    SRCS         "src/demo_net.c" "${GEN_C}"
    INCLUDE_DIRS "include" "${GEN_DIR}"
    REQUIRES     js2c)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)

# LOG_ERROR must be in scope before js2c_builtins.h inside the generated TU only,
# so the component's hand-written sources keep their own error handling.
set_source_files_properties("${GEN_C}" TARGET_DIRECTORY ${COMPONENT_LIB}
    PROPERTIES COMPILE_OPTIONS "-include;js2c_error_capture.h")
