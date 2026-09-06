# The SPI device handle type appears in the public header, so esp_driver_spi is a
# public requirement. esp_rom supplies the microsecond delay the reset path needs
# and crosses nothing.
idf_component_register(
    SRCS
        "src/ina239.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_driver_spi
    PRIV_REQUIRES
        esp_rom
)

# Strict warnings for this component only. PRIVATE, so a consuming project is
# not forced to build the rest of its tree this way.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
