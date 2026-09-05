# Host test for the INA226's calibration arithmetic. No ESP-IDF, no hardware.
#
#   make -C ina226/test
#
COMP   := $(abspath $(CURDIR)/..)
CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -I$(COMP)/src
# ESP-IDF's export.sh puts cross toolchains ahead of /usr/bin; their ld cannot
# link a host binary.
LDFLAGS := -B/usr/bin

SRCS := test_ina226_calc.c $(COMP)/src/ina226_calc.c

.PHONY: all run clean
all: run

test_ina226_calc: $(SRCS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(SRCS) -o $@ -lm

run: test_ina226_calc
	./test_ina226_calc

clean:
	rm -f test_ina226_calc
