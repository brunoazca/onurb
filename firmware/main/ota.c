#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ota.h"
#include "api.h"
#include "screen.h"
#include "spool.h"
#include "ui.h"
#include "wifi.h"

static const char *TAG = "onurb_ota";

#define CONFIRM_TIMEOUT_US (90LL * 1000000)

static bool s_pending_verify;
static int64_t s_next_check_us;           /* 0 = check as soon as Wi-Fi is up */
static char s_running_hex[65];
static char s_power_status[160];           /* "batt=..;mv=..;chg=..;doze=.." (power telemetry) */
static char s_device_status[64];          /* "rec_limit_s=..;psram_free_kb=.." for the gateway log */

/* Checks cost radio time (battery): hourly (CONFIG_ONURB_OTA_CHECK_MIN), plus one when
 * the user wakes the watch / opens the screen, at most every CHECK_SOON_MIN_S. */
#define CHECK_SOON_MIN_S (15 * 60)
static int64_t s_last_check_us;

void onurb_ota_check_soon(void)
{
    if (s_last_check_us == 0 || esp_timer_get_time() - s_last_check_us >= (int64_t)CHECK_SOON_MIN_S * 1000000) {
        s_next_check_us = 0;
    }
}

void onurb_ota_set_power_status(const char *s)
{
    strlcpy(s_power_status, s, sizeof(s_power_status));
}

void onurb_ota_report_record_limit(int seconds, unsigned psram_free_kb)
{
    snprintf(s_device_status, sizeof(s_device_status), "rec_limit_s=%d;psram_free_kb=%u", seconds, psram_free_kb);
}

static void hex(const uint8_t *b, int n, char *out)
{
    for (int i = 0; i < n; i++) {
        sprintf(out + 2 * i, "%02x", b[i]);
    }
}

void onurb_ota_boot_info(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    uint8_t sha[32];

    if (esp_partition_get_sha256(running, sha) == ESP_OK) {
        hex(sha, 32, s_running_hex);
    }
    s_pending_verify = esp_ota_get_state_partition(running, &state) == ESP_OK &&
                       state == ESP_OTA_IMG_PENDING_VERIFY;
    ESP_LOGI(TAG, "running %s (%s, built %s %s), image %.12s%s", running->label,
             esp_app_get_description()->version, esp_app_get_description()->date,
             esp_app_get_description()->time, s_running_hex,
             s_pending_verify ? " - NEW, pending verify" : "");
}

static const char *s_ota_base;            /* base the current image download uses */

static esp_err_t set_auth(esp_http_client_handle_t client)
{
    onurb_api_headers(client, s_ota_base ? s_ota_base : onurb_api_base());
    return ESP_OK;
}

/* GET /v1/firmware/info -> {"digest": "<64 hex>", "size": N}; false if none/unreachable. */
static bool fetch_info(char digest[65])
{
    char url[160], body[256];
    const char *base = onurb_api_base();
    s_ota_base = base;
    snprintf(url, sizeof(url), "%s/v1/firmware/info", base);
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 5000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) {
        return false;
    }
    set_auth(c);
    esp_http_client_set_header(c, "X-Onurb-Running", s_running_hex);   /* lets the gateway log what's booted */
    /* + why the chip last reset and uptime: reboots/crashes become visible off USB */
    static const char *const reasons[] = {"unknown", "poweron", "ext", "sw", "panic", "int_wdt", "task_wdt",
                                          "wdt", "deepsleep", "brownout", "sdio", "usb", "jtag", "efuse",
                                          "pwr_glitch", "cpu_lockup"};
    int r = (int)esp_reset_reason();
    static char status[288];   /* may run on the main task (small stack) */
    snprintf(status, sizeof(status), "%s;%s;reset=%s;up=%llds", s_device_status, s_power_status,
             r >= 0 && r < (int)(sizeof(reasons) / sizeof(reasons[0])) ? reasons[r] : "other",
             (long long)(esp_timer_get_time() / 1000000));
    esp_http_client_set_header(c, "X-Onurb-Status", status);
    bool ok = false;
    bool opened = esp_http_client_open(c, 0) == ESP_OK;
    if (!opened) {
        onurb_api_failed(base);            /* the next check tries the other gateway */
    }
    if (opened && esp_http_client_fetch_headers(c) >= 0 &&
        esp_http_client_get_status_code(c) == 200) {
        int n = esp_http_client_read(c, body, sizeof(body) - 1);
        if (n > 0) {
            body[n] = 0;
            const char *d = strstr(body, "\"digest\"");
            d = d ? strchr(d + 8, '"') : NULL;
            if (d && strspn(d + 1, "0123456789abcdef") == 64) {
                memcpy(digest, d + 1, 64);
                digest[64] = 0;
                ok = true;
            }
        }
    }
    esp_http_client_cleanup(c);
    return ok;
}

/* Background update (Bruno: never interrupt a recording). The download runs
 * in its own task and pauses between chunks while the user is in a turn
 * (onurb_ota_set_busy): flash writes stall the cache and could glitch the mic
 * capture. The new image is only booted from the idle loop, with no turn and
 * no queued send in flight. */
static volatile bool s_busy;               /* recording / processing / reply on screen */
static volatile bool s_task_running;
static volatile bool s_ready_to_reboot;
static char s_offered_hex[65];
static volatile bool s_retry_soon;

void onurb_ota_set_busy(bool busy)
{
    s_busy = busy;
}

static void ui_progress(int pct)
{
#if ONURB_UI_HAS_UPDATING
    onurb_ui_show_updating(pct);
#else
    onurb_ui_set_status(pct >= 100 ? "Updated - restarting" : "Updating...", ONURB_UI_COLOR_BUSY);
#endif
}

static void ota_task(void *arg)
{
    /* The check runs here, not on the main task: away from home it is HTTPS and
     * the TLS handshake overflowed the main task's stack (reboots every check). */
    char digest[65];
    if (!fetch_info(digest)) {
        ESP_LOGD(TAG, "no firmware offered");
        s_task_running = false;
        vTaskDelete(NULL);
        return;
    }
    if (strcmp(digest, s_running_hex) == 0) {
        ESP_LOGI(TAG, "firmware up to date (%.12s)", digest);
        s_task_running = false;
        vTaskDelete(NULL);
        return;
    }
    memcpy(s_offered_hex, digest, sizeof(s_offered_hex));

    char url[160];
    s_ota_base = onurb_api_base();
    snprintf(url, sizeof(url), "%s/v1/firmware/image", s_ota_base);
    esp_http_client_config_t http = {
        .url = url,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t cfg = {
        .http_config = &http,
        .http_client_init_cb = set_auth,
    };
    bool shown = false;                    /* our progress screen is up (we hold the panel) */

    ESP_LOGI(TAG, "background update %.12s -> %.12s", s_running_hex, s_offered_hex);
    int64_t t0 = esp_timer_get_time();
    esp_https_ota_handle_t ota = NULL;
    esp_err_t err = esp_https_ota_begin(&cfg, &ota);
    if (err == ESP_OK) {
        int total = esp_https_ota_get_image_size(ota), last_pct = -2;
        while (true) {
            if (s_busy) {
                /* The user started a turn. Don't pause with the connection open: unread
                 * image data then fills the Wi-Fi RX buffers and the turn upload crawls
                 * (observed: 5 min uploads, "Busy - try again"). Drop it and redo the
                 * ~25 s download once the watch is idle again. */
                err = ESP_ERR_INVALID_STATE;
                ESP_LOGI(TAG, "turn started - download dropped, retry when idle");
                break;
            }
            if (!shown && onurb_screen_is_on()) {   /* background update: don't light a dark screen */
                shown = true;
                last_pct = -2;
                onurb_screen_hold();
            }
            err = esp_https_ota_perform(ota);
            if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
                break;
            }
            int pct = total > 0 ? (int)(100LL * esp_https_ota_get_image_len_read(ota) / total) : -1;
            if (shown && pct / 2 != last_pct / 2 && pct < 100 && !s_busy) {   /* screen every 2 % */
                last_pct = pct;
                ui_progress(pct);
            }
        }
        if (err == ESP_OK && !esp_https_ota_is_complete_data_received(ota)) {
            err = ESP_ERR_INVALID_SIZE;
        }
        if (err == ESP_OK) {
            err = esp_https_ota_finish(ota);   /* verifies the image hash, sets boot slot */
        } else {
            esp_https_ota_abort(ota);
        }
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "update written in %.1f s - restarting when idle", (esp_timer_get_time() - t0) / 1e6);
        s_ready_to_reboot = true;          /* onurb_ota_tick() restarts from the idle loop */
    } else {
        ESP_LOGE(TAG, "update failed: %s - retry soon", esp_err_to_name(err));
        if (shown && !s_busy) {
            onurb_ui_set_state(ONURB_UI_IDLE, 0);
        }
        s_retry_soon = true;               /* onurb_ota_tick retries 60 s after idle */
    }
    if (shown) {
        onurb_screen_release();
    }
    s_task_running = false;
    vTaskDelete(NULL);
}

bool onurb_ota_tick(void)
{
    int64_t now = esp_timer_get_time();
    bool online = onurb_wifi_level() >= 1;

    if (s_pending_verify) {
        if (online) {
            esp_ota_mark_app_valid_cancel_rollback();
            s_pending_verify = false;
            ESP_LOGI(TAG, "new image confirmed (Wi-Fi up)");
        } else if (now > CONFIRM_TIMEOUT_US) {
            ESP_LOGE(TAG, "new image never reached Wi-Fi - rolling back");
            esp_ota_mark_app_invalid_rollback_and_reboot();
        }
        return false;
    }
    if (s_ready_to_reboot) {
        if (s_busy || onurb_spool_busy()) {   /* never cut a turn or a queued send */
            return false;
        }
        ESP_LOGI(TAG, "idle - restarting into %.12s", s_offered_hex);
        if (onurb_screen_is_on()) {
            onurb_screen_hold();
            ui_progress(100);
            vTaskDelay(pdMS_TO_TICKS(800));
        }
        esp_restart();
    }
    static int64_t idle_since_us;
    if (s_busy) {
        idle_since_us = now;
    }
    if (s_retry_soon && !s_task_running && !s_busy && now - idle_since_us >= 60LL * 1000000) {
        s_retry_soon = false;
        s_next_check_us = 0;                   /* dropped/failed download: try again now */
    }
    static int64_t busy_since_us;
    if (!s_busy) {
        busy_since_us = 0;
    } else if (busy_since_us == 0) {
        busy_since_us = now;
    }
    /* "busy" that lasts 15 min is a stuck state, not a turn: check anyway, so a fix can arrive. */
    bool stuck = s_busy && busy_since_us && now - busy_since_us > 15LL * 60 * 1000000;
    if (s_task_running || !online || now < s_next_check_us || (s_busy && !stuck)) {
        return false;                          /* never start a download during a turn */
    }
    if (stuck) {
        ESP_LOGW(TAG, "busy for 15+ min - checking for an update anyway");
    }
    s_next_check_us = now + (int64_t)CONFIG_ONURB_OTA_CHECK_MIN * 60 * 1000000;
    s_last_check_us = now;

    s_task_running = true;                     /* check + download in their own task */
    if (xTaskCreatePinnedToCore(ota_task, "ota", 8192, NULL, 2, NULL, 0) != pdPASS) {
        s_task_running = false;
        ESP_LOGE(TAG, "no memory for the update task");
    }
    return false;
}

bool onurb_ota_busy_downloading(void)
{
    return s_task_running;
}
