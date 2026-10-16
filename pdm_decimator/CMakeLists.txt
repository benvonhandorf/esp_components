# The public header includes only esp_err.h, so there are no public requirements.
# heap is for heap_caps_aligned_alloc(); the rest is plain C.
#
# The vector source is added only where it can assemble. It is the one file here
# that would not compile on another chip, which is the reason it is its own file
# and not an #if in the middle of the filter.
set(srcs
    "src/pdm_decimator.c"
    "src/pdm_sinc6.c"
    "src/pdm_twostage.c"
    "src/pdm_stage2_scalar.c")

idf_build_get_property(target IDF_TARGET)
if(target STREQUAL "esp32s3")
    list(APPEND srcs "src/pdm_stage2_simd.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "include"
    PRIV_REQUIRES heap
)

# -O2 whatever the project asks for. The filter runs once per received word,
# 600 000 times a second for two channels at 4.8 MHz; at -Og the receiver
# overruns silently and the noise floor reads 20 dB too high. PRIVATE, so it
# applies to this component alone, and -Wall -Wextra -Werror likewise.
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -Wall -Wextra -Werror)
