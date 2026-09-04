# Changelog

All notable changes to this component are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [0.1.0] - 2026-09-04

### Added

- Read with an optional override path consulted first, so a config on removable
  media can reconfigure a device without reflashing it.
- Write with rotation: the previous config becomes `.bak` before the new one takes
  its place, so an interrupted write loses the new config rather than the working one.
- `config_store_restore_backup()` for when a newly written config turns out not to work.
- `config_store_schedule_restart()`, kept in its own translation unit so the file
  handling stays free of ESP-IDF and can be host-tested against real files.

Assembled from `solar_power_monitor`'s `main/config_reader/` and the config-write path
in `main/http/http_actions.c`.

### Changed from the original

- **No filesystem dependency.** The original mounted LittleFS itself
  (`esp_vfs_littlefs_register` on a partition named `res`). Mounting is the
  application's decision -- it might be FAT, or an SD card, or nothing at all under
  test -- so this takes paths and the caller mounts. It is the same rule the drivers
  follow: the component takes what it is given rather than going looking.
- **SD-card override implemented.** The original project documented falling back from
  an SD card to LittleFS but contained no SD code at all.
- **Writes rotate rather than timestamp.** The original wrote
  `config-backup-<timestamp>.json`, which accumulates files on a partition with no
  quota and needs a clock that may not be set. One `.bak` matches what the project
  README describes.
- **A path whose `.bak` form would not fit is refused at init**, rather than at the
  first write -- which would be after the device has been running for days.
