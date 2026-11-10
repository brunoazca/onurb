#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"

#define ONURB_ACTION_ID_LEN 16          /* X-Onurb-Action ids are 12 hex chars */

typedef enum {
    ONURB_TURN_UPLOADING,
    ONURB_TURN_WAITING,     /* upload done, waiting for the reply (STT + agent + TTS) */
    ONURB_TURN_DOWNLOADING,
} onurb_turn_phase_t;

typedef void (*onurb_turn_phase_cb_t)(onurb_turn_phase_t phase);

/* Threads (one side stick per request on the watch). Optional (NULL) on every call.
 * in:  reply_to = the thread the user is answering by voice ("" = a new request).
 * out: the gateway's X-Onurb-Thread (thread id), X-Onurb-Thread-State
 *      ("done" | "open" = waits for the user | "working" = background task running)
 *      and X-Onurb-Heard (the transcript, UTF-8). */
#define ONURB_HEARD_CAP 320
typedef struct {
    char reply_to[33];
    char thread[33];
    char state[12];
    char heard[ONURB_HEARD_CAP];
    char action[ONURB_ACTION_ID_LEN];  /* X-Onurb-Action on GET (inbox) replies */
} onurb_turn_meta_t;

/* POST mono s16 PCM as WAV to CONFIG_ONURB_API_BASE_URL/v1/turns (SPEC §9.1)
 * and return the reply body in a PSRAM buffer the caller must free().
 * turn_id (filled before the upload starts) is what onurb_turn_cancel() takes;
 * reply_text gets the X-Onurb-Reply-Text (UTF-8). Several turns may run at once.
 * A fresh HTTP client is created per turn, so no socket survives a reconnect. */
esp_err_t onurb_turn_send(const int16_t *pcm, size_t samples, uint32_t sample_rate,
                          onurb_turn_phase_cb_t on_phase, char turn_id[33],
                          char *reply_text, size_t text_cap, char action_id[ONURB_ACTION_ID_LEN],
                          uint8_t **reply, size_t *reply_len, int *http_status, onurb_turn_meta_t *meta);

/* WhatsApp approval (docs/AGENT_B.md). A turn reply may carry a proposed
 * message (action_id non-empty). The watch shows the preview, then:
 *   onurb_action_post(id, "confirm" | "cancel")   -> the gateway sends / drops it
 *   onurb_action_edit(id, audio...)               -> spoken change; the reply may
 *                                                    carry a new action id */
esp_err_t onurb_action_post(const char *action, const char *verb);
esp_err_t onurb_action_edit(const char *action, const int16_t *pcm, size_t samples, uint32_t sample_rate,
                            onurb_turn_phase_cb_t on_phase, char turn_id[33], char *reply_text, size_t text_cap,
                            char new_action[ONURB_ACTION_ID_LEN], uint8_t **reply, size_t *reply_len, int *http_status,
                            onurb_turn_meta_t *meta);

/* GET CONFIG_ONURB_API_BASE_URL + path (<= max_len bytes, PSRAM, NUL-terminated;
 * caller frees). 204 -> ESP_OK with *body NULL. reply_text gets X-Onurb-Reply-Text. */
esp_err_t onurb_http_get(const char *path, size_t max_len, uint8_t **body, size_t *len, int *http_status,
                         char *reply_text, size_t text_cap, onurb_turn_meta_t *meta);

/* Send a turn recorded while offline (SPEC: offline queue), streaming the
 * PCM from src (positioned at the samples) so a 90 s file needs no 4 MB
 * buffer. recorded_at =
 * unix time or 0. The gateway forwards it to the brain and returns no audio;
 * ESP_OK means delivered (HTTP 200). *http_status 0 = gateway unreachable. */
esp_err_t onurb_turn_send_queued_file(FILE *src, size_t samples, uint32_t sample_rate,
                                      int64_t recorded_at, int *http_status);

/* Tell the gateway to drop the live turn in flight (user tapped cancel):
 * not sent to the brain if it hasn't been yet, and no reply either way. */
esp_err_t onurb_turn_cancel(const char *turn_id);


/* POST a gesture match + the user's verdict ("correct"/"wrong"/"timeout")
 * with its raw IMU segment (CSV) to /v1/gesture-feedback. */
esp_err_t onurb_turn_post_gesture_feedback(const char *csv, size_t len, const char *label, const char *detail);

/* POST an IMU capture (CSV) to CONFIG_ONURB_API_BASE_URL/v1/imu-log, labelled
 * "gesture" or "other" by the user (feedback screen after the capture). */
esp_err_t onurb_turn_post_imu(const char *csv, size_t len, int capture_id, const char *label);
