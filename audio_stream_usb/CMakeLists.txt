# The USB sink for audio_stream. usb_mic compiles to stubs on a chip with no USB OTG
# controller, so this builds for every target and audio_stream_usb_check() says
# ESP_ERR_NOT_SUPPORTED where there is none.
#
# Both siblings appear in the public header (audio_stream_t types, usb_mic_check_t).
idf_component_register(
    SRCS         "src/audio_stream_usb.c"
    INCLUDE_DIRS "include"
    REQUIRES     audio_stream usb_mic)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
