# EMBED_FILES compiles the page into the binary, so the console works with no
# filesystem and no partition to keep in sync with the firmware.
idf_component_register(
    SRCS         "src/cli_web.c"
    INCLUDE_DIRS "include"
    REQUIRES     esp_http_server     # httpd_handle_t in the public header
    PRIV_REQUIRES cli diag
    EMBED_FILES  "res/index.html")

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
