# Host test for the PDM decimators. No ESP-IDF, no hardware.
#
#   make -C pdm_decimator/test
#
# It builds the real sources, and not the vector file: that cannot assemble here,
# and the portable stage 2 it is checked against is what these tests exercise.
# The vector version is compared with it on the chip, by `audio pdmbench`.
COMP   := $(abspath $(CURDIR)/..)
CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -I$(COMP)/include -I$(COMP)/src \
          -I$(CURDIR)/stub
# ESP-IDF's export.sh puts cross toolchains ahead of /usr/bin; their ld cannot
# link a host binary.
LDFLAGS := -B/usr/bin

SRCS := test_pdm_decimator.c \
        $(COMP)/src/pdm_decimator.c \
        $(COMP)/src/pdm_sinc6.c \
        $(COMP)/src/pdm_twostage.c \
        $(COMP)/src/pdm_stage2_scalar.c

.PHONY: all run clean
all: run

test_pdm_decimator: $(SRCS) stub/esp_err.h
	$(CC) $(CFLAGS) $(LDFLAGS) $(SRCS) -o $@ -lm

run: test_pdm_decimator
	./test_pdm_decimator

clean:
	rm -f test_pdm_decimator
