#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "spool.h"
#include "turn.h"
#include "wifi.h"

static const char *TAG = "onurb_spool";

#define BASE       "/spool"
#define LABEL      "spool"
#define MAGIC      0x4C4F4F53u            /* "SOOL" */
#define RESERVE    (64 * 1024)            /* keep SPIFFS from filling completely */
#define POLL_MS    10000
#define BACKOFF_MS 60000

typedef struct {
    uint32_t magic;
    uint32_t sample_rate;
    int64_t recorded_at;                  /* unix time, 0 = clock not synced */
} spool_hdr_t;

static SemaphoreHandle_t s_lock;
static unsigned s_next_seq;
static volatile int s_count;
static volatile bool s_sending;           /* a queued turn is in flight (don't reboot) */

/* Scan the queue: count files, find the oldest (lowest seq) and the next seq. */
static int scan(char *oldest, size_t cap)
{
    DIR *d = opendir(BASE);
    unsigned lo = UINT32_MAX, hi = 0;
    int n = 0;

    if (d == NULL) {
        return 0;
    }
    for (struct dirent *e; (e = readdir(d)) != NULL;) {
        unsigned seq;
        if (sscanf(e->d_name, "q%u.pcm", &seq) == 1) {
            n++;
            lo = seq < lo ? seq : lo;
            hi = seq > hi ? seq : hi;
        }
    }
    closedir(d);
    if (n > 0 && s_next_seq <= hi) {
        s_next_seq = hi + 1;
    }
    if (oldest && n > 0) {
        snprintf(oldest, cap, BASE "/q%06u.pcm", lo);
    }
    s_count = n;
    return n;
}

esp_err_t onurb_spool_save(const int16_t *pcm, size_t samples, uint32_t sample_rate)
{
    size_t total = 0, used = 0, need = sizeof(spool_hdr_t) + samples * sizeof(int16_t);
    char path[40];

    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (esp_spiffs_info(LABEL, &total, &used) == ESP_OK && used + need + RESERVE > total) {
        /* SPIFFS counts deleted (sent) files as used until garbage-collected: after a
         * backlog was sent the partition looked full with nothing queued (2026-11-10). */
        esp_spiffs_gc(LABEL, need + RESERVE);
        esp_spiffs_info(LABEL, &total, &used);
    }
    if (esp_spiffs_info(LABEL, &total, &used) != ESP_OK || used + need + RESERVE > total) {
        ESP_LOGW(TAG, "queue full (%u/%u KB used)", (unsigned)(used / 1024), (unsigned)(total / 1024));
        return ESP_ERR_NO_MEM;
    }
    time_t now = time(NULL);
    spool_hdr_t h = {MAGIC, sample_rate, now > 1700000000 ? (int64_t)now : 0};

    xSemaphoreTake(s_lock, portMAX_DELAY);
    snprintf(path, sizeof(path), BASE "/q%06u.pcm", s_next_seq++);
    FILE *f = fopen(path, "wb");
    bool ok = f && fwrite(&h, sizeof(h), 1, f) == 1 &&
              fwrite(pcm, sizeof(int16_t), samples, f) == samples;
    if (f) {
        fclose(f);
    }
    if (!ok) {
        remove(path);
    }
    scan(NULL, 0);
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "%s %s (%.1f s), %d queued", ok ? "saved" : "FAILED to save", path,
             (double)samples / sample_rate, s_count);
    return ok ? ESP_OK : ESP_FAIL;
}

bool onurb_spool_busy(void)
{
    return s_sending;
}

int onurb_spool_count(void)
{
    return s_count;
}

/* Send the oldest queued turn. Returns the delay before the next attempt. */
static int send_oldest(void)
{
    char path[40];
    struct stat st;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = scan(path, sizeof(path));
    xSemaphoreGive(s_lock);
    if (n == 0 || onurb_wifi_level() < 1) {
        return POLL_MS;
    }
    if (stat(path, &st) != 0 || st.st_size < (off_t)sizeof(spool_hdr_t)) {
        remove(path);
        return 1000;
    }
    FILE *f = fopen(path, "rb");             /* streamed: a 90 s turn is 4.3 MB */
    spool_hdr_t h;
    if (f == NULL || fread(&h, sizeof(h), 1, f) != 1 || h.magic != MAGIC) {
        ESP_LOGE(TAG, "%s unreadable - dropped", path);
        if (f) {
            fclose(f);
        }
        remove(path);
        return 1000;
    }
    size_t samples = ((size_t)st.st_size - sizeof(h)) / sizeof(int16_t);
    int status = 0;
    s_sending = true;
    esp_err_t err = onurb_turn_send_queued_file(f, samples, h.sample_rate, h.recorded_at, &status);
    fclose(f);
    if (err == ESP_OK) {
        remove(path);                         /* before clearing s_sending: no resend after reboot */
    }
    s_sending = false;

    if (err == ESP_OK) {
        remove(path);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        scan(NULL, 0);
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "sent %s (%.1f s), %d left", path, (double)samples / h.sample_rate, s_count);
        return 1000;
    }
    if (status >= 400 && status < 500) {      /* gateway refuses it: retrying won't help */
        ESP_LOGE(TAG, "%s rejected (HTTP %d) - dropped", path, status);
        remove(path);
        return 1000;
    }
    ESP_LOGW(TAG, "send %s failed (HTTP %d) - retry in %d s", path, status, BACKOFF_MS / 1000);
    return BACKOFF_MS;
}

static void spool_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(5000));
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(send_oldest()));
    }
}

esp_err_t onurb_spool_start(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = BASE,
        .partition_label = LABEL,
        .max_files = 4,
        .format_if_mount_failed = true,      /* first boot: empty partition */
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s - offline queue disabled", esp_err_to_name(err));
        return err;
    }
    s_lock = xSemaphoreCreateMutex();
    size_t total = 0, used = 0;
    esp_spiffs_info(LABEL, &total, &used);
    ESP_LOGI(TAG, "%d queued, %u/%u KB used", scan(NULL, 0), (unsigned)(used / 1024), (unsigned)(total / 1024));
    xTaskCreatePinnedToCore(spool_task, "spool", 8192, NULL, 3, NULL, 0);
    return ESP_OK;
}
