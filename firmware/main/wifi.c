/*
 * Wi-Fi station with automatic reconnect (SPEC §8.2):
 * - credentials: a network picked on the watch (saved in NVS) or the Kconfig
 *   one (placeholders in git; real values only in local sdkconfig). With both
 *   present, the saved one is tried first and they alternate after
 *   FALLBACK_AFTER failed attempts, so a bad saved network can't strand us.
 * - on disconnect, esp_wifi_connect() is retried from a one-shot timer with
 *   exponential backoff 1, 2, 4 ... 60 s (+/-20 % jitter), never giving up;
 *   backoff resets on IP
 * - onurb_wifi_join() tries a new network; it is saved to NVS only once it
 *   gets an IP, otherwise the previous network is restored
 * - first IP starts SNTP, which doubles as an internet reachability check
 * - passwords are never logged
 *
 * All connection-state changes run on the default event loop task (joins and
 * the join deadline are posted there as ONURB_WIFI_EVENT), so there is no
 * locking between the UI, the timers and the Wi-Fi driver events.
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "wifi.h"

static const char *TAG = "onurb_wifi";

#define BACKOFF_MIN_MS 1000
#define BACKOFF_MAX_MS 60000
#define FALLBACK_AFTER 4          /* failed attempts before trying the other network */
#define JOIN_MAX_FAILS 3          /* failed attempts before a join is abandoned */
#define JOIN_DEADLINE_MS 20000
#define NVS_NS "onurb_wifi"

ESP_EVENT_DEFINE_BASE(ONURB_WIFI_EVENT);
enum { EV_JOIN, EV_JOIN_DEADLINE };

typedef struct {
    char ssid[33];
    char pass[65];
} creds_t;

enum { SRC_NONE, SRC_SAVED, SRC_KCFG, SRC_TRIAL };

static esp_timer_handle_t s_retry_timer;
static esp_timer_handle_t s_join_timer;
static uint32_t s_backoff_ms = BACKOFF_MIN_MS;
static volatile bool s_connected;
static bool s_sntp_started;
static volatile bool s_scanning;
static bool s_ignore_leave;       /* our own esp_wifi_disconnect(): don't retry */
static int s_fail_streak;

static creds_t s_saved, s_kcfg, s_cur, s_prev;
static bool s_have_saved, s_have_kcfg;
static int s_src = SRC_NONE, s_prev_src = SRC_NONE;
static volatile int s_join_state = ONURB_WIFI_JOIN_IDLE;
static int s_join_fails;
static char s_ssid_shown[33];     /* copy for onurb_wifi_ssid(), read by the UI */

static void retry_cb(void *arg)
{
    if (!s_scanning) {
        esp_wifi_connect();
    }
}

static void join_deadline_cb(void *arg)
{
    esp_event_post(ONURB_WIFI_EVENT, EV_JOIN_DEADLINE, NULL, 0, 0);
}

static void schedule_retry(void)
{
    /* +/-20 % jitter so a room full of devices doesn't reconnect in lockstep. */
    uint32_t jitter = s_backoff_ms / 5;
    uint32_t delay = s_backoff_ms - jitter + esp_random() % (2 * jitter + 1);

    ESP_LOGI(TAG, "retrying in %lu ms", (unsigned long)delay);
    esp_timer_stop(s_retry_timer);
    esp_timer_start_once(s_retry_timer, (uint64_t)delay * 1000);
    s_backoff_ms = s_backoff_ms * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : s_backoff_ms * 2;
}

/* Point the station at `c` and start connecting (drops any current link). */
static void apply(const creds_t *c, int src)
{
    wifi_config_t cfg = {
        .sta = {
            .threshold.authmode = c->pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN,
            .pmf_cfg = { .capable = true, .required = false },
        },
    };

    if (&s_cur != c) {
        s_cur = *c;
    }
    s_src = src;
    strlcpy(s_ssid_shown, s_cur.ssid, sizeof(s_ssid_shown));
    strlcpy((char *)cfg.sta.ssid, c->ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, c->pass, sizeof(cfg.sta.password));

    esp_timer_stop(s_retry_timer);
    s_backoff_ms = BACKOFF_MIN_MS;
    s_fail_streak = 0;
    if (s_connected) {
        s_ignore_leave = true;
    }
    esp_wifi_disconnect();
    s_connected = false;
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    memset(&cfg, 0, sizeof(cfg));
    esp_wifi_connect();
    /* Never log the password. */
    ESP_LOGI(TAG, "connecting to \"%s\" (%s)", s_cur.ssid,
             src == SRC_SAVED ? "saved" : src == SRC_KCFG ? "default" : "new");
}

static void save_nvs(const creds_t *c)
{
    nvs_handle_t h;

    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs open failed; network not saved");
        return;
    }
    if (nvs_set_str(h, "ssid", c->ssid) != ESP_OK || nvs_set_str(h, "pass", c->pass) != ESP_OK ||
        nvs_commit(h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs write failed; network not saved");
    }
    nvs_close(h);
}

static bool load_nvs(creds_t *c)
{
    nvs_handle_t h;
    size_t n1 = sizeof(c->ssid), n2 = sizeof(c->pass);
    bool ok;

    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    ok = nvs_get_str(h, "ssid", c->ssid, &n1) == ESP_OK && nvs_get_str(h, "pass", c->pass, &n2) == ESP_OK &&
         c->ssid[0] != '\0';
    nvs_close(h);
    return ok;
}

static void join_failed(const char *why)
{
    esp_timer_stop(s_join_timer);
    ESP_LOGW(TAG, "join \"%s\" failed (%s)", s_cur.ssid, why);
    memset(s_cur.pass, 0, sizeof(s_cur.pass));
    s_join_state = ONURB_WIFI_JOIN_FAILED;
    if (s_prev_src != SRC_NONE) {
        apply(&s_prev, s_prev_src);
    } else {
        s_src = SRC_NONE;
        s_ssid_shown[0] = '\0';
        esp_timer_stop(s_retry_timer);
        esp_wifi_disconnect();
    }
}

static void on_attempt_failed(void)
{
    if (s_src == SRC_TRIAL) {
        if (++s_join_fails >= JOIN_MAX_FAILS) {
            join_failed("no connection");
            return;
        }
    } else if (++s_fail_streak >= FALLBACK_AFTER && s_have_saved && s_have_kcfg) {
        if (s_src == SRC_SAVED) {
            ESP_LOGW(TAG, "saved network unreachable; trying the default one");
            apply(&s_kcfg, SRC_KCFG);
        } else {
            ESP_LOGW(TAG, "default network unreachable; trying the saved one");
            apply(&s_saved, SRC_SAVED);
        }
        return;
    }
    schedule_retry();
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_src != SRC_NONE) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *ev = data;
        s_connected = false;
        ESP_LOGW(TAG, "disconnected (reason %d, rssi %d)", ev->reason, ev->rssi);
        if (ev->reason == WIFI_REASON_ASSOC_LEAVE && s_ignore_leave) {
            s_ignore_leave = false; /* we dropped the link on purpose */
            return;
        }
        if (s_scanning || s_src == SRC_NONE) {
            return; /* the scan reconnects when it's done */
        }
        on_attempt_failed();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        wifi_ap_record_t ap;
        s_connected = true;
        s_ignore_leave = false;
        s_backoff_ms = BACKOFF_MIN_MS;
        s_fail_streak = 0;
        esp_timer_stop(s_retry_timer);
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            ESP_LOGI(TAG, "connected: ip " IPSTR ", channel %d, rssi %d dBm",
                     IP2STR(&ev->ip_info.ip), ap.primary, ap.rssi);
        }
        if (s_src == SRC_TRIAL) {
            esp_timer_stop(s_join_timer);
            s_saved = s_cur;
            s_have_saved = true;
            s_src = SRC_SAVED;
            save_nvs(&s_saved);
            s_join_state = ONURB_WIFI_JOIN_OK;
            ESP_LOGI(TAG, "saved \"%s\" as the preferred network", s_saved.ssid);
        }
        if (!s_sntp_started) {
            esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            esp_netif_sntp_init(&cfg);
            s_sntp_started = true;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        s_connected = false;
        ESP_LOGW(TAG, "lost IP");
    } else if (base == ONURB_WIFI_EVENT && id == EV_JOIN) {
        creds_t *c = data;
        if (s_src != SRC_TRIAL) {
            s_prev = s_cur;
            s_prev_src = s_src;
        }
        s_join_fails = 0;
        apply(c, SRC_TRIAL);
        memset(c, 0, sizeof(*c)); /* the event loop's copy of the password */
        esp_timer_stop(s_join_timer);
        esp_timer_start_once(s_join_timer, (uint64_t)JOIN_DEADLINE_MS * 1000);
    } else if (base == ONURB_WIFI_EVENT && id == EV_JOIN_DEADLINE) {
        if (s_src == SRC_TRIAL && s_join_state == ONURB_WIFI_JOIN_PENDING) {
            join_failed("timeout");
        }
    }
}

esp_err_t onurb_wifi_start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    s_have_kcfg = CONFIG_ONURB_WIFI_SSID[0] != '\0' && strcmp(CONFIG_ONURB_WIFI_SSID, "CHANGE_ME_SSID") != 0;
    if (s_have_kcfg) {
        strlcpy(s_kcfg.ssid, CONFIG_ONURB_WIFI_SSID, sizeof(s_kcfg.ssid));
        strlcpy(s_kcfg.pass, CONFIG_ONURB_WIFI_PASSWORD, sizeof(s_kcfg.pass));
    }
    s_have_saved = load_nvs(&s_saved);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    const esp_timer_create_args_t timer_args = { .callback = retry_cb, .name = "wifi_retry" };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_retry_timer));
    const esp_timer_create_args_t join_args = { .callback = join_deadline_cb, .name = "wifi_join" };
    ESP_ERROR_CHECK(esp_timer_create(&join_args, &s_join_timer));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(ONURB_WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));

    if (s_have_saved) {
        apply(&s_saved, SRC_SAVED);
    } else if (s_have_kcfg) {
        apply(&s_kcfg, SRC_KCFG);
    } else {
        ESP_LOGW(TAG, "no Wi-Fi network configured (menuconfig -> Onurb, or pick one on the watch)");
        return ESP_ERR_INVALID_STATE;
    }
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

const char *onurb_wifi_state_text(void)
{
    if (s_retry_timer == NULL || s_src == SRC_NONE) {
        return "Wi-Fi not configured";
    }
    if (!s_connected) {
        return "Wi-Fi connecting...";
    }
    return time(NULL) > 1735689600 /* 2025-01-01 */ ? "Wi-Fi online" : "Wi-Fi connected";
}

int onurb_wifi_level(void)
{
    if (s_retry_timer == NULL || !s_connected) {
        return 0;
    }
    return time(NULL) > 1735689600 /* 2025-01-01 */ ? 2 : 1;
}

const char *onurb_wifi_ssid(void)
{
    return s_ssid_shown;
}

static int cmp_rssi(const void *a, const void *b)
{
    return ((const onurb_wifi_ap_t *)b)->rssi - ((const onurb_wifi_ap_t *)a)->rssi;
}

int onurb_wifi_scan(onurb_wifi_ap_t *out, int max)
{
    uint16_t num = 0;
    wifi_ap_record_t *recs;
    int n = 0;

    if (s_retry_timer == NULL || max <= 0 || s_join_state == ONURB_WIFI_JOIN_PENDING) {
        return -1;
    }
    s_scanning = true;
    esp_timer_stop(s_retry_timer);
    if (!s_connected) {
        esp_wifi_disconnect(); /* a pending connect attempt blocks scanning */
    }
    esp_err_t err = esp_wifi_scan_start(NULL, true);
    if (err == ESP_OK) {
        esp_wifi_scan_get_ap_num(&num);
    }
    if (num > 24) {
        num = 24;
    }
    recs = num ? calloc(num, sizeof(*recs)) : NULL;
    if (recs != NULL && esp_wifi_scan_get_ap_records(&num, recs) == ESP_OK) {
        for (int i = 0; i < num; i++) {
            const char *ssid = (const char *)recs[i].ssid;
            int j;
            if (ssid[0] == '\0') {
                continue; /* hidden network */
            }
            for (j = 0; j < n && strcmp(out[j].ssid, ssid) != 0; j++) {
            }
            if (j < n) {
                if (recs[i].rssi > out[j].rssi) {
                    out[j].rssi = recs[i].rssi;
                }
                continue;
            }
            if (n < max) {
                strlcpy(out[n].ssid, ssid, sizeof(out[n].ssid));
                out[n].rssi = recs[i].rssi;
                out[n].secure = recs[i].authmode != WIFI_AUTH_OPEN && recs[i].authmode != WIFI_AUTH_OWE;
                n++;
            }
        }
    } else {
        esp_wifi_clear_ap_list();
    }
    free(recs);
    s_scanning = false;
    if (!s_connected && s_src != SRC_NONE) {
        esp_wifi_connect();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
        return -1;
    }
    qsort(out, n, sizeof(*out), cmp_rssi);
    ESP_LOGI(TAG, "scan: %d networks", n); /* names not logged */
    return n;
}

esp_err_t onurb_wifi_join(const char *ssid, const char *password)
{
    creds_t c = { 0 };

    if (s_retry_timer == NULL || ssid == NULL || ssid[0] == '\0' || strlen(ssid) >= sizeof(c.ssid) ||
        (password != NULL && strlen(password) >= sizeof(c.pass))) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(c.ssid, ssid, sizeof(c.ssid));
    if (password != NULL) {
        strlcpy(c.pass, password, sizeof(c.pass));
    }
    s_join_state = ONURB_WIFI_JOIN_PENDING;
    esp_err_t err = esp_event_post(ONURB_WIFI_EVENT, EV_JOIN, &c, sizeof(c), pdMS_TO_TICKS(100));
    memset(&c, 0, sizeof(c));
    if (err != ESP_OK) {
        s_join_state = ONURB_WIFI_JOIN_FAILED;
    }
    return err;
}

int onurb_wifi_join_state(void)
{
    return s_join_state;
}
