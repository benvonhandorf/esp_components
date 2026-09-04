# Host test for cli dispatch and argument parsing. Plain gcc: cli_dispatch.c and
# cli_args.c touch no ESP-IDF API beyond esp_err_t, so they compile off target
# against small stubs and the test exercises the real sources, not copies.
#
#   make -C cli/test
#
COMP   := $(abspath $(CURDIR)/..)
STUBS  := $(CURDIR)/stubs

CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -I$(COMP)/include -I$(STUBS)

# Sourcing ESP-IDF's export.sh puts cross toolchains ahead of /usr/bin on PATH,
# and their `ld` cannot link a host binary. -B names where to find it.
LDFLAGS := -B/usr/bin

SRCS := test_cli.c $(STUBS)/diag_capture.c $(COMP)/src/cli_dispatch.c $(COMP)/src/cli_args.c

.PHONY: all run clean
all: run

test_cli: $(SRCS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(SRCS) -o $@ -lm

run: test_cli
	./test_cli

clean:
	rm -f test_cli
