/*
 * IMU bring-up (groundwork for the trained gesture, SPEC §7.2-7.4):
 * samples the QMI8658 at 50 Hz, shows live tilt/motion on screen, and logs
 * one line per motion event (duration, peak rotation rate, start/end tilt)
 * so real wrist movements can be characterised before designing detection.
 * onurb_imu_capture_start() streams labelled raw traces (tap-to-capture).
 * Raise-to-mouth detection (v3). Trigger: the watch ENTERS the mouth pose
 * (pitch -65..-5, |roll| >= 140). Decision 0.4 s later, all required:
 *   - gate: >= 200 dps peak and >= 10 deg orientation change in the 1.5 s
 *     before entry (v2 rule, keeps the classifier in its training domain)
 *   - classifier: logistic regression on integrated rotation per axis and
 *     peak-to-entry time, trained on the user's green/red feedback
 *     (tools/gesture_train.py -> gesture_model.h)
 *   - dwell: in a looser mouth box (pitch -80..+25, |roll| >= 120) for
 *     >= 80 % of the 0.4 s (talking = holding it there)
 * Every entry (accepted or not) is uploaded with 1.5 s before / 1 s after.
 * History: v1 waited for the wrist to go still - it never does at the mouth;
 * v2 (rule only) was ~45 % precise on the first feedback session.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "qmi8658.h"

#include "bsp/esp-bsp.h"

#include "gesture_model.h"
#include "imu.h"

static const char *TAG = "onurb_imu";

#define SAMPLE_MS          20      /* 50 Hz */
#define MOVE_START_DPS     80.0f
#define MOVE_END_DPS       30.0f
#define MOVE_END_HOLD_MS   300
#define RING               160     /* 3.2 s of raw samples */
#define WIN                75      /* 1.5 s look-back */
#define POSE_PITCH_MIN     -65.0f
#define POSE_PITCH_MAX     -5.0f
#define POSE_ROLL_ABS_MIN  140.0f
#define BURST_DPS          300.0f  /* 95 % of real raises peak >= 372 dps; 200 let tiny moves through */
#define GRAVITY_CHANGE_DEG 15.0f  /* 95 % of real raises change >= 15 deg */
#define REFRACTORY_MS      3000
#define DWELL_SAMPLES      20      /* 0.4 s */
#define DWELL_MIN          12      /* 60 %: 80 % missed real raises that kept moving */
/* Looser box for the dwell than for entry: seated, the watch sits flatter near
 * the mouth (pitch up to ~+16) right after entering the strict pose. */
#define DWELL_PITCH_MIN    -80.0f
#define DWELL_PITCH_MAX    50.0f   /* some raises settle at pitch +27..+45 */
#define DWELL_ROLL_ABS_MIN 75.0f   /* 120 rejected the lying-down raises (|roll| ~100) */
#define POST_SAMPLES       50      /* 1 s after entry kept for training */
/* End of a gesture-started recording. 120 dps / 60 ms cut users off mid-
 * sentence (hand moves while talking), so it now takes either leaving the
 * mouth area (lowering the arm) or a deliberate strong flick. */
#define END_OUT_PITCH_MAX  60.0f   /* above the widest mouth pitch seen (+45) */
#define END_OUT_PITCH_MIN  -85.0f
#define END_OUT_ROLL_MIN   70.0f   /* lying-down mouth position is |roll| ~100 */
#define END_OUT_SAMPLES    20      /* 0.4 s outside */
#define END_DPS            250.0f
#define END_SAMPLES        10      /* 0.2 s above END_DPS */

static qmi8658_dev_t s_dev;
static volatile int s_capture_left;   /* samples still to stream */
static volatile int s_capture_id;
static int s_capture_n;
static char s_csv[16 * 1024];         /* ~125 rows x ~60 chars */
static size_t s_csv_len;
static volatile float s_pitch, s_roll;

static float s_ring[RING][6];          /* raw ax ay az (m/s^2) gx gy gz (dps) */
static uint32_t s_idx;                 /* total samples taken */
static volatile int s_strong_run;      /* consecutive samples >= END_DPS */
static volatile int s_out_run;         /* consecutive samples outside the mouth area */
/* Candidate state, owned by the IMU task until handed over. */
static bool s_cand_active;
static uint32_t s_cand_entry;
static bool s_cand_gate;
static float s_cand_prob;
static int s_cand_dwell;

static volatile bool s_gesture_pending;     /* decision ready for the main loop */
static volatile bool s_gesture_csv_ready;   /* window incl. post-entry samples ready */
static bool s_gesture_accepted;
static char s_gesture_detail[128];
static char s_gesture_csv[16 * 1024];
static size_t s_gesture_csv_len;

static void tilt(const qmi8658_data_t *d, float *pitch, float *roll)
{
    *pitch = atan2f(-d->accelX, sqrtf(d->accelY * d->accelY + d->accelZ * d->accelZ)) * 57.2958f;
    *roll = atan2f(d->accelY, d->accelZ) * 57.2958f;
}

/* Lying down / reclined (Bruno's captures 2026-11-10): the resting arm already
 * sits in the upright mouth box, and the raise ends near pitch -47, |roll| ~100.
 * So entering this second box also counts as a pose entry. */
#define LYING_PITCH_MIN   -70.0f
#define LYING_PITCH_MAX   -30.0f
#define LYING_ROLL_MIN     80.0f
#define LYING_ROLL_MAX    135.0f

/* 0 = not at the mouth, 1 = upright mouth box, 2 = lying-down mouth box. A pose
 * entry is moving into a box from outside it, including box 1 -> box 2 (lying:
 * the resting arm is already in box 1). */
static int mouth_region(float pitch, float roll)
{
    float r = fabsf(roll);
    if (pitch >= POSE_PITCH_MIN && pitch <= POSE_PITCH_MAX && r >= POSE_ROLL_ABS_MIN) {
        return 1;
    }
    if (pitch >= LYING_PITCH_MIN && pitch <= LYING_PITCH_MAX && r >= LYING_ROLL_MIN && r <= LYING_ROLL_MAX) {
        return 2;
    }
    return 0;
}

static bool in_dwell_pose(float pitch, float roll)
{
    return pitch >= DWELL_PITCH_MIN && pitch <= DWELL_PITCH_MAX && fabsf(roll) >= DWELL_ROLL_ABS_MIN;
}

static float gravity_angle(const float *a, const float *b)
{
    float na = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    float nb = sqrtf(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
    float c = (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) / ((na * nb) > 0 ? na * nb : 1);
    c = c > 1 ? 1 : (c < -1 ? -1 : c);
    return acosf(c) * 57.2958f;
}

/* Classifier probability from the 1.5 s before entry (mirrors gesture_train.py). */
static float classify(uint32_t start, uint32_t entry)
{
    float rot[3] = {0}, peak = -1;
    uint32_t peak_i = entry;

    for (uint32_t i = start; i <= entry; i++) {
        const float *r = s_ring[i % RING];
        float dps = sqrtf(r[3] * r[3] + r[4] * r[4] + r[5] * r[5]);
        for (int k = 0; k < 3; k++) {
            rot[k] += r[3 + k] * (SAMPLE_MS / 1000.0f);
        }
        if (dps > peak) {
            peak = dps;
            peak_i = i;
        }
    }
    const float x[GESTURE_N_FEATURES] = {rot[0], rot[1], rot[2], (entry - peak_i) * (SAMPLE_MS / 1000.0f)};
    float z = gesture_bias;
    for (int k = 0; k < GESTURE_N_FEATURES; k++) {
        z += gesture_weight[k] * (x[k] - gesture_mean[k]) / gesture_std[k];
    }
    return 1.0f / (1.0f + expf(-z));
}

/* The watch just entered the mouth pose: start a candidate; decided after the dwell. */
static void on_pose_entry(void)
{
    if (s_cand_active || s_gesture_pending || s_capture_left > 0 || s_idx < WIN) {
        return;
    }
    uint32_t start = s_idx - WIN;
    float peak = 0;
    for (uint32_t i = start; i <= s_idx; i++) {
        const float *r = s_ring[i % RING];
        float dps = sqrtf(r[3] * r[3] + r[4] * r[4] + r[5] * r[5]);
        if (dps > peak) {
            peak = dps;
        }
    }
    float dgrav = gravity_angle(s_ring[start % RING], s_ring[s_idx % RING]);
    s_cand_gate = peak >= BURST_DPS && dgrav >= GRAVITY_CHANGE_DEG;
    s_cand_prob = classify(start, s_idx);
    s_cand_entry = s_idx;
    s_cand_dwell = 0;
    s_cand_active = true;
    snprintf(s_gesture_detail, sizeof(s_gesture_detail), "p=%.2f;peak=%.0f;dgrav=%.0f", s_cand_prob, peak, dgrav);
}

/* Called every sample while a candidate is active. */
static void candidate_step(bool in_dwell)
{
    uint32_t since = s_idx - s_cand_entry;

    if (since >= 1 && since <= DWELL_SAMPLES && in_dwell) {
        s_cand_dwell++;
    }
    if (since == DWELL_SAMPLES) {
        bool accept = s_cand_gate && s_cand_prob >= gesture_threshold && s_cand_dwell >= DWELL_MIN;
        size_t n = strlen(s_gesture_detail);
        snprintf(s_gesture_detail + n, sizeof(s_gesture_detail) - n, ";dwell=%d/%d;entry=%d",
                 s_cand_dwell, DWELL_SAMPLES, WIN);
        ESP_LOGI(TAG, "mouth pose: %s -> %s", s_gesture_detail, accept ? "GESTURE" : "rejected");
        s_gesture_accepted = accept;
        s_gesture_csv_ready = false;
        s_gesture_pending = true;               /* main loop may open the screen now */
    }
    if (since == POST_SAMPLES) {
        uint32_t start = s_cand_entry - WIN;
        s_gesture_csv_len = snprintf(s_gesture_csv, sizeof(s_gesture_csv), "t_ms,ax,ay,az,gx,gy,gz\n");
        for (uint32_t i = 0; i <= WIN + POST_SAMPLES; i++) {
            const float *r = s_ring[(start + i) % RING];
            int w = snprintf(s_gesture_csv + s_gesture_csv_len, sizeof(s_gesture_csv) - s_gesture_csv_len,
                             "%d,%.3f,%.3f,%.3f,%.1f,%.1f,%.1f\n", (int)i * SAMPLE_MS,
                             r[0], r[1], r[2], r[3], r[4], r[5]);
            if (w <= 0 || s_gesture_csv_len + w >= sizeof(s_gesture_csv)) {
                break;
            }
            s_gesture_csv_len += w;
        }
        s_gesture_csv_ready = true;
        s_cand_active = false;
    }
}

static volatile float s_motion_dps;     /* wrist motion, peak-hold with decay (power: wake from doze) */

float onurb_imu_motion_dps(void)
{
    return s_motion_dps;
}

static void imu_task(void *arg)
{
    qmi8658_data_t d;
    bool moving = false;
    int64_t move_start_ms = 0, quiet_since_ms = 0;
    int prev_region = 0;
    int64_t last_entry_ms = -REFRACTORY_MS;
    float peak_dps = 0, p0 = 0, r0 = 0;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_MS));
        if (qmi8658_read_sensor_data(&s_dev, &d) != ESP_OK) {
            continue;
        }
        float pitch, roll;
        float dps = sqrtf(d.gyroX * d.gyroX + d.gyroY * d.gyroY + d.gyroZ * d.gyroZ);
        s_motion_dps = dps > s_motion_dps * 0.85f ? dps : s_motion_dps * 0.85f;
        int64_t now_ms = (int64_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
        tilt(&d, &pitch, &roll);
        s_strong_run = dps >= END_DPS ? s_strong_run + 1 : 0;
        bool near_mouth = pitch >= END_OUT_PITCH_MIN && pitch <= END_OUT_PITCH_MAX && fabsf(roll) >= END_OUT_ROLL_MIN;
        s_out_run = near_mouth ? 0 : s_out_run + 1;
        s_pitch = pitch;
        s_roll = roll;
        float *slot = s_ring[s_idx % RING];
        slot[0] = d.accelX; slot[1] = d.accelY; slot[2] = d.accelZ;
        slot[3] = d.gyroX;  slot[4] = d.gyroY;  slot[5] = d.gyroZ;

        if (s_capture_left > 0) {
            static int64_t t0_ms;
            if (s_capture_n == 0) {
                t0_ms = now_ms;
                s_csv_len = snprintf(s_csv, sizeof(s_csv), "t_ms,ax,ay,az,gx,gy,gz\n");
            }
            char row[96];
            int n_row = snprintf(row, sizeof(row), "%lld,%.3f,%.3f,%.3f,%.1f,%.1f,%.1f\n",
                                 (long long)(now_ms - t0_ms), d.accelX, d.accelY, d.accelZ,
                                 d.gyroX, d.gyroY, d.gyroZ);
            printf("IMU,%d,%s", s_capture_id, row);
            if (s_csv_len + n_row < sizeof(s_csv)) {
                memcpy(s_csv + s_csv_len, row, n_row + 1);
                s_csv_len += n_row;
            }
            s_capture_n++;
            if (--s_capture_left == 0) {
                printf("IMU_END,%d,%d\n", s_capture_id, s_capture_n);
            }
        }

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

        int region = mouth_region(pitch, roll);
        if (region != 0 && region != prev_region && now_ms - last_entry_ms >= REFRACTORY_MS) {
            last_entry_ms = now_ms;
            on_pose_entry();
        }
        if (s_cand_active) {
            candidate_step(in_dwell_pose(pitch, roll));
        }
        prev_region = region;
        s_idx++;

    }
}

int onurb_imu_capture_start(int ms)
{
    s_capture_n = 0;
    s_capture_id++;
    s_capture_left = ms / SAMPLE_MS;
    return s_capture_id;
}

bool onurb_imu_capture_busy(void)
{
    return s_capture_left > 0;
}

const char *onurb_imu_capture_csv(size_t *len)
{
    *len = s_csv_len;
    return s_csv;
}

bool onurb_imu_gesture_take(bool *accepted, const char **detail)
{
    if (!s_gesture_pending) {
        return false;
    }
    *accepted = s_gesture_accepted;
    *detail = s_gesture_detail;
    return true;
}

bool onurb_imu_gesture_csv(const char **csv, size_t *len)
{
    if (!s_gesture_csv_ready) {
        return false;
    }
    *csv = s_gesture_csv;
    *len = s_gesture_csv_len;
    return true;
}

void onurb_imu_gesture_done(void)
{
    s_gesture_csv_ready = false;
    s_gesture_pending = false;
}

void onurb_imu_get_tilt(float *pitch, float *roll)
{
    *pitch = s_pitch;
    *roll = s_roll;
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

int onurb_imu_end_motion(void)
{
    return s_out_run >= END_OUT_SAMPLES ? ONURB_IMU_END_LEFT_MOUTH
         : s_strong_run >= END_SAMPLES ? ONURB_IMU_END_FLICK : ONURB_IMU_END_NONE;
}
