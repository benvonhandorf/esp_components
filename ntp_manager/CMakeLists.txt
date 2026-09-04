if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    js2c_generate("${CMAKE_CURRENT_SOURCE_DIR}/ntp_config_schema.json"
                  BASENAME ntp_config OUT_C GEN_C)
    get_filename_component(GEN_C_DIR "${GEN_C}" DIRECTORY)
endif()

idf_component_register(
    SRCS         "src/ntp_manager.c" "${GEN_C}"
    INCLUDE_DIRS "include" "${GEN_C_DIR}"
    REQUIRES     esp_common          # esp_err_t
                 js2c                # the generated config type is public API
    PRIV_REQUIRES esp_netif          # esp_netif_sntp
                  lwip               # esp_sntp.h
                  esp_event
                  net_events
                  log)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
set_source_files_properties("${GEN_C}" TARGET_DIRECTORY ${COMPONENT_LIB}
    PROPERTIES COMPILE_OPTIONS "-include;js2c_error_capture.h")
