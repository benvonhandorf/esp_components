# ntp_manager

Sets the system clock from NTP, once there is a network to do it over.

```c
ntp_manager_start(&cfg);   /* at boot, before there is any connectivity */
```

It subscribes to `NET_EVENT_LINK_UP` and does nothing until one arrives, so **no caller
has to sequence it against the network coming up**. On a successful sync it posts
`NET_EVENT_TIME_SYNCED`, which is what anything timestamping records should wait for —
before that, the clock holds whatever the last boot left behind.

Deliberately *not* started on `NET_EVENT_AP_STARTED`: an access point the device serves
itself has clients but no route to a time server.

## Configuration

`ntp_manager_config_schema.json` → `ntp_manager_config_t`. `server`, an optional
`server_fallback`, `sync_interval_ms`, `enabled`, and a POSIX `timezone` string.

The timezone is applied **before** the first sync, not after: `localtime()` reads `TZ` at
call time, so a record timestamped between the sync and a later `setenv` would be
formatted in the wrong zone.

### One server, unless the build allows more

`esp_sntp_config_t::servers` is sized by `CONFIG_LWIP_SNTP_MAX_SERVERS`, whose **default
is 1**. A second server is therefore not merely ignored — writing it would run off the end
of the struct. The fallback is used only where the build has room, and logs a warning
naming `CONFIG_LWIP_SNTP_MAX_SERVERS` otherwise.

## On reconnect

SNTP is restarted rather than left alone, because the servers may resolve differently the
second time — a `.local` name cannot be looked up until mDNS is up, so a client that
failed its first resolution would otherwise sit retrying a lookup that already failed.

A brief `LINK_DOWN` does *not* tear it down: esp-sntp retries on its own, and restarting
would lose the sync interval on every flap.
