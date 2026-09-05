/*
 * Just enough of ESP-IDF's esp_err.h to build the driver on the host. The
 * component's public header takes an i2c_master_dev_handle_t and returns
 * esp_err_t, so an off-target test needs both; nothing here is used by the
 * firmware build.
 */
#ifndef STUB_ESP_ERR_H
#define STUB_ESP_ERR_H

typedef int esp_err_t;

#define ESP_OK                0
#define ESP_FAIL             -1
#define ESP_ERR_NO_MEM        0x101
#define ESP_ERR_INVALID_ARG   0x102
#define ESP_ERR_INVALID_STATE 0x103

#endif
