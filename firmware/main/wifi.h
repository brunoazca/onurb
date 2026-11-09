#pragma once

#include "esp_err.h"

/* Start the station with auto-reconnect (SPEC §8.2). Returns
 * ESP_ERR_INVALID_STATE if the SSID is still the placeholder. */
esp_err_t onurb_wifi_start(void);

/* Log connection state and whether SNTP has confirmed internet access. */
void onurb_wifi_log_status(void);

/* Short human-readable state for the screen ("Wi-Fi online", ...). */
const char *onurb_wifi_state_text(void);
