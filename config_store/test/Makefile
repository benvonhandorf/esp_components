# Host test for config_store. The component uses only stdio and paths, so this
# runs the real source against real files in a temporary directory.
#
#   make -C config_store/test
#
COMP   := $(abspath $(CURDIR)/..)
STUBS  := $(CURDIR)/stubs

CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -I$(COMP)/include -I$(STUBS)

# Sourcing ESP-IDF's export.sh puts cross toolchains ahead of /usr/bin on PATH,
# and their `ld` cannot link a host binary. -B names where to find it.
LDFLAGS := -B/usr/bin

SRCS := test_config_store.c $(COMP)/src/config_store.c

.PHONY: all run clean
all: run

test_config_store: $(SRCS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(SRCS) -o $@

run: test_config_store
	./test_config_store

clean:
	rm -f test_config_store
