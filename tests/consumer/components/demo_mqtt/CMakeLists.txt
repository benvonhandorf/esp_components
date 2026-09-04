if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    js2c_generate("${CMAKE_CURRENT_SOURCE_DIR}/mqtt_config_schema.json"
                  BASENAME demo_mqtt_config OUT_C GEN_C OUT_H GEN_H)
    get_filename_component(GEN_DIR "${GEN_C}" DIRECTORY)
endif()

idf_component_register(
    SRCS         "${GEN_C}"
    INCLUDE_DIRS "${GEN_DIR}"
    REQUIRES     js2c)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
set_source_files_properties("${GEN_C}" TARGET_DIRECTORY ${COMPONENT_LIB}
    PROPERTIES COMPILE_OPTIONS "-include;js2c_error_capture.h")
