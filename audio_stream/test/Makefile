# Host test for the format half of audio_stream and its ring. No ESP-IDF, no hardware.
#
#   make -C audio_stream/test
#
# The runner and the queue need FreeRTOS and are checked on the chip.
COMP   := $(abspath $(CURDIR)/..)
CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -pthread \
          -I$(COMP)/include -I$(COMP)/src -I$(CURDIR)/stub -I$(COMP)/../pdm_decimator/include
# ESP-IDF's export.sh puts cross toolchains ahead of /usr/bin; their ld cannot
# link a host binary.
LDFLAGS := -B/usr/bin

SRCS := test_audio_stream.c $(COMP)/src/audio_stream_fmt.c $(COMP)/src/audio_stream_ring.c

.PHONY: all run clean
all: run

test_audio_stream: $(SRCS) stub/esp_err.h
	$(CC) $(CFLAGS) $(LDFLAGS) $(SRCS) -o $@

run: test_audio_stream
	./test_audio_stream

clean:
	rm -f test_audio_stream
