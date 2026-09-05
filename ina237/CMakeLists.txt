# The I2C device handle type appears in the public header, so esp_driver_i2c is
# a public requirement. Nothing else crosses the API.
idf_component_register(
    SRCS
        "src/ina237.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_driver_i2c
)

# Strict warnings for this component only. PRIVATE, so a consuming project is
# not forced to build the rest of its tree this way.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
