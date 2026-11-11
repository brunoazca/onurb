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
#define MAX_KNOWN 6               /* networks remembered (most recently used first) */
#define RESCAN_MS 30000           /* no known network in range: look again, */
#define RESCAN_MAX_MS 300000      /* doubling up to 5 min (each scan costs radio time) */
static uint32_t s_rescan_ms = RESCAN_MS;

ESP_EVENT_DEFINE_BASE(ONURB_WIFI_EVENT);
enum { EV_JOIN, EV_JOIN_DEADLINE, EV_AUTOSCAN };

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

/* ---- Known networks (Bruno, 2026-11-10) --------------------------------
 * Every network that got an IP is remembered in NVS (up to MAX_KNOWN, most
 * recent first; the old single "ssid"/"pass" entry is imported). When the
 * current network keeps failing, the watch scans and joins the strongest
 * known network actually in range (the Kconfig one counts as known too);
 * if none is in range it looks again every RESCAN_MS instead of hammering
 * a network that isn't there. */
static bool s_dozing_flag(void);
static creds_t s_known[MAX_KNOWN];
static int s_nknown;
static esp_timer_handle_t s_rescan_timer;
static bool s_auto_scanning;

static void save_known(void)
{
    nvs_handle_t h;
    char k[12];
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs open failed; networks not saved");
        return;
    }
    nvs_set_u8(h, "n", (uint8_t)s_nknown);
    for (int i = 0; i < s_nknown; i++) {
        snprintf(k, sizeof(k), "s%d", i);
        nvs_set_str(h, k, s_known[i].ssid);
        snprintf(k, sizeof(k), "p%d", i);
        nvs_set_str(h, k, s_known[i].pass);
    }
    if (nvs_commit(h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs write failed; networks not saved");
    }
    nvs_close(h);
}

static void load_known(void)
{
    nvs_handle_t h;
    uint8_t n = 0;
    char k[12];
    s_nknown = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, "n", &n) != ESP_OK) {
            n = 0;
        }
        for (int i = 0; i < n && s_nknown < MAX_KNOWN; i++) {
            creds_t c = {0};
            size_t n1 = sizeof(c.ssid), n2 = sizeof(c.pass);
            snprintf(k, sizeof(k), "s%d", i);
            bool ok = nvs_get_str(h, k, c.ssid, &n1) == ESP_OK;
            snprintf(k, sizeof(k), "p%d", i);
            ok = ok && nvs_get_str(h, k, c.pass, &n2) == ESP_OK && c.ssid[0];
            if (ok) {
                s_known[s_nknown++] = c;
            }
        }
        nvs_close(h);
    }
    creds_t legacy = {0};
    if (s_nknown == 0 && load_nvs(&legacy)) {       /* single network saved by older firmware */
        s_known[s_nknown++] = legacy;
        save_known();
    }
    memset(&legacy, 0, sizeof(legacy));
    ESP_LOGI(TAG, "%d known network(s)", s_nknown);
}

/* Move `c` to the front of the known list (new or existing) and persist it. */
static void remember(const creds_t *c)
{
    int i;
    for (i = 0; i < s_nknown && strcmp(s_known[i].ssid, c->ssid) != 0; i++) {
    }
    if (i == 0 && s_nknown > 0 && strcmp(s_known[0].pass, c->pass) == 0) {
        return;                                      /* already first, unchanged: no flash write */
    }
    if (i == s_nknown) {
        i = s_nknown < MAX_KNOWN ? s_nknown++ : MAX_KNOWN - 1;   /* full: forget the oldest */
    }
    memmove(&s_known[1], &s_known[0], (size_t)i * sizeof(creds_t));
    s_known[0] = *c;
    save_known();
}

static void rescan_cb(void *arg)
{
    esp_event_post(ONURB_WIFI_EVENT, EV_AUTOSCAN, NULL, 0, 0);
}

static void auto_scan(void)
{
    if (s_scanning || s_dozing_flag()) {
        return;
    }
    esp_timer_stop(s_retry_timer);
    esp_timer_stop(s_rescan_timer);
    s_auto_scanning = s_scanning = true;
    if (!s_connected) {
        esp_wifi_disconnect();                       /* a pending connect attempt blocks scanning */
    }
    if (esp_wifi_scan_start(NULL, false) != ESP_OK) {
        s_auto_scanning = s_scanning = false;
        esp_timer_start_once(s_rescan_timer, (uint64_t)RESCAN_MS * 1000);
    }
}

/* Scan done: join the strongest known network in range. */
static void auto_scan_done(void)
{
    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    if (num > 24) {
        num = 24;
    }
    wifi_ap_record_t *recs = num ? calloc(num, sizeof(*recs)) : NULL;
    const creds_t *best = NULL;
    int best_rssi = -1000, best_src = SRC_NONE;
    if (recs && esp_wifi_scan_get_ap_records(&num, recs) == ESP_OK) {
        for (int i = 0; i < num; i++) {
            const char *ssid = (const char *)recs[i].ssid;
            for (int k = 0; k <= s_nknown; k++) {
                const creds_t *c = k < s_nknown ? &s_known[k] : (s_have_kcfg ? &s_kcfg : NULL);
                if (c && strcmp(c->ssid, ssid) == 0 && recs[i].rssi > best_rssi) {
                    best = c;
                    best_rssi = recs[i].rssi;
                    best_src = k < s_nknown ? SRC_SAVED : SRC_KCFG;
                }
            }
        }
    } else {
        esp_wifi_clear_ap_list();
    }
    free(recs);
    s_auto_scanning = s_scanning = false;
    if (best) {
        ESP_LOGI(TAG, "best known network in range: \"%s\" (%d dBm)", best->ssid, best_rssi);
        apply(best, best_src);
    } else {
        ESP_LOGW(TAG, "no known network in range (%d seen); looking again in %u s", num, (unsigned)(s_rescan_ms / 1000));
        esp_timer_start_once(s_rescan_timer, (uint64_t)s_rescan_ms * 1000);
        s_rescan_ms = s_rescan_ms * 2 > RESCAN_MAX_MS ? RESCAN_MAX_MS : s_rescan_ms * 2;
    }
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
    } else if (++s_fail_streak >= FALLBACK_AFTER) {
        ESP_LOGW(TAG, "\"%s\" unreachable; scanning for a known network", s_cur.ssid);
        auto_scan();
        return;
    }
    schedule_retry();
}

static volatile bool s_dozing;            /* radio off on purpose (onurb_wifi_doze) */

static bool s_dozing_flag(void)
{
    return s_dozing;
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        if (s_auto_scanning) {
            auto_scan_done();
        }
        return;
    }
    if (base == ONURB_WIFI_EVENT && id == EV_AUTOSCAN) {
        if (!s_connected && s_src != SRC_TRIAL) {
            auto_scan();
        }
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_src != SRC_NONE) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *ev = data;
        s_connected = false;
        if (s_dozing) {
            return;                       /* radio being switched off for doze */
        }
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
        s_rescan_ms = RESCAN_MS;
        esp_timer_stop(s_retry_timer);
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            ESP_LOGI(TAG, "connected: ip " IPSTR ", channel %d, rssi %d dBm",
                     IP2STR(&ev->ip_info.ip), ap.primary, ap.rssi);
        }
        esp_timer_stop(s_rescan_timer);
        if (s_src != SRC_KCFG || s_nknown > 0) {
            remember(&s_cur);                        /* most recently used network first */
        }
        if (s_src == SRC_TRIAL) {
            esp_timer_stop(s_join_timer);
            s_saved = s_cur;
            s_have_saved = true;
            s_src = SRC_SAVED;
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
    load_known();
    s_have_saved = s_nknown > 0;
    if (s_have_saved) {
        s_saved = s_known[0];
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    const esp_timer_create_args_t timer_args = { .callback = retry_cb, .name = "wifi_retry" };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_retry_timer));
    const esp_timer_create_args_t join_args = { .callback = join_deadline_cb, .name = "wifi_join" };
    ESP_ERROR_CHECK(esp_timer_create(&join_args, &s_join_timer));
    const esp_timer_create_args_t rescan_args = { .callback = rescan_cb, .name = "wifi_rescan" };
    ESP_ERROR_CHECK(esp_timer_create(&rescan_args, &s_rescan_timer));

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

/* Doze (power): on = radio off (no reconnect attempts); off = radio back on,
 * reconnecting to the current network (STA_START -> connect, ~1-2 s). */
void onurb_wifi_doze(bool on)
{
    if (on == s_dozing || s_retry_timer == NULL || s_scanning || s_src == SRC_TRIAL) {
        return;
    }
    if (on) {
        s_dozing = true;
        esp_timer_stop(s_retry_timer);
        s_connected = false;
        esp_wifi_stop();
        ESP_LOGI(TAG, "doze: radio off");
    } else {
        s_dozing = false;
        s_backoff_ms = BACKOFF_MIN_MS;
        s_fail_streak = 0;
        esp_wifi_start();                 /* STA_START reconnects */
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        ESP_LOGI(TAG, "doze: radio on");
    }
}

bool onurb_wifi_dozing(void)
{
    return s_dozing;
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
