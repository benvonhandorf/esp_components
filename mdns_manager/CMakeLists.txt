idf_component_register(
    SRCS         "src/mdns_manager.c"
    INCLUDE_DIRS "include"
    REQUIRES     esp_common          # esp_err_t in the public header
    PRIV_REQUIRES espressif__mdns    # the protocol implementation
                  net_events         # link state
                  esp_event
                  log)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
