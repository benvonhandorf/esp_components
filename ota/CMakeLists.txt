# ota.c has no transport dependency: an update arriving over MQTT or a serial
# console uses the same session API. ota_http.c is the HTTP route for the common
# case, which is why esp_http_server and http_server are private here.
idf_component_register(
    SRCS         "src/ota.c" "src/ota_http.c"
    INCLUDE_DIRS "include"
    REQUIRES     esp_common          # esp_err_t
    PRIV_REQUIRES app_update          # esp_ota_ops
                  esp_partition
                  esp_app_format
                  esp_system
                  esp_http_server
                  http_server
                  freertos
                  log)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
