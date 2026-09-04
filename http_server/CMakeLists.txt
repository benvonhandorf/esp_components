# No application types here. The predecessor's header included system_status.h
# (never used) and its CMakeLists required relay_control (never called).
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    js2c_generate("${CMAKE_CURRENT_SOURCE_DIR}/http_server_config_schema.json"
                  BASENAME http_server_config OUT_C GEN_C)
    get_filename_component(GEN_C_DIR "${GEN_C}" DIRECTORY)
endif()

idf_component_register(
    SRCS         "src/http_server.c" "src/http_auth.c" "${GEN_C}"
    INCLUDE_DIRS "include" "${GEN_C_DIR}"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES     esp_common          # esp_err_t
                 esp_http_server     # httpd_req_t / httpd_handle_t in the public header
                 js2c                # the generated config type is public API
    PRIV_REQUIRES net_events
                  esp_event
                  log)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
set_source_files_properties("${GEN_C}" TARGET_DIRECTORY ${COMPONENT_LIB}
    PROPERTIES COMPILE_OPTIONS "-include;js2c_error_capture.h")
