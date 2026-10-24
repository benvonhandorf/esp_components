# Host test for the tone generator. No ESP-IDF, no hardware.
#
#   make -C tone_gen/test
#
# `WAV=1 make -C tone_gen/test run` also writes the signals to *.wav in this
# directory, for listening to.
COMP   := $(abspath $(CURDIR)/..)
CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -I$(COMP)/include -I$(CURDIR)/stub
# ESP-IDF's export.sh puts cross toolchains ahead of /usr/bin; their ld cannot
# link a host binary.
LDFLAGS := -B/usr/bin

SRCS := test_tone_gen.c $(wildcard $(COMP)/src/*.c)

.PHONY: all run clean
all: run

test_tone_gen: $(SRCS) stub/esp_err.h
	$(CC) $(CFLAGS) $(LDFLAGS) $(SRCS) -o $@ -lm

run: test_tone_gen
	./test_tone_gen

clean:
	rm -f test_tone_gen *.wav
