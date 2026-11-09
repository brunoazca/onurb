#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Configure the AXP2101 charger the way Waveshare's official 01_AXP2101 example
 * does (TS pin detection off, 4.2 V cut-off), with the charge current from
 * Kconfig. Only charger/measurement settings are touched - never power rails.
 * Requires bsp_i2c_init() first. */
esp_err_t onurb_pmu_init(void);

typedef struct {
    bool battery;
    int percent;
    int millivolts;
    bool charging;
    bool usb_in;
} onurb_pmu_status_t;

/* Read battery/charger state; returns ESP_ERR_INVALID_STATE before init. */
esp_err_t onurb_pmu_get_status(onurb_pmu_status_t *out);

/* Log battery/charger state (B-06). */
void onurb_pmu_log_status(void);

#ifdef __cplusplus
}
#endif
