#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    ONURB_TURN_UPLOADING,
    ONURB_TURN_WAITING,     /* upload done, waiting for the reply (STT + agent + TTS) */
    ONURB_TURN_DOWNLOADING,
} onurb_turn_phase_t;

typedef void (*onurb_turn_phase_cb_t)(onurb_turn_phase_t phase);

/* POST mono s16 PCM as WAV to CONFIG_ONURB_API_BASE_URL/v1/turns (SPEC §9.1)
 * and return the reply body in a PSRAM buffer the caller must free().
 * A fresh HTTP client is created per turn, so no socket survives a reconnect. */
esp_err_t onurb_turn_send(const int16_t *pcm, size_t samples, uint32_t sample_rate,
                          onurb_turn_phase_cb_t on_phase,
                          uint8_t **reply, size_t *reply_len, int *http_status);
