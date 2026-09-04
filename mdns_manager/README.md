# mdns_manager

Advertises a device and its services over mDNS, so it can be found by name rather than by
whatever address DHCP handed out.

```c
const mdns_manager_config_t cfg = {
    .hostname = "esp-greenhouse", .instance_name = "Greenhouse Sensor",
};
mdns_manager_start(&cfg);    /* at boot, before there is any connectivity */

static const mdns_manager_txt_t txt[] = {{"path", "/"}};
static const mdns_manager_service_t http = {
    .type = "_http", .proto = "_tcp", .port = 80, .txt = txt, .txt_count = 1,
};
mdns_manager_add_service(&http);
```

It subscribes to `NET_EVENT_LINK_UP` and `NET_EVENT_AP_STARTED`, so nothing has to
sequence it against the network. Services registered before the link is up are advertised
as soon as it is, and the whole set is re-advertised after a reconnect.

It advertises on the device's **own access point** too: a device that found no known
network is exactly the one you need to reach by name, since there is no router to ask.

## Registration is data, not a weak symbol

The original exposed a `__attribute__((weak))` `mdns_manager_register_services()` that the
application overrode with a strong symbol. That worked only while everything was compiled
into one binary — `main.c` needed

```c
esp_err_t (*volatile dummy_ptr)() = mdns_manager_register_services;
```

purely to stop the linker discarding the override — and it gave a *component* no way to
advertise a service of its own. `mdns_manager_add_service()` replaces it.

Descriptors are **copied**, not referenced, so a caller may register from a stack frame or
from a config struct it later reloads. The table has to survive to be re-advertised after
every reconnect. Its size is `CONFIG_MDNS_MANAGER_MAX_SERVICES`.
