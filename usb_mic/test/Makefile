# Host test for the usb_mic arithmetic. No ESP-IDF, no hardware.
#
#   make -C usb_mic/test
COMP   := $(abspath $(CURDIR)/..)
CFLAGS := -std=gnu11 -O1 -g -Wall -Wextra -Werror -I$(COMP)/include -I$(CURDIR)/stub
LDFLAGS := -B/usr/bin

SRCS := test_usb_mic.c $(COMP)/src/usb_mic_calc.c $(COMP)/src/usb_mic_desc.c

.PHONY: all run clean
all: run

test_usb_mic: $(SRCS) stub/esp_err.h $(COMP)/include/usb_mic.h $(COMP)/src/usb_mic_desc.h
	$(CC) $(CFLAGS) $(LDFLAGS) $(SRCS) -o $@

run: test_usb_mic
	./test_usb_mic

clean:
	rm -f test_usb_mic
