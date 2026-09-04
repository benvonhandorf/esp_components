# The dependency list is the point of this component's rework. It names only the
# link layer: nothing that runs *over* WiFi appears here, because mDNS, SNTP and
# MQTT subscribe to net_events rather than being called from the event handler.
# The predecessor listed mdns_manager and ntp_manager, an upward dependency that
# made WiFi unbuildable without them.
if(NOT CMAKE_BUILD_EARLY_EXPANSION)
    js2c_generate("${CMAKE_CURRENT_SOURCE_DIR}/wifi_manager_config_schema.json"
                  BASENAME wifi_manager_config OUT_C GEN_C)
    get_filename_component(GEN_C_DIR "${GEN_C}" DIRECTORY)
endif()

idf_component_register(
    SRCS         "src/wifi_manager.c" "${GEN_C}"
    INCLUDE_DIRS "include" "${GEN_C_DIR}"
    REQUIRES     esp_common          # esp_err_t
                 js2c                # the generated config type is public API
                 lwip                # ping/ping_sock.h in the public header
    PRIV_REQUIRES esp_wifi
                  esp_netif
                  esp_event
                  esp_timer
                  nvs_flash
                  net_events
                  log)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
set_source_files_properties("${GEN_C}" TARGET_DIRECTORY ${COMPONENT_LIB}
    PROPERTIES COMPILE_OPTIONS "-include;js2c_error_capture.h")
