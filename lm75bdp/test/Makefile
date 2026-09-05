# Host test for the INA226's calibration arithmetic. No ESP-IDF, no hardware.
#
#   make -C lm75bdp/test
#
COMP   := $(abspath $(CURDIR)/..)
CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -I$(COMP)/src
# ESP-IDF's export.sh puts cross toolchains ahead of /usr/bin; their ld cannot
# link a host binary.
LDFLAGS := -B/usr/bin

SRCS := test_lm75bdp_calc.c $(COMP)/src/lm75bdp_calc.c

.PHONY: all run clean
all: run

test_lm75bdp_calc: $(SRCS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(SRCS) -o $@ -lm

run: test_lm75bdp_calc
	./test_lm75bdp_calc

clean:
	rm -f test_lm75bdp_calc
