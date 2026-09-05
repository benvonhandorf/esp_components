# The I2C device handle type appears in the public header, so esp_driver_i2c is
# a public requirement. GPIO is used only for the DRDY pin, whose number crosses
# the API as a plain int, so it stays private.
idf_component_register(
    SRCS
        "src/nau7802.c"
        "src/nau7802_scale.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_driver_i2c
    PRIV_REQUIRES
        esp_driver_gpio
)

# Strict warnings for this component only. PRIVATE, so a consuming project is
# not forced to build the rest of its tree this way.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
