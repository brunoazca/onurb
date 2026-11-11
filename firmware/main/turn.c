#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_wifi.h"

#include "imu.h"
#include "turn.h"
#include "api.h"
#include "wav.h"

static const char *TAG = "onurb_turn";

#define MAX_REPLY_BYTES (2 * 1024 * 1024)
#define CHUNK_BYTES     4096

typedef struct {                       /* per-turn response headers (turns run concurrently) */
    char *buf;                         /* X-Onurb-Reply-Text, percent-decoded */
    size_t cap;
    char *action;                      /* X-Onurb-Action: proposed WhatsApp preview id */
    onurb_turn_meta_t *meta;           /* thread headers (optional) */
} reply_text_t;

static int hexval(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1;
}

static void percent_decode(const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (; *in && o + 1 < cap; in++) {
        int hi, lo;
        if (in[0] == '%' && (hi = hexval(in[1])) >= 0 && (lo = hexval(in[2])) >= 0) {
            out[o++] = (char)(hi * 16 + lo);
            in += 2;
        } else {
            out[o++] = *in;
        }
    }
    out[o] = 0;
}

static esp_err_t on_http_event(esp_http_client_event_t *e)
{
    reply_text_t *t = e->user_data;
    if (t && e->event_id == HTTP_EVENT_ON_HEADER) {
        if (t->buf && strcasecmp(e->header_key, "X-Onurb-Reply-Text") == 0) {
            percent_decode(e->header_value, t->buf, t->cap);
        } else if (t->action && strcasecmp(e->header_key, "X-Onurb-Action") == 0) {
            strlcpy(t->action, e->header_value, ONURB_ACTION_ID_LEN);
        } else if (t->meta && strcasecmp(e->header_key, "X-Onurb-Thread") == 0) {
            strlcpy(t->meta->thread, e->header_value, sizeof(t->meta->thread));
        } else if (t->meta && strcasecmp(e->header_key, "X-Onurb-Thread-State") == 0) {
            strlcpy(t->meta->state, e->header_value, sizeof(t->meta->state));
        } else if (t->meta && !t->action && strcasecmp(e->header_key, "X-Onurb-Action") == 0) {
            strlcpy(t->meta->action, e->header_value, sizeof(t->meta->action));
        } else if (t->meta && strcasecmp(e->header_key, "X-Onurb-Heard") == 0) {
            percent_decode(e->header_value, t->meta->heard, sizeof(t->meta->heard));
        }
    }
    return ESP_OK;
}

static void phase(onurb_turn_phase_cb_t cb, onurb_turn_phase_t p)
{
    if (cb) {
        cb(p);
    }
}

#define LIVE INT64_MIN

/* queued_at == LIVE: live turn, reply audio returned. Otherwise a turn sent
 * late from the spool (recorded at unix time queued_at, 0 = unknown): the
 * gateway forwards it but returns no audio, and nothing is shown. */
static esp_err_t turn_post(const char *path, const int16_t *pcm, FILE *src, size_t samples, uint32_t sample_rate,
                           onurb_turn_phase_cb_t on_phase, int64_t queued_at,
                           char *turn_id_out, char *text, size_t text_cap, char *action_out,
                           uint8_t **reply, size_t *reply_len, int *http_status, onurb_turn_meta_t *meta)
{
    reply_text_t rt = {text, text_cap, action_out, meta};
    const char *base = onurb_api_base();
    char url[160];
    char turn_id[33];
    uint8_t header[WAV_HEADER_SIZE];
    const uint32_t data_bytes = samples * sizeof(int16_t);
    esp_err_t err;

    *reply = NULL;
    *reply_len = 0;
    *http_status = 0;
    if (text && text_cap) {
        text[0] = 0;
    }
    if (action_out) {
        action_out[0] = 0;
    }
    if (meta) {
        meta->thread[0] = meta->state[0] = meta->heard[0] = meta->action[0] = 0;
    }

#if !CONFIG_ONURB_ALLOW_PLAIN_HTTP
    if (strncmp(base, "https://", 8) != 0) {
        ESP_LOGE(TAG, "plain HTTP disabled (ONURB_ALLOW_PLAIN_HTTP=n)");
        return ESP_ERR_NOT_SUPPORTED;
    }
#endif
    if (strncmp(base, "http://", 7) == 0) {
        ESP_LOGW(TAG, "using plain HTTP - LAN/mock use only");
    }
    snprintf(url, sizeof(url), "%s%s", base, path);
    for (int i = 0; i < 4; i++) {
        snprintf(turn_id + 8 * i, 9, "%08" PRIx32, esp_random());
    }
    if (turn_id_out) {
        memcpy(turn_id_out, turn_id, sizeof(turn_id));   /* known before upload: cancellable */
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 300000,              /* may queue behind an earlier turn at the gateway */
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .event_handler = on_http_event,
        .user_data = &rt,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(client, "Content-Type", "audio/wav");
    esp_http_client_set_header(client, "Accept", "audio/wav");
    esp_http_client_set_header(client, "X-Onurb-Turn-Id", turn_id);
    esp_http_client_set_header(client, "X-Onurb-Trigger", meta && meta->trigger[0] ? meta->trigger : "button");
    if (meta && meta->reply_to[0]) {
        esp_http_client_set_header(client, "X-Onurb-Thread", meta->reply_to);
    }
    if (meta && meta->rec[0]) {
        esp_http_client_set_header(client, "X-Onurb-Rec", meta->rec);
    }
    if (queued_at != LIVE) {
        char at[24];
        snprintf(at, sizeof(at), "%lld", (long long)queued_at);
        esp_http_client_set_header(client, "X-Onurb-Queued", "1");
        esp_http_client_set_header(client, "X-Onurb-Recorded-At", at);
    }
    float pitch, roll;
    char tilt[48];
    onurb_imu_get_tilt(&pitch, &roll);
    snprintf(tilt, sizeof(tilt), "pitch=%.0f;roll=%.0f", pitch, roll);
    esp_http_client_set_header(client, "X-Onurb-Tilt", tilt);
    onurb_api_headers(client, base);

    phase(on_phase, ONURB_TURN_UPLOADING);
    /* Modem sleep (wifi.c default) throttles a burst upload; off for the upload only. */
    if (queued_at == LIVE) {
        esp_wifi_set_ps(WIFI_PS_NONE);
    }
    int64_t up_t0 = esp_timer_get_time();
    err = esp_http_client_open(client, WAV_HEADER_SIZE + data_bytes);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "connect to %s failed: %s", base, esp_err_to_name(err));
        goto done;
    }
    wav_write_header(header, sample_rate, 1, data_bytes);
    if (esp_http_client_write(client, (const char *)header, sizeof(header)) != sizeof(header)) {
        err = ESP_FAIL;
        goto done;
    }
    static char file_chunk[CHUNK_BYTES];    /* only the spool task streams from a file */
    for (size_t off = 0; off < data_bytes; off += CHUNK_BYTES) {
        int n = data_bytes - off < CHUNK_BYTES ? data_bytes - off : CHUNK_BYTES;
        const char *data = (const char *)pcm + off;
        if (src != NULL) {                  /* queued 90 s turn: don't load 4 MB into PSRAM */
            if (fread(file_chunk, 1, n, src) != (size_t)n) {
                ESP_LOGE(TAG, "spool file short at %u bytes", (unsigned)off);
                err = ESP_FAIL;
                goto done;
            }
            data = file_chunk;
        }
        if (esp_http_client_write(client, data, n) != n) {
            ESP_LOGE(TAG, "upload failed at %u/%" PRIu32 " bytes", (unsigned)off, data_bytes);
            err = ESP_FAIL;
            goto done;
        }
    }

    ESP_LOGI(TAG, "upload %u KB in %.2f s", (unsigned)(data_bytes / 1024), (esp_timer_get_time() - up_t0) / 1e6);
    if (queued_at == LIVE) {
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    }
    phase(on_phase, ONURB_TURN_WAITING);
    int64_t content_len = esp_http_client_fetch_headers(client);
    *http_status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "turn %.8s: HTTP %d, %" PRId64 " bytes", turn_id, *http_status, content_len);
    if (*http_status == 204) {              /* nothing was said: no reply, dismiss silently */
        err = ESP_OK;
        goto done;
    }
    if (*http_status != 200) {
        err = ESP_FAIL;
        goto done;
    }
    if (queued_at != LIVE) {                /* delivered; the reply stays on the phone */
        err = ESP_OK;
        goto done;
    }
    if (content_len <= 0 || content_len > MAX_REPLY_BYTES) {
        ESP_LOGE(TAG, "unsupported reply length %" PRId64, content_len);
        err = ESP_ERR_INVALID_SIZE;
        goto done;
    }

    phase(on_phase, ONURB_TURN_DOWNLOADING);
    uint8_t *buf = heap_caps_malloc(content_len, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        err = ESP_ERR_NO_MEM;
        goto done;
    }
    size_t got = 0;
    while (got < (size_t)content_len) {
        int n = esp_http_client_read(client, (char *)buf + got, content_len - got);
        if (n <= 0) {
            break;
        }
        got += n;
    }
    if (got != (size_t)content_len) {
        ESP_LOGE(TAG, "short reply: %u/%" PRId64, (unsigned)got, content_len);
        free(buf);
        err = ESP_FAIL;
        goto done;
    }
    *reply = buf;
    *reply_len = got;
    err = ESP_OK;

done:
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

esp_err_t onurb_action_edit(const char *action, const int16_t *pcm, size_t samples, uint32_t sample_rate,
                            onurb_turn_phase_cb_t on_phase, char turn_id[33], char *reply_text, size_t text_cap,
                            char new_action[ONURB_ACTION_ID_LEN], uint8_t **reply, size_t *reply_len, int *http_status,
                            onurb_turn_meta_t *meta)
{
    char path[64];
    snprintf(path, sizeof(path), "/v1/actions/%s/edit", action);
    return turn_post(path, pcm, NULL, samples, sample_rate, on_phase, LIVE, turn_id, reply_text, text_cap,
                     new_action, reply, reply_len, http_status, meta);
}

esp_err_t onurb_turn_send(const int16_t *pcm, size_t samples, uint32_t sample_rate,
                          onurb_turn_phase_cb_t on_phase, char turn_id[33],
                          char *reply_text, size_t text_cap, char action_id[ONURB_ACTION_ID_LEN],
                          uint8_t **reply, size_t *reply_len, int *http_status, onurb_turn_meta_t *meta)
{
    const char *base = onurb_api_base();
    esp_err_t err = turn_post("/v1/turns", pcm, NULL, samples, sample_rate, on_phase, LIVE, turn_id, reply_text,
                              text_cap, action_id, reply, reply_len, http_status, meta);
    if (err != ESP_OK && *http_status == 0 && onurb_api_failed(base)) {   /* away from home / Mac off */
        err = turn_post("/v1/turns", pcm, NULL, samples, sample_rate, on_phase, LIVE, turn_id, reply_text,
                        text_cap, action_id, reply, reply_len, http_status, meta);
    }
    return err;
}

esp_err_t onurb_turn_send_queued_file(FILE *src, size_t samples, uint32_t sample_rate,
                                      int64_t recorded_at, int *http_status)
{
    uint8_t *reply;
    size_t len;
    return turn_post("/v1/turns", NULL, src, samples, sample_rate, NULL, recorded_at < 0 ? 0 : recorded_at,
                     NULL, NULL, 0, NULL, &reply, &len, http_status, NULL);
}

static esp_err_t post_csv(const char *path, const char *csv, size_t len,
                          const char *h1, const char *v1, const char *h2, const char *v2);

esp_err_t onurb_http_get(const char *path, size_t max_len, uint8_t **body, size_t *len, int *http_status,
                         char *reply_text, size_t text_cap, onurb_turn_meta_t *meta)
{
    char url[160];
    reply_text_t rt = {reply_text, text_cap, NULL, meta};
    if (meta) {
        meta->thread[0] = meta->state[0] = meta->heard[0] = meta->action[0] = 0;
    }
    *body = NULL;
    *len = 0;
    *http_status = 0;
    if (reply_text && text_cap) {
        reply_text[0] = 0;
    }
    const char *base = onurb_api_base();
    snprintf(url, sizeof(url), "%s%s", base, path);
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
        .event_handler = on_http_event,
        .user_data = &rt,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) {
        return ESP_ERR_NO_MEM;
    }
    onurb_api_headers(c, base);
    esp_err_t err = esp_http_client_open(c, 0);
    if (err == ESP_OK) {
        int64_t n = esp_http_client_fetch_headers(c);
        *http_status = esp_http_client_get_status_code(c);
        if (*http_status == 200 && n > 0 && (size_t)n <= max_len) {
            uint8_t *buf = heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM);
            size_t got = 0;
            while (buf && got < (size_t)n) {
                int r = esp_http_client_read(c, (char *)buf + got, n - got);
                if (r <= 0) {
                    break;
                }
                got += r;
            }
            if (buf && got == (size_t)n) {
                buf[n] = 0;                  /* JSON callers get a C string */
                *body = buf;
                *len = got;
            } else {
                free(buf);
                err = ESP_FAIL;
            }
        } else if (*http_status != 204 && *http_status != 200) {
            err = ESP_FAIL;
        }
    }
    if (err != ESP_OK && *http_status == 0) {
        onurb_api_failed(base);                  /* next poll tries the other gateway */
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return err;
}

esp_err_t onurb_action_post(const char *action, const char *verb)
{
    char path[64];
    snprintf(path, sizeof(path), "/v1/actions/%s/%s", action, verb);
    return post_csv(path, "", 0, "X-Onurb-Action", action, NULL, NULL);
}

esp_err_t onurb_turn_cancel(const char *turn_id)
{
    return post_csv("/v1/turns/cancel", "", 0, "X-Onurb-Turn-Id", turn_id, NULL, NULL);
}

static esp_err_t post_csv(const char *path, const char *csv, size_t len,
                          const char *h1, const char *v1, const char *h2, const char *v2)
{
    char url[160];

    const char *base = onurb_api_base();
    snprintf(url, sizeof(url), "%s%s", base, path);
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(client, "Content-Type", "text/csv");
    esp_http_client_set_header(client, h1, v1);
    if (h2) {
        esp_http_client_set_header(client, h2, v2);
    }
    onurb_api_headers(client, base);
    esp_http_client_set_post_field(client, csv, len);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err == ESP_OK && status != 200) {
        err = ESP_FAIL;
    }
    ESP_LOGI(TAG, "%s upload: %s (HTTP %d)", path, esp_err_to_name(err), status);
    return err;
}

esp_err_t onurb_turn_post_imu(const char *csv, size_t len, int capture_id, const char *label)
{
    char id[12];
    snprintf(id, sizeof(id), "%d", capture_id);
    return post_csv("/v1/imu-log", csv, len, "X-Onurb-Capture", id, "X-Onurb-Label", label);
}

esp_err_t onurb_turn_post_gesture_feedback(const char *csv, size_t len, const char *label, const char *detail)
{
    return post_csv("/v1/gesture-feedback", csv, len, "X-Onurb-Label", label, "X-Onurb-Detail", detail);
}
