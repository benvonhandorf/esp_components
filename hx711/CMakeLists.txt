# Both pins cross the API as plain ints and the public header includes nothing
# but esp_err.h, so this component has no public requirements at all -- unlike
# nau7802, whose header names i2c_master_dev_handle_t.
#
# esp_driver_gpio covers driver/gpio.h and esp_timer covers esp_timer_get_time().
# esp_hw_support (esp_cpu.h, esp_private/esp_clk.h), esp_rom (esp_rom_sys.h),
# soc (soc/gpio_reg.h), esp_common (esp_attr.h) and freertos (portMUX_TYPE) are
# IDF common requirements and are deliberately not listed.
idf_component_register(
    SRCS
        "src/hx711.c"
        "src/hx711_scale.c"
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        esp_driver_gpio
        esp_timer
)

# Strict warnings for this component only. PRIVATE, so a consuming project is
# not forced to build the rest of its tree this way.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
