# es8311.h includes driver/i2c_master.h, so esp_driver_i2c is a public
# requirement. FreeRTOS is used only for the post-reset delay.
idf_component_register(
    SRCS         "src/es8311.c" "src/es8311_calc.c"
    INCLUDE_DIRS "include"
    REQUIRES     esp_driver_i2c
    PRIV_REQUIRES freertos)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
