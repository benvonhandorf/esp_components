idf_component_register(
    SRCS         "src/diag.c"
    INCLUDE_DIRS "include"
    REQUIRES     esp_common          # esp_err_t in the public header
    PRIV_REQUIRES log freertos)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
