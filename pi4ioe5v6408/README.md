# pi4ioe5v6408

Diodes PI4IOE5V6408 8-bit I/O expander with configurable pulls, interrupt-on-change and
5 V tolerant inputs.

```c
pi4ioe5v6408_config_t cfg = {
    .dev          = i2c_device_handle(PI4IOE5V6408_I2C_ADDR_DEFAULT),
    .outputs      = 0xF0,   /* P4-P7 drive, P0-P2 sense buttons */
    .initial      = 0xF0,   /* LEDs on the cathode side: high is off */
    .pull_enable  = 0x07,   /* buttons switch to ground with no external pull-up */
    .pull_up      = 0x07,
    .interrupt_on = 0x07,
};

pi4ioe5v6408_handle_t io;
pi4ioe5v6408_create(&cfg, &io, NULL);
```

## Pins are numbered, not named

The driver this replaces had one product's peripherals in its API: `set_backlight()`,
`set_led(led, on)` and `read_buttons()`, with the LED polarity and the button-to-pin
mapping compiled in. What P4 drives is a fact about the board, so naming belongs in board
code:

```c
static inline void board_backlight(bool on) { pi4ioe5v6408_set_pin(io, 7, !on); }
```

## Levels before direction, and a shadow for set_pin()

As with any expander driving a load: `initial` is established before pins become outputs,
and `set_pin()` works from the last written value because the input register reflects pin
state rather than what was driven.

## It does not own the bus

`cfg.dev` is a device handle the caller created and owns, and it **may be NULL** -- the
handle is still created, and every call needing the bus returns `ESP_ERR_INVALID_STATE`
until `pi4ioe5v6408_set_device()` supplies one. That lets a handle exist before anything is
powered.

A bus that is torn down and rebuilt invalidates every device handle taken from it, so
whoever owns the bus calls `pi4ioe5v6408_set_device()` again rather than this driver caching a
handle that can dangle.
