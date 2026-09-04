# Host test for MQTT topic matching and building. Pure string logic, no ESP-IDF.
#
#   make -C mqtt_manager/test
#
COMP   := $(abspath $(CURDIR)/..)
CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -I$(COMP)/src
# ESP-IDF's export.sh puts cross toolchains ahead of /usr/bin; their ld cannot
# link a host binary.
LDFLAGS := -B/usr/bin

SRCS := test_mqtt_topic.c $(COMP)/src/mqtt_topic.c

.PHONY: all run clean
all: run

test_mqtt_topic: $(SRCS)
	$(CC) $(CFLAGS) $(LDFLAGS) $(SRCS) -o $@

run: test_mqtt_topic
	./test_mqtt_topic

clean:
	rm -f test_mqtt_topic
