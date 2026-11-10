# Format, ring, runner and queue. No USB and no HTTP: those are the sink components
# (audio_stream_usb, audio_stream_http), so a project takes only the one it uses.
#
# The public headers include only esp_err.h, so there are no public requirements.
# pdm_decimator is for a raw PDM source; heap is for the queue's external-RAM buffer.
idf_component_register(
    SRCS         "src/audio_stream_fmt.c" "src/audio_stream_ring.c" "src/audio_stream.c"
                 "src/audio_stream_queue.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES pdm_decimator freertos heap esp_hw_support)

# -O2 whatever the project asks for: the conversion loop runs once per sample, 600 000 times
# a second for stereo at 300 kHz, beside a decimator that already uses most of a core.
# PRIVATE, so it applies to this component alone, and -Wall -Wextra -Werror likewise.
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -Wall -Wextra -Werror)
