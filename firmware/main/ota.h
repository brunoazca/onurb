#pragma once

#include <stdbool.h>

/* Wi-Fi firmware updates from the gateway (GET /v1/firmware/info + /image).
 *
 * A new image boots "pending verify": onurb_ota_tick() confirms it once Wi-Fi
 * is up, or rolls back to the previous slot if it isn't within 90 s. A crash
 * before confirmation also rolls back (bootloader). */
void onurb_ota_boot_info(void);

/* Reported to the gateway with each update check (verifiable off USB). */
void onurb_ota_report_record_limit(int seconds, unsigned psram_free_kb);

/* Main sets this for the whole turn (recording, processing, reply). A
 * background download pauses meanwhile and the restart waits for idle. */
void onurb_ota_set_busy(bool busy);

/* Call from the idle loop. Confirms a pending image, and checks the gateway
 * for a different build at boot and every CONFIG_ONURB_OTA_CHECK_MIN minutes.
 * Downloads in a background task; reboots only from here, when idle.
 * Returns true if it showed something on screen (caller may refresh). */
bool onurb_ota_tick(void);

/* Battery / doze telemetry sent with each update check (gateway battery log). */
void onurb_ota_set_power_status(const char *s);
/* Check (and report status) as soon as Wi-Fi is up, e.g. after a doze. */
void onurb_ota_check_soon(void);
bool onurb_ota_busy_downloading(void);
