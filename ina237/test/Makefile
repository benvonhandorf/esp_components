# Host test for the INA237. No ESP-IDF, no hardware: stub/ supplies just enough
# of esp_err.h and driver/i2c_master.h to build the driver, and fake_i2c.c
# answers its transfers from a register map.
#
#   make -C ina237/test
#
COMP   := $(abspath $(CURDIR)/..)
CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -Istub -I$(COMP)/include -I$(COMP)/src
# ESP-IDF's export.sh puts cross toolchains ahead of /usr/bin; their ld cannot
# link a host binary.
LDFLAGS := -B/usr/bin

SRCS := test_ina237.c fake_i2c.c $(COMP)/src/ina237.c

.PHONY: all run clean
all: run

test_ina237: $(SRCS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(SRCS) -o $@ -lm

run: test_ina237
	./test_ina237

clean:
	rm -f test_ina237
