#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"

#include "turn.h"
#include "wav.h"

static const char *TAG = "onurb_turn";

#define MAX_REPLY_BYTES (2 * 1024 * 1024)
#define CHUNK_BYTES     4096

static void phase(onurb_turn_phase_cb_t cb, onurb_turn_phase_t p)
{
    if (cb) {
        cb(p);
    }
}

esp_err_t onurb_turn_send(const int16_t *pcm, size_t samples, uint32_t sample_rate,
                          onurb_turn_phase_cb_t on_phase,
                          uint8_t **reply, size_t *reply_len, int *http_status)
{
    const char *base = CONFIG_ONURB_API_BASE_URL;
    char url[160];
    char turn_id[33];
    uint8_t header[WAV_HEADER_SIZE];
    const uint32_t data_bytes = samples * sizeof(int16_t);
    esp_err_t err;

    *reply = NULL;
    *reply_len = 0;
    *http_status = 0;

#if !CONFIG_ONURB_ALLOW_PLAIN_HTTP
    if (strncmp(base, "https://", 8) != 0) {
        ESP_LOGE(TAG, "plain HTTP disabled (ONURB_ALLOW_PLAIN_HTTP=n)");
        return ESP_ERR_NOT_SUPPORTED;
    }
#endif
    if (strncmp(base, "http://", 7) == 0) {
        ESP_LOGW(TAG, "using plain HTTP - LAN/mock use only");
    }
    snprintf(url, sizeof(url), "%s/v1/turns", base);
    for (int i = 0; i < 4; i++) {
        snprintf(turn_id + 8 * i, 9, "%08" PRIx32, esp_random());
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 90000,               /* agent replies can take a while */
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(client, "Content-Type", "audio/wav");
    esp_http_client_set_header(client, "Accept", "audio/wav");
    esp_http_client_set_header(client, "X-Onurb-Turn-Id", turn_id);
    esp_http_client_set_header(client, "X-Onurb-Trigger", "button");
    if (strlen(CONFIG_ONURB_API_TOKEN) > 0) {
        char auth[128];
        snprintf(auth, sizeof(auth), "Bearer %s", CONFIG_ONURB_API_TOKEN);
        esp_http_client_set_header(client, "Authorization", auth);
    }

    phase(on_phase, ONURB_TURN_UPLOADING);
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
    for (size_t off = 0; off < data_bytes; off += CHUNK_BYTES) {
        int n = data_bytes - off < CHUNK_BYTES ? data_bytes - off : CHUNK_BYTES;
        if (esp_http_client_write(client, (const char *)pcm + off, n) != n) {
            ESP_LOGE(TAG, "upload failed at %u/%" PRIu32 " bytes", (unsigned)off, data_bytes);
            err = ESP_FAIL;
            goto done;
        }
    }

    phase(on_phase, ONURB_TURN_WAITING);
    int64_t content_len = esp_http_client_fetch_headers(client);
    *http_status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "turn %.8s: HTTP %d, %" PRId64 " bytes", turn_id, *http_status, content_len);
    if (*http_status != 200) {
        err = ESP_FAIL;
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
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}
