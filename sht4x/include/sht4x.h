/*
 * Sensirion SHT4x humidity and temperature sensor.
 *
 * Command bytes, timings, CRC parameters and conversion formulas come from the
 * SHT4x datasheet, version 7.1 (March 2025).
 *
 * This driver formats no text. Calls return esp_err_t and fill out-structs with
 * facts -- which word of a reply failed its CRC and what was computed for it,
 * whether a humidity value had to be cropped to the physical range -- so the
 * caller can report them in whatever way suits it.
 */
#ifndef SHT4X_H
#define SHT4X_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Part variants differ only in their fixed address: A=0x44, B=0x45, C=0x46.
 * There are no address pins, so the address is a fact about which part was
 * fitted rather than about how it was strapped.
 */
#define SHT4X_ADDR_FIRST   0x44
#define SHT4X_ADDR_LAST    0x46
#define SHT4X_ADDR_DEFAULT 0x44

/* Error codes. The base is outside the range ESP-IDF assigns to itself, and
 * distinct from the other drivers in this repository so that esp_err_to_name()
 * cannot attribute one part's failure to another. */
#define ESP_ERR_SHT4X_BASE 0x32000
/*
 * A reply arrived but did not survive its CRC. Distinct from a transfer error
 * because the bus worked -- the bytes are wrong, which points at signal
 * integrity or at something that is not an SHT4x, not at a missing part.
 */
#define ESP_ERR_SHT4X_CRC (ESP_ERR_SHT4X_BASE + 1)

/* Table 8, Overview of I2C commands. The enumerator value is the command byte. */
typedef enum {
    SHT4X_REPEATABILITY_HIGH   = 0xFD,
    SHT4X_REPEATABILITY_MEDIUM = 0xF6,
    SHT4X_REPEATABILITY_LOW    = 0xE0,
} sht4x_repeatability_t;

/*
 * The six heater settings the device implements. The enumerator value is the
 * command byte; power and duration are recovered with the accessors below.
 *
 * Each command runs the heater then takes a high-precision reading just before
 * switching it off, so every one of these returns a measurement.
 */
typedef enum {
    SHT4X_HEATER_200MW_1S    = 0x39,
    SHT4X_HEATER_200MW_100MS = 0x32,
    SHT4X_HEATER_110MW_1S    = 0x2F,
    SHT4X_HEATER_110MW_100MS = 0x24,
    SHT4X_HEATER_20MW_1S     = 0x1E,
    SHT4X_HEATER_20MW_100MS  = 0x15,
} sht4x_heater_t;

/*
 * Encoding tables, exported so a caller can render the legal sets without
 * keeping a second copy of them. Values that are not defined return -1 / NULL.
 */
const char *sht4x_repeatability_name(sht4x_repeatability_t r);
esp_err_t sht4x_repeatability_from_name(const char *name,
                                        sht4x_repeatability_t *out);
extern const sht4x_repeatability_t sht4x_repeatabilities[3];

int sht4x_heater_power_mw(sht4x_heater_t h);    /* 20, 110, 200; -1 if invalid */
int sht4x_heater_duration_ms(sht4x_heater_t h); /* 100, 1000;    -1 if invalid */
esp_err_t sht4x_heater_from_values(int power_mw, int duration_ms,
                                   sht4x_heater_t *out);
extern const sht4x_heater_t sht4x_heaters[6];

/* Opaque handle. The address is carried by the I2C device handle. */
typedef struct sht4x_dev_t *sht4x_handle_t;

/*
 * `dev` may be NULL: a handle can be created before there is a bus, and every
 * call that would talk to the part returns ESP_ERR_INVALID_STATE until one
 * arrives via sht4x_set_device().
 */
typedef struct {
    i2c_master_dev_handle_t dev;
} sht4x_config_t;

/*
 * Which word of a two-word reply failed its CRC, and what was expected.
 *
 * Only meaningful when a call returns ESP_ERR_SHT4X_CRC. `word` is 1 or 2;
 * `received` is the CRC byte the sensor sent and `computed` is what the data
 * bytes actually hash to -- quoting both is what distinguishes a corrupted
 * byte from a part that does not implement this CRC at all.
 */
typedef struct {
    int word;
    uint8_t received;
    uint8_t computed;
} sht4x_crc_error_t;

typedef struct {
    double temperature_c;
    /*
     * The conversion can yield non-physical values just outside 0-100 %RH.
     * The datasheet expects those to be cropped, but both numbers are reported
     * because during bringup a wildly out-of-range value means a real problem
     * and cropping would hide it.
     */
    double humidity_pct;          /* as converted, uncropped */
    double humidity_pct_cropped;  /* clamped to 0-100 */
    bool humidity_was_cropped;

    uint16_t temperature_ticks;
    uint16_t humidity_ticks;

    sht4x_crc_error_t crc_error;  /* valid only on ESP_ERR_SHT4X_CRC */
} sht4x_measurement_t;

esp_err_t sht4x_create(const sht4x_config_t *config, sht4x_handle_t *out);

/* Free the handle. Does NOT delete the I2C device handle, which the caller owns. */
esp_err_t sht4x_delete(sht4x_handle_t handle);

/*
 * Re-point the handle at a device.
 *
 * This part has no per-device state to carry, so one handle can serve every
 * address on the bus by being re-pointed between calls. Callers whose I2C layer
 * caches device handles must do this before each use in any case: a cache that
 * recycles entries, or a bus that is torn down and rebuilt, leaves a previously
 * held handle dangling.
 */
esp_err_t sht4x_set_device(sht4x_handle_t handle, i2c_master_dev_handle_t dev);

/* Take one measurement at the given repeatability. */
esp_err_t sht4x_measure(sht4x_handle_t handle, sht4x_repeatability_t r,
                        sht4x_measurement_t *out);

/*
 * Run the heater, then measure. Blocks for the whole pulse plus the
 * measurement -- up to about 1.2 s for the 1 s settings.
 */
esp_err_t sht4x_run_heater(sht4x_handle_t handle, sht4x_heater_t h,
                           sht4x_measurement_t *out);

/*
 * Read the 32-bit serial number.
 *
 * The SHT4x has no ID register; a serial number that reads back with valid
 * CRCs is the available evidence that a real sensor is present.
 *
 * `crc_error` may be NULL; it is filled only on ESP_ERR_SHT4X_CRC.
 */
esp_err_t sht4x_read_serial(sht4x_handle_t handle, uint32_t *out,
                            sht4x_crc_error_t *crc_error);

/* Soft reset. Returns once the device has had time to come back. */
esp_err_t sht4x_soft_reset(sht4x_handle_t handle);

#ifdef __cplusplus
}
#endif

#endif
