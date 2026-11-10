#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* Offline queue: turns recorded without a connection are stored in the
 * "spool" flash partition (SPIFFS, ~6.5 MB: one full 90 s turn, or many
 * short ones) and sent,
 * oldest first, by a background task once the gateway is reachable again.
 * Their replies are not played or shown (they arrive on the phone). */
esp_err_t onurb_spool_start(void);

/* Save mono s16 PCM for later. ESP_ERR_NO_MEM if the queue is full. */
esp_err_t onurb_spool_save(const int16_t *pcm, size_t samples, uint32_t sample_rate);

/* Turns waiting to be sent. */
int onurb_spool_count(void);

/* True while a queued turn is being sent (a reboot then could resend it). */
bool onurb_spool_busy(void);
