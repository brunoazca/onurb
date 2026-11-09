/*
 * IMU bring-up (groundwork for the trained gesture, SPEC §7.2-7.4):
 * samples the QMI8658 at 50 Hz, shows live tilt/motion on screen, and logs
 * one line per motion event (duration, peak rotation rate, start/end tilt)
 * so real wrist movements can be characterised before designing detection.
 */
#include <math.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "qmi8658.h"

#include "bsp/esp-bsp.h"

#include "imu.h"
#include "ui.h"

static const char *TAG = "onurb_imu";

#define SAMPLE_MS          20      /* 50 Hz */
#define MOVE_START_DPS     80.0f
#define MOVE_END_DPS       30.0f
#define MOVE_END_HOLD_MS   300
#define UI_EVERY_SAMPLES   10      /* 5 Hz */

static qmi8658_dev_t s_dev;

static void tilt(const qmi8658_data_t *d, float *pitch, float *roll)
{
    *pitch = atan2f(-d->accelX, sqrtf(d->accelY * d->accelY + d->accelZ * d->accelZ)) * 57.2958f;
    *roll = atan2f(d->accelY, d->accelZ) * 57.2958f;
}

static void imu_task(void *arg)
{
    qmi8658_data_t d;
    bool moving = false;
    int64_t move_start_ms = 0, quiet_since_ms = 0;
    float peak_dps = 0, p0 = 0, r0 = 0;
    int n = 0;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_MS));
        if (qmi8658_read_sensor_data(&s_dev, &d) != ESP_OK) {
            continue;
        }
        float pitch, roll;
        float dps = sqrtf(d.gyroX * d.gyroX + d.gyroY * d.gyroY + d.gyroZ * d.gyroZ);
        int64_t now_ms = (int64_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
        tilt(&d, &pitch, &roll);

        if (!moving && dps > MOVE_START_DPS) {
            moving = true;
            move_start_ms = now_ms;
            quiet_since_ms = 0;
            peak_dps = dps;
            p0 = pitch;
            r0 = roll;
        } else if (moving) {
            if (dps > peak_dps) {
                peak_dps = dps;
            }
            if (dps < MOVE_END_DPS) {
                if (quiet_since_ms == 0) {
                    quiet_since_ms = now_ms;
                } else if (now_ms - quiet_since_ms >= MOVE_END_HOLD_MS) {
                    moving = false;
                    ESP_LOGI(TAG, "motion: %.2f s, peak %.0f dps, tilt pitch %.0f->%.0f roll %.0f->%.0f",
                             (quiet_since_ms - move_start_ms) / 1000.0, peak_dps, p0, pitch, r0, roll);
                }
            } else {
                quiet_since_ms = 0;
            }
        }

        if (++n % UI_EVERY_SAMPLES == 0) {
            char line[64];
            if (moving) {
                snprintf(line, sizeof(line), "Moving %.0f deg/s", dps);
            } else {
                snprintf(line, sizeof(line), "Still  pitch %.0f  roll %.0f", pitch, roll);
            }
            onurb_ui_set_motion(line);
        }
    }
}

esp_err_t onurb_imu_start(void)
{
    esp_err_t err = qmi8658_init(&s_dev, bsp_i2c_get_handle(), QMI8658_ADDRESS_HIGH);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "QMI8658 init failed: %s", esp_err_to_name(err));
        return err;
    }
    qmi8658_set_accel_unit_mps2(&s_dev, true);
    qmi8658_set_accel_odr(&s_dev, QMI8658_ACCEL_ODR_125HZ);
    qmi8658_set_gyro_odr(&s_dev, QMI8658_GYRO_ODR_125HZ);

    xTaskCreatePinnedToCore(imu_task, "imu", 4096, NULL, 4, NULL, 1);
    ESP_LOGI(TAG, "IMU running: 50 Hz, accel 8 g, gyro 512 dps");
    return ESP_OK;
}
