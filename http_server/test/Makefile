# Host test for Basic-auth credential handling. Pure logic, no ESP-IDF.
#
#   make -C http_server/test
#
COMP   := $(abspath $(CURDIR)/..)
CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -I$(COMP)/src
# ESP-IDF's export.sh puts cross toolchains ahead of /usr/bin; their ld cannot
# link a host binary.
LDFLAGS := -B/usr/bin

SRCS := test_http_auth.c $(COMP)/src/http_auth.c

.PHONY: all run clean
all: run

test_http_auth: $(SRCS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(SRCS) -o $@

run: test_http_auth
	./test_http_auth

clean:
	rm -f test_http_auth
