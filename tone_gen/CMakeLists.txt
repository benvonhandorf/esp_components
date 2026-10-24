# The public header includes only esp_err.h, so there are no public requirements.
# libm is for pow() in the sweep's frequency update and llrint() for phase steps;
# nothing in the per-sample path uses it.
idf_component_register(
    SRCS         "src/tone_gen_osc.c" "src/tone_gen.c" "src/tone_gen_dtmf.c"
    INCLUDE_DIRS "include")

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
