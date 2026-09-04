# Nothing application-specific appears here. The predecessor listed relay_control
# and common, because it included the application's relay and status headers --
# which is why it could only ever be used by the project it came from.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    js2c_generate("${CMAKE_CURRENT_SOURCE_DIR}/mqtt_manager_config_schema.json"
                  BASENAME mqtt_manager_config OUT_C GEN_C)
    get_filename_component(GEN_C_DIR "${GEN_C}" DIRECTORY)
endif()

idf_component_register(
    SRCS         "src/mqtt_manager.c" "src/mqtt_topic.c" "src/mqtt_log_sink.c" "${GEN_C}"
    INCLUDE_DIRS "include" "${GEN_C_DIR}"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES     esp_common          # esp_err_t
                 js2c                # the generated config type is public API
    PRIV_REQUIRES espressif__mqtt    # esp-mqtt; a registry component since IDF v6
                  net_events
                  diag               # the log sink
                  esp_event
                  freertos
                  log)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
set_source_files_properties("${GEN_C}" TARGET_DIRECTORY ${COMPONENT_LIB}
    PROPERTIES COMPILE_OPTIONS "-include;js2c_error_capture.h")
