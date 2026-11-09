/*
 * Wi-Fi station with automatic reconnect (SPEC §8.2):
 * - credentials from Kconfig (placeholders in git; real values only in local sdkconfig)
 * - on disconnect, esp_wifi_connect() is retried from a one-shot timer with
 *   exponential backoff 1, 2, 4 ... 60 s (+/-20 % jitter), never giving up;
 *   backoff resets on IP
 * - first IP starts SNTP, which doubles as an internet reachability check
 */
#include <string.h>
#include <time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "wifi.h"

static const char *TAG = "onurb_wifi";

#define BACKOFF_MIN_MS 1000
#define BACKOFF_MAX_MS 60000

static esp_timer_handle_t s_retry_timer;
static uint32_t s_backoff_ms = BACKOFF_MIN_MS;
static bool s_connected;
static bool s_sntp_started;

static void retry_cb(void *arg)
{
    esp_wifi_connect();
}

static void schedule_retry(void)
{
    /* +/-20 % jitter so a room full of devices doesn't reconnect in lockstep. */
    uint32_t jitter = s_backoff_ms / 5;
    uint32_t delay = s_backoff_ms - jitter + esp_random() % (2 * jitter + 1);

    ESP_LOGI(TAG, "retrying in %lu ms", (unsigned long)delay);
    esp_timer_start_once(s_retry_timer, (uint64_t)delay * 1000);
    s_backoff_ms = s_backoff_ms * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : s_backoff_ms * 2;
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *ev = data;
        s_connected = false;
        ESP_LOGW(TAG, "disconnected (reason %d, rssi %d)", ev->reason, ev->rssi);
        schedule_retry();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        wifi_ap_record_t ap;
        s_connected = true;
        s_backoff_ms = BACKOFF_MIN_MS;
        esp_timer_stop(s_retry_timer);
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            ESP_LOGI(TAG, "connected: ip " IPSTR ", channel %d, rssi %d dBm",
                     IP2STR(&ev->ip_info.ip), ap.primary, ap.rssi);
        }
        if (!s_sntp_started) {
            esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            esp_netif_sntp_init(&cfg);
            s_sntp_started = true;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        s_connected = false;
        ESP_LOGW(TAG, "lost IP");
    }
}

esp_err_t onurb_wifi_start(void)
{
    if (strcmp(CONFIG_ONURB_WIFI_SSID, "CHANGE_ME_SSID") == 0 || CONFIG_ONURB_WIFI_SSID[0] == '\0') {
        ESP_LOGW(TAG, "Wi-Fi SSID not configured (idf.py menuconfig -> Onurb); skipping Wi-Fi");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    const esp_timer_create_args_t timer_args = { .callback = retry_cb, .name = "wifi_retry" };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_retry_timer));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, on_event, NULL));

    wifi_config_t cfg = {
        .sta = {
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = { .capable = true, .required = false },
        },
    };
    strlcpy((char *)cfg.sta.ssid, CONFIG_ONURB_WIFI_SSID, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, CONFIG_ONURB_WIFI_PASSWORD, sizeof(cfg.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));

    /* Never log the password. */
    ESP_LOGI(TAG, "connecting to \"%s\"", CONFIG_ONURB_WIFI_SSID);
    return ESP_OK;
}

void onurb_wifi_log_status(void)
{
    time_t now = time(NULL);
    struct tm tm;

    gmtime_r(&now, &tm);
    if (!s_connected) {
        ESP_LOGI(TAG, "status: not connected");
    } else if (tm.tm_year + 1900 >= 2025) {
        ESP_LOGI(TAG, "status: online, SNTP time %04d-%02d-%02d %02d:%02d:%02d UTC",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    } else {
        ESP_LOGI(TAG, "status: connected, waiting for SNTP (internet not confirmed yet)");
    }
}
