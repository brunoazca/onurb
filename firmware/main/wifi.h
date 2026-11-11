#pragma once

#include "esp_err.h"

/* Start the station with auto-reconnect (SPEC §8.2). Returns
 * ESP_ERR_INVALID_STATE if the SSID is still the placeholder. */
esp_err_t onurb_wifi_start(void);

/* Log connection state and whether SNTP has confirmed internet access. */
void onurb_wifi_log_status(void);

/* Short human-readable state for the screen ("Wi-Fi online", ...). */
const char *onurb_wifi_state_text(void);

/* 0 = not configured / connecting, 1 = connected, 2 = online (SNTP synced).
 * Matches onurb_ui_wifi_t. */
int onurb_wifi_level(void);

/* Power: switch the radio off while the watch dozes / back on (reconnects). */
void onurb_wifi_doze(bool on);
bool onurb_wifi_dozing(void);

/* SSID the station is using or trying ("" if none). */
const char *onurb_wifi_ssid(void);

/* Network picker support. onurb_wifi_scan() blocks for ~2-4 s (call it off
 * the LVGL task); it returns visible networks deduped by SSID, strongest
 * first, hidden ones skipped, or -1 on error. Names are never logged. */
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;
} onurb_wifi_ap_t;
int onurb_wifi_scan(onurb_wifi_ap_t *out, int max);

/* Switch to a new network (password NULL/"" for open). Non-blocking; poll
 * onurb_wifi_join_state(). Saved to NVS only once it gets an IP; after 3
 * failed attempts or 20 s the previous network is restored. The password is
 * never logged. */
enum {
    ONURB_WIFI_JOIN_IDLE = 0,
    ONURB_WIFI_JOIN_PENDING = 1,
    ONURB_WIFI_JOIN_OK = 2,
    ONURB_WIFI_JOIN_FAILED = 3,
};
esp_err_t onurb_wifi_join(const char *ssid, const char *password);
int onurb_wifi_join_state(void);
