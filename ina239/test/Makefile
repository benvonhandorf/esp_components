# Host test for the INA239. No ESP-IDF, no hardware: stub/ supplies just enough
# of esp_err.h, driver/spi_master.h and esp_rom_sys.h to build the driver, and
# fake_spi.c answers its transfers from a register map -- decoding the frame the
# way the datasheet describes rather than the way the driver encodes it.
#
#   make -C ina239/test
#
COMP   := $(abspath $(CURDIR)/..)
CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -Istub -I$(COMP)/include -I$(COMP)/src
# ESP-IDF's export.sh puts cross toolchains ahead of /usr/bin; their ld cannot
# link a host binary.
LDFLAGS := -B/usr/bin

SRCS := test_ina239.c fake_spi.c $(COMP)/src/ina239.c

.PHONY: all run clean
all: run

test_ina239: $(SRCS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(SRCS) -o $@ -lm

run: test_ina239
	./test_ina239

clean:
	rm -f test_ina239
