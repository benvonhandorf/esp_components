# Changelog

All notable changes to this component are documented here.
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [0.1.0] - 2026-09-05

Initial release, extracted from `solar_power_monitor`'s `main/int_handler/`.

### Changed from the original

- **The pin is a parameter.** It was `#define INT_GPIO GPIO_NUM_14`, which is a fact about
  one board.
- **Handlers take a context pointer**, so a handler can serve one of several instances of
  the same part rather than reaching for a global.
- **The GPIO ISR service being already installed is not an error.** It is process-wide, so
  a project that installs it elsewhere would otherwise fail here.
- **The line is checked once at start-up.** A part already asserting it has had its edge,
  and that edge will not come again — without this the device ignores that part until
  something else happens to assert the line.
- **Unserviced assertions are counted.** A handler that fails to clear its part turns a
  shared line into a busy loop; `int_dispatch_unserviced_count()` makes that visible
  instead of the device merely feeling slow.

### Not yet verified on hardware
