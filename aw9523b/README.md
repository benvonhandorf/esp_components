# aw9523b

Awinic AW9523B 16-bit I/O expander over I2C.

```c
aw9523b_config_t cfg = {
    .dev             = i2c_device_handle(AW9523B_I2C_ADDR_DEFAULT),
    .port0_inputs    = 0xF0,   /* P0.0-P0.3 drive relays, P0.4-P0.7 sense */
    .port0_initial   = 0x00,   /* relays open before the pins become outputs */
    .port0_push_pull = true,
    .port1_inputs    = 0xFF,
};

aw9523b_handle_t aw;
aw9523b_create(&cfg, &aw, NULL);
aw9523b_set_pin(aw, AW9523B_PORT0, 2, true);
```

## Levels before direction

`port0_initial` and `port1_initial` are written **before** any pin becomes an output.
Switching direction first lets whatever the output register happened to hold reach the
pins -- on a board driving relays that is an audible clack at every reset, and on one
driving FETs it can be worse.

## Port 0 is open-drain by default

Set `port0_push_pull` to drive it high. This surprises anyone expecting a GPIO to source
current.

## Reading a port gives inputs, not outputs

The part has no readable output register, so `set_pin()` works from a shadow of the last
value written. A read-modify-write through `read_port()` would take the *input* levels and
write them back over the outputs.

## It does not own the bus

`cfg.dev` is a device handle the caller created and owns, and it **may be NULL** -- the
handle is still created, and every call needing the bus returns `ESP_ERR_INVALID_STATE`
until `aw9523b_set_device()` supplies one. That lets a handle exist before anything is
powered.

A bus that is torn down and rebuilt invalidates every device handle taken from it, so
whoever owns the bus calls `aw9523b_set_device()` again rather than this driver caching a
handle that can dangle.
