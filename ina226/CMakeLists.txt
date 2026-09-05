idf_component_register(
    SRCS         "src/ina226.c" "src/ina226_calc.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES     esp_driver_i2c)   # i2c_master_dev_handle_t in the public header

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
