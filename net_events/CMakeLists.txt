idf_component_register(
    SRCS         "src/net_events.c"
    INCLUDE_DIRS "include"
    REQUIRES     esp_event)   # ESP_EVENT_DECLARE_BASE in the public header

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
