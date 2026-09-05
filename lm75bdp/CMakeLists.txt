idf_component_register(
    SRCS         "src/lm75bdp.c" "src/lm75bdp_calc.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "src"
    REQUIRES     esp_driver_i2c)   # i2c_master_dev_handle_t in the public header

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
