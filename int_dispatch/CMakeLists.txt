idf_component_register(
    SRCS         "src/int_dispatch.c"
    INCLUDE_DIRS "include"
    REQUIRES     esp_driver_gpio     # gpio_num_t in the public header
    PRIV_REQUIRES freertos log)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
