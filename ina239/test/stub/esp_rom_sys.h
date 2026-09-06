/* Host stub: see esp_err.h. The reset path's settling delay costs nothing off
 * target, and sleeping for it would only slow the test suite down. */
#ifndef STUB_ESP_ROM_SYS_H
#define STUB_ESP_ROM_SYS_H

static inline void esp_rom_delay_us(unsigned us) { (void)us; }

#endif
