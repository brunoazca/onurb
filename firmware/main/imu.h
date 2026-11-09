#pragma once

#include "esp_err.h"

/* Start the QMI8658 sampler (50 Hz) and motion-event logger.
 * Requires bsp_i2c_init() first. */
esp_err_t onurb_imu_start(void);
