# The HTTP sink for audio_stream: an endless WAV response from a handler.
#
# esp_http_server and audio_stream are in the public header (httpd_req_t, the stream types).
# This depends on ESP-IDF's server, not on the http_server component of this repository, so a
# project with its own httpd setup can use it.
idf_component_register(
    SRCS         "src/audio_stream_http.c"
    INCLUDE_DIRS "include"
    REQUIRES     audio_stream esp_http_server
    PRIV_REQUIRES lwip esp_timer freertos)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)
