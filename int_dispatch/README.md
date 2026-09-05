# int_dispatch

Dispatch for a shared, wired-AND interrupt line — several I2C parts on one open-drain INT
pin.

```c
const int_dispatch_config_t cfg = { .pin = GPIO_NUM_14, .pull_up = true };
int_dispatch_start(&cfg);

int_dispatch_register(on_ina226_alert, ina);
int_dispatch_register(on_lm75_alert,   lm);
```

An edge says only that *something* wants attention, so every handler runs and checks its
own status register. There is no way to know which part asserted the line without asking
each one.

## Handlers run on a task, not in the ISR

Clearing an interrupt on an I2C part means a bus transaction, which cannot happen in
interrupt context. The ISR does nothing but notify.

## The line is re-checked afterwards

If it is still asserted once every handler has run, some part has an interrupt nobody
cleared. A further edge will never come — the line is level-asserted in effect — so the
dispatcher re-arms immediately. Without that, the device silently stops responding to that
part.

That case is **counted**. `int_dispatch_unserviced_count()` rising means a handler is not
clearing what it should, which otherwise shows up only as a device that feels slow.

## Start-up

A part may already be asserting the line when the dispatcher starts, in which case its
edge has been and gone. The line is checked once at start-up for exactly that reason.
