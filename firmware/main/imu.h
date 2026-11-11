#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/* Start the QMI8658 sampler (50 Hz) and motion-event logger.
 * Requires bsp_i2c_init() first. */
esp_err_t onurb_imu_start(void);

/* Stream the next `ms` of samples to the console as CSV for offline analysis:
 *   IMU,<capture#>,<t_ms>,ax,ay,az,gx,gy,gz      (m/s^2, deg/s)
 *   IMU_END,<capture#>,<samples>
 * Returns the capture number. */
int onurb_imu_capture_start(int ms);

/* True while a capture is still streaming. */
bool onurb_imu_capture_busy(void);

/* CSV of the last finished capture (header line + one row per sample);
 * valid until the next capture starts. */
const char *onurb_imu_capture_csv(size_t *len);

/* A decided mouth-pose entry waiting for the main loop (accepted = looks like
 * the trained raise-to-mouth gesture). Returns false if none. Detection stays
 * paused until onurb_imu_gesture_done(). */
bool onurb_imu_gesture_take(bool *accepted, const char **detail);

/* Its raw window (1.5 s before + 1 s after entry, capture CSV columns);
 * false until the post-entry second has been sampled. */
bool onurb_imu_gesture_csv(const char **csv, size_t *len);
void onurb_imu_gesture_done(void);

/* Latest tilt in degrees (0/0 before the first sample). */
void onurb_imu_get_tilt(float *pitch, float *roll);

/* Ends a gesture-started recording: the wrist left the mouth area for 0.4 s
 * (arm lowered) or a strong flick (>= 250 dps for 0.2 s). 0 = keep going. */
enum { ONURB_IMU_END_NONE = 0, ONURB_IMU_END_LEFT_MOUTH, ONURB_IMU_END_FLICK };
int onurb_imu_end_motion(void);

/* Current wrist rotation (deg/s, peak-held ~0.3 s): wakes the watch from doze. */
float onurb_imu_motion_dps(void);
