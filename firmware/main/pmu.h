#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Configure the AXP2101 charger the way Waveshare's official 01_AXP2101 example
 * does (TS pin detection off, 4.2 V cut-off), with the charge current from
 * Kconfig. Only charger/measurement settings are touched - never power rails.
 * Requires bsp_i2c_init() first. */
esp_err_t onurb_pmu_init(void);

/* Log battery/charger state (B-06). */
void onurb_pmu_log_status(void);

#ifdef __cplusplus
}
#endif
