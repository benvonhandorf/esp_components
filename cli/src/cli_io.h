#ifndef CLI_IO_H
#define CLI_IO_H

#include <stdbool.h>

/*
 * Serial transport for the shell. Private to the component: which device the
 * console sits on is decided by CONFIG_ESP_CONSOLE_*, not by the caller.
 */

/* Install the console driver and route stdio through it. */
void cli_io_init(void);

/* Human-readable device name for the banner, e.g. "USB-Serial-JTAG". */
const char *cli_io_name(void);

/*
 * Whether a host is attached. Meaningful on USB-Serial-JTAG, where a board
 * typically boots with nothing plugged in; always true on a UART, which has no
 * notion of an attached host.
 */
bool cli_io_host_connected(void);

#endif /* CLI_IO_H */
