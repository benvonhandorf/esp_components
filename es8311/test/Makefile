# Host test for the ES8311 clock and volume arithmetic. No ESP-IDF, no hardware.
#
#   make -C es8311/test
#
# Only es8311_calc.c is built: the driver proper needs the I2C master and
# FreeRTOS. What fails silently -- a divider that plays at the wrong pitch -- is
# all in the calc file.
COMP   := $(abspath $(CURDIR)/..)
CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -I$(COMP)/include -I$(CURDIR)/stub
# ESP-IDF's export.sh puts cross toolchains ahead of /usr/bin; their ld cannot
# link a host binary.
LDFLAGS := -B/usr/bin

.PHONY: all run clean
all: run

test_es8311: test_es8311.c $(COMP)/src/es8311_calc.c stub/esp_err.h
	$(CC) $(CFLAGS) $(LDFLAGS) test_es8311.c $(COMP)/src/es8311_calc.c -o $@

run: test_es8311
	./test_es8311

clean:
	rm -f test_es8311
