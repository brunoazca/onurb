/*
 * Onurb hardware bring-up (SPEC §16.2, tests B-01, B-02, B-04, B-05 + a first
 * hold-to-record -> play loop).
 *
 * 0. Configures the AXP2101 charger and logs battery state (B-06), again every 30 s.
 * 1. Logs chip/flash/PSRAM info, turns on the status screen, starts Wi-Fi
 *    (auto-reconnect + SNTP check).
 * 2. Scans the BSP I2C bus and names the devices we expect (B-04).
 * 3. Brings audio up with the BSP 24 kHz voice profile BEFORE creating codec
 *    devices; otherwise the BSP silently falls back to its legacy 22.05 kHz
 *    mono mode with no ES7210 mics selected (SPEC §6.1, trap 1) (B-01).
 * 4. Plays a 1 kHz test tone (B-05).
 * 5. Quick BOOT tap: countdown + 2.5 s IMU capture streamed as CSV.
 * 6. Raise-to-mouth gesture starts a turn; a medium-strong movement ends it.
 * 7. Loop: hold BOOT to talk (90 s limit); on
 *    release, log per-TDM-slot levels (B-02), send the loudest slot as WAV to
 *    the gateway (Turn API) and play the spoken reply.
 */
#include <inttypes.h>
#include <math.h>
#include "esp_pm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_chip_info.h"
#include "esp_codec_dev.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_timer.h"

#include "bsp/esp-bsp.h"
#include "cJSON.h"

#include "haptic.h"
#include "imu.h"
#include "ota.h"
#include "screen.h"
#include "spool.h"
#include "ui_feedback.h"
#include "pmu.h"
#include "turn.h"
#include "ui.h"
#include "wav.h"
#include "wifi.h"

static const char *TAG = "onurb_bringup";

#define SAMPLE_RATE      24000
#define TDM_SLOTS        4
#define FRAMES_PER_READ  240            /* 10 ms */
#define TDM_ALL_SLOTS    0x0F

/* Slot order claimed by the factory bsp_extra README; B-02 confirms or refutes it. */
static const char *const SLOT_NAMES[TDM_SLOTS] = {
    "slot0 (MIC1?)", "slot1 (MIC3/ref?)", "slot2 (MIC2?)", "slot3 (MIC4/NC?)",
};

static esp_codec_dev_handle_t s_spk;
static esp_codec_dev_handle_t s_mic;

static void log_system_info(void)
{
    esp_chip_info_t chip;
    uint32_t flash_size = 0;

    esp_chip_info(&chip);
    esp_flash_get_size(NULL, &flash_size);
    ESP_LOGI(TAG, "chip: ESP32-S3 rev %d, %d cores", chip.revision, chip.cores);
    ESP_LOGI(TAG, "flash: %" PRIu32 " MB, PSRAM: %u KB, free PSRAM: %u KB",
             flash_size / (1024 * 1024),
             (unsigned)(esp_psram_get_size() / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}

static const char *i2c_device_name(uint16_t addr)
{
    /* Expected addresses (datasheet defaults) - unverified until this scan runs. */
    switch (addr) {
    case 0x18: return "ES8311 speaker codec";
    case 0x34: return "AXP2101 PMIC";
    case 0x38: return "FT5x06 touch";
    case 0x40: return "ES7210 mic ADC";
    case 0x51: return "PCF85063 RTC";
    case 0x6A:
    case 0x6B: return "QMI8658 IMU";
    default:   return "unknown";
    }
}

static void i2c_scan(void)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    int found = 0;

    ESP_LOGI(TAG, "B-04: scanning I2C bus");
    for (uint16_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(bus, addr, 50) == ESP_OK) {
            ESP_LOGI(TAG, "  0x%02X  %s", addr, i2c_device_name(addr));
            found++;
        }
    }
    ESP_LOGI(TAG, "B-04: %d device(s) found", found);
}

static void audio_init(void)
{
    ESP_LOGI(TAG, "B-01: bsp_audio_init_voice_24k (TX std stereo + RX 4-slot TDM, 24 kHz)");
    ESP_ERROR_CHECK(bsp_audio_init_voice_24k());

    s_spk = bsp_audio_codec_speaker_init();
    s_mic = bsp_audio_codec_microphone_init();
    if (s_spk == NULL || s_mic == NULL) {
        ESP_LOGE(TAG, "B-01: codec init failed (spk=%p mic=%p)", s_spk, s_mic);
        abort();
    }

    esp_codec_dev_sample_info_t out_fs = {
        .sample_rate = SAMPLE_RATE,
        .channel = 2,
        .bits_per_sample = 16,
    };
    esp_codec_dev_sample_info_t in_fs = {
        .sample_rate = SAMPLE_RATE,
        .channel = TDM_SLOTS,
        .channel_mask = TDM_ALL_SLOTS,
        .bits_per_sample = 16,
    };
    ESP_ERROR_CHECK(esp_codec_dev_open(s_spk, &out_fs));
    ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(s_spk, CONFIG_ONURB_VOLUME));
    ESP_ERROR_CHECK(esp_codec_dev_open(s_mic, &in_fs));
    ESP_ERROR_CHECK(esp_codec_dev_set_in_channel_gain(s_mic, BSP_AUDIO_ES7210_CONNECTED_MIC_MASK,
                                                      (float)CONFIG_ONURB_MIC_GAIN_DB));
    ESP_LOGI(TAG, "B-01: audio up: out 24 kHz/16-bit/2ch vol %d, in 24 kHz/16-bit/4 slots gain %d dB",
             CONFIG_ONURB_VOLUME, CONFIG_ONURB_MIC_GAIN_DB);
}

static void play_tone(int freq_hz, int duration_ms)
{
    int16_t buf[FRAMES_PER_READ * 2];
    int total = SAMPLE_RATE * duration_ms / 1000;
    int n = 0;

    while (n < total) {
        int frames = total - n < FRAMES_PER_READ ? total - n : FRAMES_PER_READ;
        for (int i = 0; i < frames; i++, n++) {
            int16_t v = (int16_t)(0.3f * 32767.0f * sinf(2.0f * (float)M_PI * freq_hz * n / SAMPLE_RATE));
            buf[2 * i] = v;
            buf[2 * i + 1] = v;
        }
        esp_codec_dev_write(s_spk, buf, frames * 2 * sizeof(int16_t));
    }
}

static bool button_pressed(void)
{
    return gpio_get_level(CONFIG_ONURB_BUTTON_GPIO) == 0;
}

static void button_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << CONFIG_ONURB_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
}

/* Discard audio the RX DMA buffered while nobody was reading. */
static void mic_flush(int16_t *scratch, int ms)
{
    for (int i = 0; i < ms / 10; i++) {
        esp_codec_dev_read(s_mic, scratch, FRAMES_PER_READ * TDM_SLOTS * sizeof(int16_t));
    }
}

static void ui_refresh(void)
{
    onurb_pmu_status_t st;

    if (onurb_pmu_get_status(&st) == ESP_OK) {
        onurb_ui_set_battery(st.battery ? st.percent : -1, st.charging, st.usb_in);
    }
    onurb_ui_set_wifi((onurb_ui_wifi_t)onurb_wifi_level());
}

/* The start beep leaks electrically into the ES7210 echo-reference slot for
 * ~80 ms (-13 dBFS); measured over the whole clip it can outweigh quiet
 * speech, and the near-silent reference slot was sent (whisper hears "you").
 * So slot levels skip the first 200 ms, and the sent audio drops 100 ms. */
#define PICK_SKIP_FRAMES (SAMPLE_RATE / 5)
#define SEND_SKIP_FRAMES (SAMPLE_RATE / 10)

/* Per-slot level stats accumulated chunk by chunk while recording (integer
 * math; the old two-pass double loop over the 3 s head ran in software floats
 * and stalled the recording loop ~at 3 s: frozen timer, risk of lost audio). */
typedef struct {
    int64_t sum[TDM_SLOTS], sq[TDM_SLOTS];
    int peak[TDM_SLOTS];
    int frames;
} slot_stats_t;

static void slot_stats_add(slot_stats_t *st, const int16_t *chunk, int frames)
{
    for (int f = 0; f < frames; f++) {
        for (int s = 0; s < TDM_SLOTS; s++) {
            int v = chunk[f * TDM_SLOTS + s];
            st->sum[s] += v;
            st->sq[s] += (int64_t)v * v;
            int a = v < 0 ? -v : v;
            if (a > st->peak[s]) {
                st->peak[s] = a;
            }
        }
    }
    st->frames += frames;
}

/* Loudest slot (DC removed) from the accumulated stats; logs the levels. */
static int slot_stats_pick(const slot_stats_t *st)
{
    int loudest = 0;
    float best = -1.0f;
    int n = st->frames > 0 ? st->frames : 1;
    ESP_LOGI(TAG, "B-02: %.2f s captured; per-slot levels (DC removed):", (double)n / SAMPLE_RATE);
    for (int s = 0; s < TDM_SLOTS; s++) {
        float mean = (float)st->sum[s] / n;
        float var = (float)st->sq[s] / n - mean * mean;
        float rms = var > 0 ? sqrtf(var) : 0;
        ESP_LOGI(TAG, "  %-18s rms %6.1f dBFS  peak %6.1f dBFS  dc %7.1f", SLOT_NAMES[s],
                 rms > 0 ? 20.0f * log10f(rms / 32768.0f) : -120.0f,
                 st->peak[s] > 0 ? 20.0f * log10f(st->peak[s] / 32768.0f) : -120.0f, mean);
        if (rms > best) {
            best = rms;
            loudest = s;
        }
    }
    return loudest;
}

static int report_levels(const int16_t *rec, int frames)
{
    if (frames > 2 * PICK_SKIP_FRAMES) {
        rec += (size_t)PICK_SKIP_FRAMES * TDM_SLOTS;
        frames -= PICK_SKIP_FRAMES;
    }
    double mean[TDM_SLOTS] = {0};
    double sq[TDM_SLOTS] = {0};
    int peak[TDM_SLOTS] = {0};
    int loudest = 0;
    double best = -1.0;

    for (int f = 0; f < frames; f++) {
        for (int s = 0; s < TDM_SLOTS; s++) {
            mean[s] += rec[f * TDM_SLOTS + s];
        }
    }
    for (int s = 0; s < TDM_SLOTS; s++) {
        mean[s] /= frames;
    }
    for (int f = 0; f < frames; f++) {
        for (int s = 0; s < TDM_SLOTS; s++) {
            int v = rec[f * TDM_SLOTS + s];
            double d = v - mean[s];
            sq[s] += d * d;
            if (abs(v) > peak[s]) {
                peak[s] = abs(v);
            }
        }
    }

    ESP_LOGI(TAG, "B-02: %.2f s captured; per-slot levels (DC removed):", (double)frames / SAMPLE_RATE);
    for (int s = 0; s < TDM_SLOTS; s++) {
        double rms = sqrt(sq[s] / frames);
        double rms_db = rms > 0 ? 20.0 * log10(rms / 32768.0) : -120.0;
        double peak_db = peak[s] > 0 ? 20.0 * log10(peak[s] / 32768.0) : -120.0;
        ESP_LOGI(TAG, "  %-18s rms %6.1f dBFS  peak %6.1f dBFS  dc %7.1f", SLOT_NAMES[s], rms_db, peak_db, mean[s]);
        if (rms > best) {
            best = rms;
            loudest = s;
        }
    }

    return loudest;
}

/* Play a 16-bit 24 kHz mono/stereo WAV on the stereo speaker stream (SPEC §6.3). */
static bool play_reply(const uint8_t *buf, size_t len)
{
    wav_info_t w;
    int rc = wav_parse(buf, len, &w);

    if (rc != 0 || w.bits != 16 || w.sample_rate != SAMPLE_RATE || (w.channels != 1 && w.channels != 2)) {
        ESP_LOGE(TAG, "reply format unsupported (rc %d, %u Hz, %u-bit, %u ch)",
                 rc, (unsigned)w.sample_rate, w.bits, w.channels);
        return false;
    }
    const int16_t *pcm = (const int16_t *)w.data;
    size_t frames = w.data_bytes / (2 * w.channels);
    ESP_LOGI(TAG, "playing reply: %.1f s, %u ch", (double)frames / SAMPLE_RATE, w.channels);
    onurb_ui_set_state(ONURB_UI_PLAYING, 0);

    int16_t out[FRAMES_PER_READ * 2];
    for (size_t f = 0; f < frames; f += FRAMES_PER_READ) {
        size_t n = frames - f < FRAMES_PER_READ ? frames - f : FRAMES_PER_READ;
        for (size_t i = 0; i < n; i++) {
            out[2 * i] = pcm[(f + i) * w.channels];
            out[2 * i + 1] = pcm[(f + i) * w.channels + (w.channels - 1)];
        }
        esp_codec_dev_write(s_spk, out, n * 2 * sizeof(int16_t));
    }
    return true;
}

/* ---------------------------------------------------------------- turn pipeline
 * Bruno (2026-11-09): record the next request while earlier ones are still
 * processing; replies pile up in the history (newest on top) in request order.
 *
 * - record() stays on the main task. When it ends, the turn becomes a job with
 *   a sequence number and its own worker task (upload + wait for the reply),
 *   and the main loop is free again (raise-to-mouth starts the next one).
 * - Jobs are kept in request order; a reply is presented only when it is the
 *   oldest job, so replies that arrive out of order still show in order.
 * - The 90 s audio buffer is shared: a new recording waits until the previous
 *   upload has finished (s_uploading; fractions of a second for short turns).
 * - The on-screen X cancels only a request still processing (the newest one).
 * - No Wi-Fi: the recording goes to the offline spool as before. */
#define MAX_JOBS 6

typedef enum {
    STOP_ON_RELEASE,    /* hold-to-talk: BOOT released */
    STOP_ON_MOTION,     /* gesture-started: wrist moves again, or BOOT pressed */
    STOP_ON_SILENCE,    /* spoken edit while reading the preview: silence or BOOT ends it */
} stop_mode_t;

static int16_t *s_mono;                          /* the 90 s recording buffer (defined below) */
static char s_rec_diag[48];                      /* last recording: audio vs wall-clock seconds */

typedef struct {
    int seq;
    int frames;
    char turn_id[33];                            /* set by the worker before the upload */
    char text[512];                              /* reply text for the screen */
    bool done, abandoned;                        /* guarded by s_job_mux */
    bool spooled, spool_full;
    esp_err_t err;
    int status;
    uint8_t *reply;
    size_t reply_len;
    char action[ONURB_ACTION_ID_LEN];            /* WhatsApp preview proposed by the agent ("" = none) */
    char edit_of[ONURB_ACTION_ID_LEN];           /* set: this recording is a spoken change to that preview */
    char *action_json;                           /* fetched by the worker: {name, messages[]} */
    uint8_t *action_avatar;                      /* 96x96 (contact) or 120x120 (product) RGB565, or NULL */
    int action_avatar_px;
    char *gesture_csv;                           /* IMU window of the gesture that started it (or NULL) */
    size_t gesture_len;
    char gesture_detail[128];
    uint32_t thread;                             /* local id of its side stick (0 = none) */
    int64_t created_us;
    onurb_turn_meta_t meta;                      /* thread in (reply_to) / out (id, state, heard) */
} turn_job_t;

static portMUX_TYPE s_job_mux = portMUX_INITIALIZER_UNLOCKED;
static turn_job_t *s_jobs[MAX_JOBS];             /* in request order; guarded by s_job_mux */
static int s_njobs;
static int s_next_seq = 1;
static volatile bool s_uploading;                /* a worker is still reading s_mono */

typedef enum { UI_IDLE, UI_PROCESSING, UI_REPLY, UI_NOTICE, UI_FEEDBACK, UI_ACTION } ui_mode_t;
static ui_mode_t s_ui_mode = UI_IDLE;
static int64_t s_ui_until_us;                    /* UI_REPLY / UI_NOTICE end */
static bool s_screen_held;
static turn_job_t *s_last_job;                   /* job just started by record_and_queue (main task) */

/* The WhatsApp preview waiting for Confirm / Cancel / Edit on the watch (one at
 * a time; a newer proposal replaces it). Re-shown after other activity. */
#define ACTION_TTL_US (10LL * 60 * 1000000)
static struct {
    char id[ONURB_ACTION_ID_LEN];
    char *json;
    uint8_t *avatar;
    int avatar_px;
    bool purchase;                               /* Confirm buys (checkout task) instead of sending */
    int64_t until_us;
} s_action;
static volatile bool s_inbox_busy;
#define PROCESSING_LIT_S 12
#define SCREEN_DETAIL_IDLE_S 60
#define THREAD_HIST_CAP (ONURB_UI_THREAD_HIST_MAX + 1)
static int64_t s_processing_lit_until_us;   /* screen stays on while waiting until then */
static int64_t s_inbox_next_us;

static void free_job(turn_job_t *job)
{
    free(job->reply);
    free(job->gesture_csv);
    free(job->action_json);
    free(job->action_avatar);
    free(job);
}

/* ---- Threads (Bruno, 2026-11-10): one side stick per request -------------
 * Newest first. A stick shows what was said, a spinner while processing, then
 * the short reply of that request. Done ones disappear THREAD_DONE_TTL after
 * finishing; "open" ones (the agent asked something / needs the user) stay,
 * yellow, with a mic button that records an answer to that thread only.
 * Main task only (start_turn, present_next, inbox presentation, pipeline_tick). */
#define MAX_THREADS      10
#define THREAD_DONE_TTL  (60LL * 1000000)
typedef enum { TH_PROCESSING, TH_WORKING, TH_DONE, TH_OPEN, TH_ERROR } th_state_t;
typedef struct {
    uint32_t id;                                 /* local, = seq of the request that opened it */
    th_state_t state;
    th_state_t before;                           /* state before an addition was sent (restored if it fails) */
    char tid[33];                                /* gateway thread id (turn id of the opening request) */
    char heard[ONURB_HEARD_CAP];
    char reply[320];
    char *hist;                                  /* PSRAM, THREAD_HIST_CAP: earlier exchanges (ui.h format) */
    int64_t done_at;
} thread_t;
static thread_t s_threads[MAX_THREADS];
static int s_nthreads;
static bool s_threads_dirty;
static uint32_t s_reply_to;                      /* set while recording an answer to a thread */
static char s_rec_trigger[8] = "button";         /* what started the current recording (X-Onurb-Trigger) */
static void gesture_post(const char *csv, size_t len, const char *label, const char *detail);

static thread_t *thread_find(uint32_t id)
{
    for (int i = 0; i < s_nthreads; i++) {
        if (s_threads[i].id == id) {
            return &s_threads[i];
        }
    }
    return NULL;
}

static thread_t *thread_find_tid(const char *tid)
{
    for (int i = 0; tid && tid[0] && i < s_nthreads; i++) {
        if (strcmp(s_threads[i].tid, tid) == 0) {
            return &s_threads[i];
        }
    }
    return NULL;
}

static void thread_remove(uint32_t id)
{
    for (int i = 0; i < s_nthreads; i++) {
        if (s_threads[i].id == id) {
            free(s_threads[i].hist);
            memmove(&s_threads[i], &s_threads[i + 1], (size_t)(s_nthreads - i - 1) * sizeof(thread_t));
            s_nthreads--;
            s_threads_dirty = true;
            return;
        }
    }
}

/* New stick on top; when full, the oldest finished one (else the oldest) goes. */
static thread_t *thread_new(uint32_t id)
{
    if (s_nthreads == MAX_THREADS) {
        int victim = s_nthreads - 1;
        for (int i = s_nthreads - 1; i >= 0; i--) {
            if (s_threads[i].state == TH_DONE || s_threads[i].state == TH_ERROR) {
                victim = i;
                break;
            }
        }
        thread_remove(s_threads[victim].id);
    }
    memmove(&s_threads[1], &s_threads[0], (size_t)s_nthreads * sizeof(thread_t));
    s_nthreads++;
    memset(&s_threads[0], 0, sizeof(thread_t));
    s_threads[0].hist = heap_caps_calloc(1, THREAD_HIST_CAP, MALLOC_CAP_SPIRAM);
    s_threads[0].id = id;
    s_threads[0].state = TH_PROCESSING;
    s_threads_dirty = true;
    return &s_threads[0];
}

static void thread_finish(thread_t *t, const onurb_turn_meta_t *m, const char *reply)
{
    if (m->thread[0]) {
        strlcpy(t->tid, m->thread, sizeof(t->tid));
    }
    if (m->heard[0]) {
        strlcpy(t->heard, m->heard, sizeof(t->heard));
    }
    strlcpy(t->reply, reply ? reply : "", sizeof(t->reply));
    t->state = strcmp(m->state, "open") == 0 ? TH_OPEN : strcmp(m->state, "working") == 0 ? TH_WORKING : TH_DONE;
    t->done_at = esp_timer_get_time();
    s_threads_dirty = true;
}

/* An answer/addition that didn't go through (cancelled, unsent, nothing heard): the thread goes
 * back to what it was (waiting, working, done...), with a fresh done-TTL. */
static void thread_restore(thread_t *t)
{
    t->state = t->before;
    t->done_at = esp_timer_get_time();
    s_threads_dirty = true;
}

/* Move the thread's current heard/reply into its history (oldest dropped when full). */
static void thread_push_history(thread_t *t)
{
    if (t->hist == NULL || (t->heard[0] == '\0' && t->reply[0] == '\0')) {
        return;
    }
    size_t add = strlen(t->heard) + strlen(t->reply) + 2;
    size_t len = strlen(t->hist);
    while (len > 0 && len + add >= THREAD_HIST_CAP) {
        char *rs = strchr(t->hist, '\x1e');           /* drop the oldest exchange */
        if (rs == NULL) {
            len = 0;
            t->hist[0] = '\0';
            break;
        }
        memmove(t->hist, rs + 1, strlen(rs + 1) + 1);
        len = strlen(t->hist);
    }
    if (len + add < THREAD_HIST_CAP) {
        snprintf(t->hist + len, THREAD_HIST_CAP - len, "%s\x1f%s\x1e", t->heard, t->reply);
    }
    t->heard[0] = '\0';
    t->reply[0] = '\0';
    s_threads_dirty = true;
}

static void threads_tick(int64_t now)
{
    /* A PROCESSING thread with no request in flight (dropped, finished elsewhere) would spin or
     * say "Cancelling..." forever: remove it. Background tasks are WORKING, not PROCESSING. */
    for (int i = s_nthreads - 1; i >= 0; i--) {
        if (s_threads[i].state != TH_PROCESSING) {
            continue;
        }
        bool has_job = false;
        taskENTER_CRITICAL(&s_job_mux);
        for (int k = 0; k < s_njobs; k++) {
            has_job |= s_jobs[k]->thread == s_threads[i].id;
        }
        taskEXIT_CRITICAL(&s_job_mux);
        if (!has_job && !s_uploading) {
            ESP_LOGW(TAG, "thread %lu: processing with no request - removed", (unsigned long)s_threads[i].id);
            if (s_threads[i].tid[0]) {
                thread_restore(&s_threads[i]);   /* an answer that never went: the thread stays as it was */
            } else {
                thread_remove(s_threads[i].id);
            }
        }
    }
    for (int i = s_nthreads - 1; i >= 0; i--) {
        if ((s_threads[i].state == TH_DONE || s_threads[i].state == TH_ERROR) &&
            now - s_threads[i].done_at > THREAD_DONE_TTL &&
            !(ONURB_UI_HAS_THREADS && onurb_ui_thread_view() == s_threads[i].id)) {   /* on screen: the user may add to it */
            thread_remove(s_threads[i].id);
        }
    }
#if ONURB_UI_HAS_THREADS
    if (s_threads_dirty) {
        static const onurb_thread_state_t map[] = {
            [TH_PROCESSING] = ONURB_THREAD_PROCESSING, [TH_WORKING] = ONURB_THREAD_WORKING,
            [TH_DONE] = ONURB_THREAD_DONE, [TH_OPEN] = ONURB_THREAD_OPEN, [TH_ERROR] = ONURB_THREAD_ERROR,
        };
        onurb_thread_t v[MAX_THREADS];
        for (int i = 0; i < s_nthreads; i++) {
            v[i] = (onurb_thread_t){s_threads[i].id, map[s_threads[i].state], s_threads[i].heard, s_threads[i].reply,
                                    s_threads[i].hist, s_threads[i].tid[0] != '\0'};   /* known to the gateway: mic */
        }
        onurb_ui_set_threads(v, s_nthreads);
    }
#endif
    s_threads_dirty = false;
}

static void action_clear(void)
{
    free(s_action.json);
    free(s_action.avatar);
    memset(&s_action, 0, sizeof(s_action));
}
/* After a cancel (Bruno): the circle turns red; tapping again labels that
 * gesture "wrong" for training. Holds the cancelled request's IMU window. */
static char *s_fb_csv;
static size_t s_fb_len;
static char s_fb_detail[128];
#define FEEDBACK_MS 4000

static void fb_discard(void)
{
    free(s_fb_csv);
    s_fb_csv = NULL;
}

/* Red circle with the X for FEEDBACK_MS: tapping it labels this gesture window "wrong"
 * (a false trigger). Takes ownership of csv (malloc'd). Used after a cancel and after a
 * gesture recording was dropped for no voice (Bruno, 2026-11-11: explicit negatives only). */
static void fb_offer(char *csv, size_t len, const char *detail)
{
#if ONURB_UI_HAS_CANCELLED
    fb_discard();
    s_fb_csv = csv;
    s_fb_len = len;
    strlcpy(s_fb_detail, detail ? detail : "", sizeof(s_fb_detail));
    onurb_ui_set_state(ONURB_UI_CANCELLED, 0);
    onurb_ui_cancel_taken();                     /* an earlier tap doesn't count */
    s_ui_mode = UI_FEEDBACK;
    s_ui_until_us = esp_timer_get_time() + (int64_t)FEEDBACK_MS * 1000;
#else
    free(csv);
#endif
}

static void upload_phase(onurb_turn_phase_t p)
{
    if (p == ONURB_TURN_WAITING) {
        s_uploading = false;                     /* audio sent: s_mono is free for the next recording */
    }
}

/* WhatsApp preview proposed with this reply: fetch it now so it shows instantly. */
static void fetch_action(turn_job_t *job)
{
    char path[64];
    size_t len;
    int st;
    snprintf(path, sizeof(path), "/v1/actions/%s", job->action);
    onurb_http_get(path, 8192, (uint8_t **)&job->action_json, &len, &st, NULL, 0, NULL);
    snprintf(path, sizeof(path), "/v1/actions/%s/avatar", job->action);
    onurb_http_get(path, 120 * 120 * 2, &job->action_avatar, &len, &st, NULL, 0, NULL);
    job->action_avatar_px = len == 96 * 96 * 2 ? 96 : len == 120 * 120 * 2 ? 120 : 0;
    if (job->action_avatar && job->action_avatar_px == 0) {
        free(job->action_avatar);
        job->action_avatar = NULL;
    }
}

static void turn_worker(void *arg)
{
    turn_job_t *job = arg;

    if (job->edit_of[0]) {                       /* spoken change to a WhatsApp preview */
        job->err = onurb_action_edit(job->edit_of, s_mono, job->frames, SAMPLE_RATE, upload_phase, job->turn_id,
                                     job->text, sizeof(job->text), job->action, &job->reply, &job->reply_len,
                                     &job->status, &job->meta);
    } else {
        job->err = onurb_turn_send(s_mono, job->frames, SAMPLE_RATE, upload_phase, job->turn_id,
                                   job->text, sizeof(job->text), job->action, &job->reply, &job->reply_len,
                                   &job->status, &job->meta);
    }
    if (job->err == ESP_OK && job->action[0]) {  /* fetch the preview now so it shows instantly */
        fetch_action(job);
    }
    if (job->err != ESP_OK && job->status == 0 && s_uploading) {
        /* never reached the gateway: s_mono still holds this turn -> offline spool */
        job->spooled = onurb_spool_save(s_mono, job->frames, SAMPLE_RATE) == ESP_OK;
        job->spool_full = !job->spooled;
    }
    s_uploading = false;
    taskENTER_CRITICAL(&s_job_mux);
    bool abandoned = job->abandoned;
    job->done = true;
    taskEXIT_CRITICAL(&s_job_mux);
    if (abandoned) {                             /* cancelled: nobody will present it */
        free_job(job);
    }
    vTaskDelete(NULL);
}

static int jobs_pending(void)
{
    taskENTER_CRITICAL(&s_job_mux);
    int n = s_njobs;
    taskEXIT_CRITICAL(&s_job_mux);
    return n;
}

/* Wait (bounded) for the previous upload to release s_mono. */
static bool wait_upload_free(void)
{
    if (!s_uploading) {
        return true;
    }
    onurb_ui_set_status("Sending...", ONURB_UI_COLOR_BUSY);
    for (int i = 0; s_uploading && i < 3000; i++) {   /* <= 60 s (a 90 s turn on slow Wi-Fi) */
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return !s_uploading;
}

static void notice(const char *text, uint32_t color, int ms)
{
    onurb_ui_set_status(text, color);
    s_ui_mode = UI_NOTICE;
    s_ui_until_us = esp_timer_get_time() + (int64_t)ms * 1000;
}

/* The recording in s_mono becomes a background job (or goes to the spool). */
static void start_turn(int frames, const char *edit_of)
{
    if (frames > 2 * SEND_SKIP_FRAMES) {         /* drop the start-beep leakage */
        frames -= SEND_SKIP_FRAMES;
        memmove(s_mono, s_mono + SEND_SKIP_FRAMES, (size_t)frames * sizeof(int16_t));
    }
    turn_job_t *job = NULL;
    if (onurb_wifi_level() < 1) {                 /* just woke from doze: Wi-Fi ~1-2 s */
        onurb_ui_set_status("Connecting...", ONURB_UI_COLOR_BUSY);
        for (int i = 0; onurb_wifi_level() < 1 && i < 200; i++) {   /* <= 20 s: reconnects can take 5-10 s */
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    if (onurb_wifi_level() >= 1 && jobs_pending() < MAX_JOBS) {
        job = calloc(1, sizeof(*job));
    }
    if (job == NULL) {                           /* offline (or too many in flight): spool it */
        bool saved = onurb_spool_save(s_mono, frames, SAMPLE_RATE) == ESP_OK;
        notice(saved ? "Offline - will send later" : "Offline - queue full",
               saved ? ONURB_UI_COLOR_BUSY : ONURB_UI_COLOR_ERROR, 2000);
        return;
    }
    job->seq = s_next_seq++;
    job->created_us = esp_timer_get_time();
    job->frames = frames;
    if (edit_of) {
        strlcpy(job->edit_of, edit_of, sizeof(job->edit_of));
    }
    thread_t *th = s_reply_to ? thread_find(s_reply_to) : NULL;
    if (th) {                                    /* spoken answer / addition inside an existing thread */
        thread_push_history(th);                 /* the question it answers stays visible above */
        strlcpy(job->meta.reply_to, th->tid, sizeof(job->meta.reply_to));
        th->before = th->state == TH_PROCESSING ? TH_OPEN : th->state;
        th->state = TH_PROCESSING;
        s_threads_dirty = true;
    } else {
        th = thread_new((uint32_t)job->seq);
        if (edit_of) {
            strlcpy(th->heard, "Editar mensagem", sizeof(th->heard));
        }
    }
    job->thread = th->id;
    strlcpy(job->meta.rec, s_rec_diag, sizeof(job->meta.rec));
    strlcpy(job->meta.trigger, s_rec_trigger, sizeof(job->meta.trigger));   /* gateway: gesture turns may be dismissed */
    s_reply_to = 0;
    s_last_job = job;
    s_uploading = true;
    taskENTER_CRITICAL(&s_job_mux);
    s_jobs[s_njobs++] = job;
    taskEXIT_CRITICAL(&s_job_mux);
    ESP_LOGI(TAG, "request #%d: %.1f s queued (%d in flight)", job->seq, (double)frames / SAMPLE_RATE, s_njobs);
    if (xTaskCreatePinnedToCore(turn_worker, "turn", 8192, job, 5, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "no memory for request #%d", job->seq);
        s_uploading = false;
        taskENTER_CRITICAL(&s_job_mux);
        s_njobs--;                               /* it was the last one added */
        taskEXIT_CRITICAL(&s_job_mux);
        free(job);
        notice("Error: no memory", ONURB_UI_COLOR_ERROR, 3000);
        return;
    }
    onurb_ui_set_state(ONURB_UI_PROCESSING, 0);
    s_ui_mode = UI_PROCESSING;
    s_processing_lit_until_us = esp_timer_get_time() + (int64_t)PROCESSING_LIT_S * 1000000;
}

/* Cancel a request still processing: thread == 0 -> the newest one (the X on
 * the main circle), else the one shown in that thread (X in its tab). */
/* POST /v1/turns/cancel: the gateway drops the turn and stops its agent run. */
static void cancel_worker(void *arg)
{
    onurb_turn_cancel((const char *)arg);
    free(arg);
    vTaskDelete(NULL);
}

static bool cancel_request(uint32_t thread)
{
    turn_job_t *job = NULL;
    uint32_t thread_id = 0;
    taskENTER_CRITICAL(&s_job_mux);
    char *csv = NULL;
    size_t csv_len = 0;
    int idx = -1;
    for (int i = s_njobs - 1; i >= 0; i--) {
        if (!s_jobs[i]->done && (thread == 0 || s_jobs[i]->thread == thread)) {
            idx = i;
            break;
        }
        if (thread == 0) {
            break;                               /* main X: only the newest */
        }
    }
    if (idx >= 0) {
        job = s_jobs[idx];
        memmove(&s_jobs[idx], &s_jobs[idx + 1], (size_t)(s_njobs - idx - 1) * sizeof(s_jobs[0]));
        s_njobs--;
        job->abandoned = true;                   /* the worker frees it */
        thread_id = job->thread;
        csv = job->gesture_csv;                  /* keep it for the "wrong gesture" tap */
        csv_len = job->gesture_len;
        job->gesture_csv = NULL;
        if (csv) {
            memcpy(s_fb_detail, job->gesture_detail, sizeof(s_fb_detail));
        }
    }
    taskEXIT_CRITICAL(&s_job_mux);
    if (job == NULL) {
        return false;
    }
    char id[33];
    memcpy(id, job->turn_id, sizeof(id));        /* job may be freed by its worker any time now */
    ESP_LOGI(TAG, "request #%d cancelled by the user", job->seq);
    {
        thread_t *th = thread_find(thread_id);
        if (th && th->tid[0]) {                  /* an answer inside an existing thread: keep it as it was */
            thread_restore(th);
        } else if (th) {
            thread_remove(th->id);
        }
    }
    threads_tick(esp_timer_get_time());          /* the thread leaves PROCESSING on screen now */
    if (id[0]) {                                 /* tell the gateway off the main loop (HTTPS can take seconds) */
        char *arg = malloc(sizeof(id));
        if (arg) {
            memcpy(arg, id, sizeof(id));
            if (xTaskCreatePinnedToCore(cancel_worker, "cancel", 8192, arg, 3, NULL, 1) != pdPASS) {
                free(arg);
            }
        }
    }
#if ONURB_UI_HAS_CANCELLED
    if (csv) {                                   /* red circle: tap again = the gesture was wrong */
        fb_discard();
        s_fb_csv = csv;
        s_fb_len = csv_len;
        onurb_ui_set_state(ONURB_UI_CANCELLED, 0);
        onurb_ui_cancel_taken();                 /* the cancelling tap doesn't count twice */
        s_ui_mode = UI_FEEDBACK;
        s_ui_until_us = esp_timer_get_time() + (int64_t)FEEDBACK_MS * 1000;
        return true;
    }
#endif
    free(csv);
    if (jobs_pending() > 0) {
        onurb_ui_set_state(ONURB_UI_PROCESSING, 0);   /* the X now applies to the next one */
    } else {
        notice("Cancelled", ONURB_UI_COLOR_BUSY, 1000);
    }
    return true;
}

/* Present the oldest finished request (keeps request order). */
static void present_next(void)
{
    turn_job_t *job = NULL;
    taskENTER_CRITICAL(&s_job_mux);
    if (s_njobs > 0 && s_jobs[0]->done) {
        job = s_jobs[0];
        memmove(&s_jobs[0], &s_jobs[1], (size_t)(--s_njobs) * sizeof(s_jobs[0]));
    }
    taskEXIT_CRITICAL(&s_job_mux);
    if (job == NULL) {
        return;
    }
    thread_t *th = job->thread ? thread_find(job->thread) : thread_find_tid(job->meta.thread);
    if (th == NULL && job->thread == 0 && job->reply) {   /* inbox result of a thread that's gone */
        th = thread_new((uint32_t)job->seq);
        strlcpy(th->heard, "Tarefa em segundo plano", sizeof(th->heard));
    }
    if (th && (job->spooled || job->spool_full || job->reply == NULL) && job->err == ESP_OK) {
        if (th->tid[0] == 0) {
            thread_remove(th->id);               /* never reached the agent / nothing heard */
        } else {
            thread_restore(th);                  /* an answer that didn't go through: as it was */
        }
    } else if (th && job->err != ESP_OK) {
        if (job->spooled || job->spool_full) {
            thread_remove(th->id);
        } else {
            strlcpy(th->reply, job->status ? "Erro no servidor" : "Sem conexão", sizeof(th->reply));
            th->state = TH_ERROR;
            th->done_at = esp_timer_get_time();
            s_threads_dirty = true;
        }
    } else if (th) {
        thread_finish(th, &job->meta, job->text);
    }
    if (job->spooled || job->spool_full) {
        notice(job->spooled ? "Offline - will send later" : "Offline - queue full",
               job->spooled ? ONURB_UI_COLOR_BUSY : ONURB_UI_COLOR_ERROR, 2000);
    } else if (job->err != ESP_OK) {
        char msg[40];
        snprintf(msg, sizeof(msg), job->status ? "Error: HTTP %d" : "Error: no connection", job->status);
        ESP_LOGE(TAG, "request #%d failed: %s (HTTP %d)", job->seq, esp_err_to_name(job->err), job->status);
        onurb_haptic(ONURB_HAPTIC_ERROR);
        notice(msg, ONURB_UI_COLOR_ERROR, 3000);
    } else if (job->reply == NULL) {             /* gateway heard nothing: no notice */
        ESP_LOGI(TAG, "request #%d: no speech - dismissed", job->seq);
        if (job->gesture_csv) {                  /* gesture-started, nothing for Onurb: offer the red X */
            fb_offer(job->gesture_csv, job->gesture_len, job->gesture_detail);
            job->gesture_csv = NULL;
        }
    } else {
        ESP_LOGI(TAG, "request #%d reply: %s", job->seq, job->text);
        /* a question / something waiting for the user buzzes stronger than a plain reply */
        onurb_haptic(th && th->state == TH_OPEN ? ONURB_HAPTIC_NOTIFY : ONURB_HAPTIC_REPLY);
#if ONURB_UI_HAS_REPLY && !ONURB_UI_HAS_THREADS
        onurb_ui_set_reply(job->text);           /* pushed on top of the history */
#endif
        if (!play_reply(job->reply, job->reply_len)) {
            ESP_LOGE(TAG, "request #%d: reply format unsupported", job->seq);
        }
        int ms = 3000 + 60 * (int)strlen(job->text);
        s_ui_mode = UI_REPLY;
        s_ui_until_us = esp_timer_get_time() + (int64_t)(ms > 12000 ? 12000 : ms) * 1000;
    }
    if (job->action_json) {                      /* the agent proposed a WhatsApp message */
        action_clear();                          /* a newer proposal replaces an unanswered one */
        strlcpy(s_action.id, job->action, sizeof(s_action.id));
        s_action.json = job->action_json;
        s_action.avatar = job->action_avatar;
        s_action.avatar_px = job->action_avatar_px;
        s_action.until_us = esp_timer_get_time() + ACTION_TTL_US;
        job->action_json = NULL;
        job->action_avatar = NULL;
        ESP_LOGI(TAG, "request #%d: WhatsApp preview %s", job->seq, s_action.id);
        s_ui_mode = UI_IDLE;                     /* pipeline_tick shows the preview right away */
    }
    free_job(job);
}

/* WhatsApp preview on screen (UI_ACTION). */
static void action_show(void)
{
#if ONURB_UI_HAS_WHATSAPP
    cJSON *j = cJSON_Parse(s_action.json);
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(j, "name"));
    cJSON *arr = cJSON_GetObjectItem(j, "messages");
    const char *msgs[8];
    int n = 0;
    for (cJSON *m = arr ? arr->child : NULL; m && n < 8; m = m->next) {
        if (cJSON_IsString(m)) {
            msgs[n++] = m->valuestring;
        }
    }
    const char *kind = cJSON_GetStringValue(cJSON_GetObjectItem(j, "kind"));
    if (kind && strcmp(kind, "purchase") == 0) {      /* photo, title, total + considerations */
        onurb_ui_show_purchase(name ? name : "?", (const uint16_t *)s_action.avatar, s_action.avatar_px, msgs, n);
    } else {
        onurb_ui_show_whatsapp(name ? name : "?", s_action.avatar_px == 96 ? (const uint16_t *)s_action.avatar : NULL,
                               msgs, n);
    }
    s_action.purchase = kind && strcmp(kind, "purchase") == 0;
    cJSON_Delete(j);
    s_ui_mode = UI_ACTION;
#endif
}

static void record_and_queue(stop_mode_t mode, const char *edit_of);

/* Gesture windows labelled accepted / wrong are training data: when the post fails (Wi-Fi
 * dozing or away), keep them (PSRAM) and retry once online. "rejected" ones are dropped:
 * there are ~100 a day and they would cost radio time. */
#define FB_QUEUE_MAX 8
static struct {
    char *csv;
    size_t len;
    char label[10];
    char detail[128];
} s_fbq[FB_QUEUE_MAX];
static int s_fbq_n;

static void gesture_post(const char *csv, size_t len, const char *label, const char *detail)
{
    if (csv == NULL || len == 0) {
        return;
    }
    bool online = onurb_wifi_level() >= 1;
    if (online && onurb_turn_post_gesture_feedback(csv, len, label, detail) == ESP_OK) {
        return;
    }
    if (strcmp(label, "rejected") == 0) {
        return;
    }
    if (s_fbq_n == FB_QUEUE_MAX) {               /* full: the oldest goes */
        heap_caps_free(s_fbq[0].csv);
        memmove(&s_fbq[0], &s_fbq[1], (FB_QUEUE_MAX - 1) * sizeof(s_fbq[0]));
        s_fbq_n--;
    }
    char *copy = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (copy == NULL) {
        return;
    }
    memcpy(copy, csv, len);
    s_fbq[s_fbq_n].csv = copy;
    s_fbq[s_fbq_n].len = len;
    strlcpy(s_fbq[s_fbq_n].label, label, sizeof(s_fbq[0].label));
    strlcpy(s_fbq[s_fbq_n].detail, detail ? detail : "", sizeof(s_fbq[0].detail));
    s_fbq_n++;
    ESP_LOGI(TAG, "gesture window (%s) queued for upload (%d waiting)", label, s_fbq_n);
}

static void gesture_queue_tick(int64_t now)
{
    static int64_t next_us;
    if (s_fbq_n == 0 || now < next_us || onurb_wifi_level() < 1 || s_uploading) {
        return;
    }
    next_us = now + 30LL * 1000000;
    if (onurb_turn_post_gesture_feedback(s_fbq[0].csv, s_fbq[0].len, s_fbq[0].label, s_fbq[0].detail) == ESP_OK) {
        heap_caps_free(s_fbq[0].csv);
        memmove(&s_fbq[0], &s_fbq[1], (FB_QUEUE_MAX - 1) * sizeof(s_fbq[0]));
        s_fbq_n--;
    }
}

/* Confirm / Cancel / Edit on the preview. */
static void action_tick(int64_t now)
{
#if ONURB_UI_HAS_WHATSAPP
    int taken = onurb_ui_action_taken();
    if (taken != 0 || now >= s_action.until_us) {
        onurb_ui_set_state(ONURB_UI_IDLE, 0);   /* closes the preview modal (set_status doesn't) */
    }
    if (taken == 1) {
        bool buy = s_action.purchase;
        onurb_ui_set_status(buy ? "Ordering..." : "Sending...", ONURB_UI_COLOR_BUSY);
        bool ok = onurb_action_post(s_action.id, "confirm") == ESP_OK;
        ESP_LOGI(TAG, "%s %s: %s", buy ? "purchase" : "WhatsApp", s_action.id,
                 ok ? "confirmed on the watch" : "confirm FAILED");
        action_clear();
        onurb_haptic(ok ? ONURB_HAPTIC_DOUBLE : ONURB_HAPTIC_ERROR);
        /* a purchase: the checkout runs in the background; its result arrives in the inbox */
        notice(ok ? (buy ? "Buying - result soon" : "Sent") : (buy ? "Order failed" : "Send failed"),
               ok ? ONURB_UI_COLOR_BUSY : ONURB_UI_COLOR_ERROR, buy ? 2500 : 1500);
    } else if (taken == 2 || now >= s_action.until_us) {
        bool buy = s_action.purchase;
        onurb_action_post(s_action.id, "cancel");
        ESP_LOGI(TAG, "%s %s: cancelled (%s)", buy ? "purchase" : "WhatsApp", s_action.id,
                 taken == 2 ? "user" : "timeout");
        action_clear();
        notice(buy ? "Not bought" : "Not sent", ONURB_UI_COLOR_BUSY, 1500);
    } else if (taken == 3) {
        char id[ONURB_ACTION_ID_LEN];
        strlcpy(id, s_action.id, sizeof(id));
        ESP_LOGI(TAG, "WhatsApp %s: edit by voice", id);
        action_clear();                          /* the agent sends a revised preview */
        onurb_ota_set_busy(true);
        record_and_queue(STOP_ON_SILENCE, id);
    }
#else
    (void)now;
#endif
}

/* Background-task results (gateway /v1/inbox) -> presented like a reply. */
static void inbox_worker(void *arg)
{
    turn_job_t *job = calloc(1, sizeof(*job));
    int status = 0;
    if (job && onurb_http_get("/v1/inbox", 2 * 1024 * 1024, &job->reply, &job->reply_len, &status,
                              job->text, sizeof(job->text), &job->meta) == ESP_OK && job->reply) {
        if (job->meta.action[0]) {               /* the agent proposed a WhatsApp message later */
            strlcpy(job->action, job->meta.action, sizeof(job->action));
            fetch_action(job);
        }
        job->done = true;
        taskENTER_CRITICAL(&s_job_mux);
        bool room = s_njobs < MAX_JOBS;
        if (room) {
            job->seq = s_next_seq++;
            s_jobs[s_njobs++] = job;
        }
        taskEXIT_CRITICAL(&s_job_mux);
        if (room) {
            ESP_LOGI(TAG, "inbox: background result #%d", job->seq);
            job = NULL;
        }
    }
    if (job) {
        free_job(job);
    }
    s_inbox_busy = false;
    vTaskDelete(NULL);
}

/* Idle-loop housekeeping for the pipeline: presents replies in order, the X,
 * reply/notice timeouts, screen and OTA "busy". */
static void pipeline_tick(void)
{
    int64_t now = esp_timer_get_time();

    /* A request whose worker never finished (no reply in 7 min, past every HTTP
     * timeout) would block every later one and keep the watch "busy" forever. */
    {
        turn_job_t *stale = NULL;
        taskENTER_CRITICAL(&s_job_mux);
        if (s_njobs > 0 && !s_jobs[0]->done && s_jobs[0]->created_us &&
            now - s_jobs[0]->created_us > 7LL * 60 * 1000000) {
            stale = s_jobs[0];
            memmove(&s_jobs[0], &s_jobs[1], (size_t)(--s_njobs) * sizeof(s_jobs[0]));
            stale->abandoned = true;             /* its worker frees it if it ever returns */
        }
        taskEXIT_CRITICAL(&s_job_mux);
        if (stale) {
            ESP_LOGE(TAG, "request #%d stuck for 7 min - dropped", stale->seq);
            thread_t *th = thread_find(stale->thread);
            if (th) {
                strlcpy(th->reply, "Sem resposta (tempo esgotado)", sizeof(th->reply));
                th->state = TH_ERROR;
                th->done_at = now;
                s_threads_dirty = true;
            }
        }
    }
    if (s_ui_mode != UI_REPLY) {
        present_next();
    }
    if (s_ui_mode == UI_PROCESSING) {
#if ONURB_UI_HAS_CANCEL
        if (onurb_ui_cancel_taken()) {
            cancel_request(0);
        }
#endif
    } else if (s_ui_mode == UI_ACTION) {
        action_tick(now);
    } else if (s_ui_mode == UI_FEEDBACK) {
#if ONURB_UI_HAS_CANCEL
        if (onurb_ui_cancel_taken() && s_fb_csv) {
            ESP_LOGI(TAG, "user: that gesture was wrong (%s)", s_fb_detail);
            gesture_post(s_fb_csv, s_fb_len, "wrong", s_fb_detail);
            fb_discard();
            notice("Marked as wrong gesture", ONURB_UI_COLOR_BUSY, 1500);
        } else
#endif
        if (now >= s_ui_until_us) {
            fb_discard();
            onurb_ui_set_state(ONURB_UI_IDLE, 0);
            s_ui_mode = UI_IDLE;                 /* back to PROCESSING below if others are pending */
        }
    }
    if ((s_ui_mode == UI_REPLY || s_ui_mode == UI_NOTICE) &&
        (now >= s_ui_until_us || (s_ui_mode == UI_REPLY && onurb_imu_end_motion() == ONURB_IMU_END_FLICK))) {
#if ONURB_UI_HAS_REPLY
        onurb_ui_set_reply(NULL);                /* hides the history view; history is kept */
#endif
        s_ui_mode = UI_IDLE;
    }
    threads_tick(now);
    gesture_queue_tick(now);
#if ONURB_UI_HAS_THREAD_CANCEL
    uint32_t cancel_th = onurb_ui_thread_cancel_taken();
    if (cancel_th) {
        thread_t *ct = thread_find(cancel_th);
        if (ct && ct->state == TH_OPEN) {        /* "waiting for you": the user closes it */
            ESP_LOGI(TAG, "thread %lu closed by the user (left unanswered)", (unsigned long)cancel_th);
            thread_remove(cancel_th);
        } else {
            ESP_LOGI(TAG, "cancel in thread %lu", (unsigned long)cancel_th);
            if (!cancel_request(cancel_th) && ct) {   /* no request behind it any more: just drop it */
                ESP_LOGW(TAG, "thread %lu had no request to cancel - removed", (unsigned long)cancel_th);
                thread_remove(cancel_th);
            }
        }
    }
#endif
#if ONURB_UI_HAS_THREADS
    uint32_t answer = onurb_ui_thread_reply_taken();
    /* Any thread the gateway knows (waiting, working task, done, error) takes a spoken addition. */
    if (answer && (s_ui_mode == UI_IDLE || s_ui_mode == UI_PROCESSING || s_ui_mode == UI_REPLY) &&
        thread_find(answer) && thread_find(answer)->tid[0] && thread_find(answer)->state != TH_PROCESSING) {
        ESP_LOGI(TAG, "answer by voice in thread %lu", (unsigned long)answer);
        s_reply_to = answer;
        onurb_ota_set_busy(true);
        record_and_queue(STOP_ON_SILENCE, NULL);
        s_reply_to = 0;                          /* dropped recording: thread stays as it was */
        return;
    }
#endif
    int pending = jobs_pending();
#if ONURB_UI_HAS_PENDING && !ONURB_UI_HAS_THREADS
    static int shown_pending = -1, shown_queued = -1;
    int queued = onurb_spool_count();            /* recorded offline, waiting for Wi-Fi */
    if (pending != shown_pending || queued != shown_queued) {   /* one side dash per request */
        shown_pending = pending;
        shown_queued = queued;
        onurb_ui_set_pending(pending, queued);
    }
#endif
    if (s_ui_mode == UI_IDLE && s_action.json && !s_uploading) {
        action_show();                           /* pending WhatsApp preview takes the screen */
    }
    if (s_ui_mode == UI_IDLE && pending == 0 && !s_uploading && !s_inbox_busy && now >= s_inbox_next_us &&
        onurb_wifi_level() >= 1) {
        s_inbox_next_us = now + 20LL * 1000000;  /* background results: poll every 20 s when idle */
        s_inbox_busy = true;
        if (xTaskCreatePinnedToCore(inbox_worker, "inbox", 8192, NULL, 3, NULL, 1) != pdPASS) {
            s_inbox_busy = false;
        }
    }
    if (s_ui_mode == UI_IDLE && pending > 0) {
        onurb_ui_set_state(ONURB_UI_PROCESSING, 0);
        s_ui_mode = UI_PROCESSING;
    } else if (s_ui_mode == UI_PROCESSING && pending == 0) {
        onurb_ui_set_state(ONURB_UI_IDLE, 0);
        s_ui_mode = UI_IDLE;
    } else if (s_ui_mode == UI_IDLE && pending == 0 && s_screen_held) {
        onurb_ui_set_state(ONURB_UI_IDLE, 0);
    }
    /* Waiting for an answer keeps the screen on only for PROCESSING_LIT_S after the
     * user asked (agent turns can take minutes; the stuck jobs at 19:00 held it lit
     * for hours). The reply lights it again (UI_REPLY) with a buzz. */
    bool waiting = pending > 0 || s_ui_mode == UI_PROCESSING;
    bool busy = waiting ? now < s_processing_lit_until_us : s_ui_mode != UI_IDLE;   /* incl. a WhatsApp preview */
    if (busy != s_screen_held) {
        s_screen_held = busy;
        if (busy) {
            onurb_screen_hold();
        } else {
            onurb_screen_release();
        }
    }
    onurb_ota_set_busy(busy || s_uploading);
}

/* Gesture-started recordings with practically no audio for the first 4 s
 * are dropped at once (a false trigger, or the user changed their mind);
 * once the user has spoken, 4 s of silence ends (and sends) the turn.
 * Measured on 60 recordings (2026-11-09): every real request had >= 0.82 s
 * of 10 ms chunks above -45 dBFS; ambient noise/music that whisper turned
 * into invented text ("Música Música", "I'm going to go.") had <= 0.1 s.
 * Hold-to-talk recordings below MIN_VOICED_CHUNKS are dropped too. */
#define NO_VOICE_FRAMES   (SAMPLE_RATE * 4)
#define MIN_VOICED_CHUNKS 40                     /* 0.4 s of voice (real requests: >= 0.82 s) */
#define VOICE_RMS         184                    /* -45 dBFS: ambient noise/music stayed below it */
#define END_SILENCE_FRAMES (SAMPLE_RATE * 4)     /* after speech, 4 s quiet ends the turn */

/* Any slot above speech level in this 10 ms chunk? (The echo-reference slot
 * is silent while recording; the start beep is skipped by the caller.) */
static bool chunk_has_voice(const int16_t *chunk)
{
    for (int s = 0; s < TDM_SLOTS; s++) {
        int64_t mean = 0, sq = 0;
        for (int f = 0; f < FRAMES_PER_READ; f++) {
            mean += chunk[f * TDM_SLOTS + s];
        }
        mean /= FRAMES_PER_READ;
        for (int f = 0; f < FRAMES_PER_READ; f++) {
            int d = chunk[f * TDM_SLOTS + s] - (int)mean;
            sq += (int64_t)d * d;
        }
        if (sq > (int64_t)VOICE_RMS * VOICE_RMS * FRAMES_PER_READ) {
            return true;
        }
    }
    return false;
}


#define MOTION_GRACE_FRAMES (SAMPLE_RATE * 12 / 10)  /* arm settles; first words */

/* Recording limit (Bruno, 2026-11-09: "um limite geral de um minuto e meio").
 * All 4 TDM slots are kept only for the first 3 s, to pick the mic slot;
 * after that only that slot goes into s_mono, a fixed 90 s buffer reserved at
 * boot (4.32 MB at 24 kHz), so every recording can really reach 90 s. At 90 s
 * the recording stops and is sent normally. */
#define MAX_RECORD_S      90
#define MAX_RECORD_FRAMES (SAMPLE_RATE * MAX_RECORD_S)
#define HEAD_FRAMES       (SAMPLE_RATE * 3)

static int16_t *s_mono;                          /* MAX_RECORD_FRAMES (or less if PSRAM is short) */
static int s_mono_frames;
static int16_t *s_head;                          /* HEAD_FRAMES x TDM_SLOTS */
static int16_t *s_scratch;                       /* one 10 ms TDM read, DMA-capable */

static void head_to_mono(int16_t *mono, int frames, int slot)
{
    for (int f = 0; f < frames; f++) {
        mono[f] = s_head[f * TDM_SLOTS + slot];
    }
}

static int s_last_voiced;                        /* voiced 10 ms chunks in the last recording */

/* Start cue, record until the stop condition (or PSRAM full), stop cue.
 * Returns the frame count with *out = the chosen mic slot as a malloc'd mono
 * buffer the caller owns, or -1 (gesture: no voice, dropped; *out = NULL). */
/* Recording timer on screen, off the audio loop: onurb_ui_set_state takes the
 * display lock and can wait for an LVGL render; the mic must keep being read. */
static volatile int s_rec_frames = -1;
static SemaphoreHandle_t s_rec_ui_mux;           /* no RECORDING redraw after the recording ended */

static void rec_ui_task(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_rec_ui_mux, portMAX_DELAY);
        int f = s_rec_frames;
        if (f >= 0) {
            onurb_ui_set_state(ONURB_UI_RECORDING, (float)f / SAMPLE_RATE);
        }
        xSemaphoreGive(s_rec_ui_mux);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void rec_ui_stop(void)
{
    xSemaphoreTake(s_rec_ui_mux, portMAX_DELAY);
    s_rec_frames = -1;
    xSemaphoreGive(s_rec_ui_mux);
}

static int record(stop_mode_t mode, int16_t **out)
{
    const char *why = "90 s limit";
    int frames = 0, slot = -1;

    *out = NULL;
    int max_frames = s_mono_frames;
    int16_t *mono = s_mono;

    onurb_haptic(ONURB_HAPTIC_REC_START);        /* felt cue: the speaker may be off */
    play_tone(880, 60);                          /* start cue, before the mic is read */
    mic_flush(s_scratch, 40);                    /* short: don't cut the first word */
    onurb_ui_rec_stop_taken();                   /* stale taps from before this recording */
    onurb_ui_rec_cancel_taken();
    ESP_LOGI(TAG, "recording (%s), limit %.0f s...",
             mode == STOP_ON_RELEASE ? "hold" : mode == STOP_ON_MOTION ? "gesture" : "edit",
             (double)max_frames / SAMPLE_RATE);
    onurb_ui_set_state(ONURB_UI_RECORDING, 0);
    int voiced = 0;                              /* 10 ms chunks with speech-level audio */
    s_last_voiced = 0;
    static slot_stats_t st;
    memset(&st, 0, sizeof(st));
    int64_t t_start = esp_timer_get_time();
    int64_t worst_us = 0, t_prev = t_start;
    s_rec_frames = 0;                            /* rec_ui_task shows the timer from this */
    int last_voice = 0;                          /* frame count at the last voiced chunk */
    while (frames + FRAMES_PER_READ <= max_frames) {
        if (mode != STOP_ON_RELEASE && frames >= NO_VOICE_FRAMES && voiced < MIN_VOICED_CHUNKS) {
            ESP_LOGI(TAG, "no voice in %d s - dismissed", NO_VOICE_FRAMES / SAMPLE_RATE);
            rec_ui_stop();
            return -1;                           /* silent: no stop cue, nothing sent */
        }
        if (mode != STOP_ON_RELEASE && voiced >= MIN_VOICED_CHUNKS &&
            frames - last_voice >= END_SILENCE_FRAMES) {
            why = "4 s of silence";
            frames = last_voice + SAMPLE_RATE / 2;   /* keep 0.5 s after the last word */
            break;
        }
        if (mode == STOP_ON_RELEASE && !button_pressed()) {
            why = "released";
            break;
        }
        if (onurb_ui_rec_cancel_taken()) {       /* X beside the red mic: discard */
            ESP_LOGI(TAG, "recording discarded by the user");
            rec_ui_stop();
            onurb_haptic(ONURB_HAPTIC_REC_STOP);
            return -1;
        }
        if (frames >= SAMPLE_RATE / 2 && onurb_ui_rec_stop_taken()) {   /* red mic tapped: send */
            why = "tap";
            break;
        }
        if (mode != STOP_ON_RELEASE && frames >= MOTION_GRACE_FRAMES) {
            int end = mode == STOP_ON_MOTION ? onurb_imu_end_motion() : ONURB_IMU_END_NONE;
            if (end != ONURB_IMU_END_NONE) {
                why = end == ONURB_IMU_END_LEFT_MOUTH ? "arm lowered" : "flick";
                break;
            }
            if (button_pressed()) {
                why = "BOOT";
                break;
            }
        }
        int16_t *chunk = frames < HEAD_FRAMES ? s_head + (size_t)frames * TDM_SLOTS : s_scratch;
        esp_codec_dev_read(s_mic, chunk, FRAMES_PER_READ * TDM_SLOTS * sizeof(int16_t));
        int64_t t_now = esp_timer_get_time();
        if (t_now - t_prev > worst_us) {
            worst_us = t_now - t_prev;                /* longest gap between reads */
        }
        t_prev = t_now;
        if (frames >= PICK_SKIP_FRAMES && chunk_has_voice(chunk)) {
            voiced++;
            last_voice = frames + FRAMES_PER_READ;
        }
        if (frames >= PICK_SKIP_FRAMES && frames < HEAD_FRAMES) {
            slot_stats_add(&st, chunk, FRAMES_PER_READ);
        }
        if (chunk == s_scratch) {
            for (int f = 0; f < FRAMES_PER_READ; f++) {
                mono[frames + f] = chunk[f * TDM_SLOTS + slot];
            }
        }
        frames += FRAMES_PER_READ;
        s_rec_frames = frames;
        if (frames == HEAD_FRAMES) {             /* enough audio to pick the mic slot */
            slot = slot_stats_pick(&st);
            head_to_mono(mono, HEAD_FRAMES, slot);
        }
    }
    rec_ui_stop();
    if (slot < 0) {                              /* shorter than the head */
        slot = st.frames > 0 ? slot_stats_pick(&st) : report_levels(s_head, frames > 0 ? frames : 1);
        head_to_mono(mono, frames, slot);
    }
    s_last_voiced = voiced;
    snprintf(s_rec_diag, sizeof(s_rec_diag), "audio=%.2f;wall=%.2f;gap_ms=%d", (double)frames / SAMPLE_RATE,
             (esp_timer_get_time() - t_start) / 1e6, (int)(worst_us / 1000));
    ESP_LOGI(TAG, "%s", s_rec_diag);
    ESP_LOGI(TAG, "stopped (%s) after %.1f s, %.2f s voiced, %s", why, (double)frames / SAMPLE_RATE,
             voiced / 100.0, SLOT_NAMES[slot]);
    play_tone(660, 120);                         /* stop cue, after capture */
    onurb_haptic(ONURB_HAPTIC_REC_STOP);         /* distinct from start: two ticks */
    *out = mono;
    return frames;
}

/* Mouth-pose entry. The trained raise-to-mouth gesture starts a turn for
 * Instinct; a medium-strong movement ends it. Every entry's IMU window is
 * still uploaded (accepted / rejected) so misses can be found and retrained. */
/* Record (after the previous upload freed the buffer) and hand the turn to
 * the pipeline; the main loop is free again right after. */
static void record_and_queue(stop_mode_t mode, const char *edit_of)
{
    strlcpy(s_rec_trigger, s_reply_to ? "thread" : mode == STOP_ON_MOTION ? "gesture" : "button",
            sizeof(s_rec_trigger));
    fb_discard();                                /* a new request ends the feedback window */
    s_last_job = NULL;
    if (!wait_upload_free()) {
        notice("Busy - try again", ONURB_UI_COLOR_ERROR, 2000);
        return;
    }
    int16_t *mono;
    int frames = record(mode, &mono);            /* -1 = gesture with no voice, dropped */
    int min_frames = mode != STOP_ON_RELEASE ? SAMPLE_RATE / 2 + MOTION_GRACE_FRAMES / 2 : SAMPLE_RATE / 4;
    if (frames >= min_frames && s_last_voiced < MIN_VOICED_CHUNKS) {
        ESP_LOGI(TAG, "only %.2f s of voice - dropped silently", s_last_voiced / 100.0);
        frames = -1;
    }
    if (frames >= min_frames) {
        start_turn(frames, edit_of);
    } else {
        onurb_ui_set_state(ONURB_UI_IDLE, 0);   /* pipeline_tick shows PROCESSING if others are pending */
        s_ui_mode = UI_IDLE;
    }
}

static void power_wake(const char *why);

static void gesture_turn(void)
{
    bool accepted;
    const char *detail, *csv;
    size_t len;

    if (!onurb_imu_gesture_take(&accepted, &detail)) {
        return;
    }
    if (accepted) {                              /* also while earlier requests process / a reply shows */
        power_wake("gesture");
        onurb_ota_set_busy(true);                /* a background update pauses for the turn */
        onurb_screen_hold();
        record_and_queue(STOP_ON_MOTION, NULL);
        onurb_screen_release();
        while (button_pressed()) {               /* BOOT used to stop: don't start a hold turn */
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    for (int i = 0; i < 100 && !onurb_imu_gesture_csv(&csv, &len); i++) {
        vTaskDelay(pdMS_TO_TICKS(20));           /* post-entry second still sampling */
    }
    if (onurb_imu_gesture_csv(&csv, &len)) {
        /* accepted but the recording was dropped (nobody spoke): a false trigger -> negative
         * example for training (Bruno, 2026-11-10) */
        /* accepted but dropped on the watch (no voice): kept as "timeout" (unlabelled); the red
         * X makes it a negative if the user taps it */
        gesture_post(csv, len, !accepted ? "rejected" : s_last_job ? "accepted" : "timeout", detail);
        if (accepted && s_last_job == NULL) {
            char *fb = malloc(len);
            if (fb) {
                memcpy(fb, csv, len);
                fb_offer(fb, len, detail);
            }
        }
        turn_job_t *job = accepted ? s_last_job : NULL;
        char *copy = job ? malloc(len) : NULL;
        if (copy) {                              /* kept with the request: cancel -> "wrong gesture" tap */
            memcpy(copy, csv, len);
            taskENTER_CRITICAL(&s_job_mux);
            bool live = false;
            for (int i = 0; i < s_njobs; i++) {
                live |= s_jobs[i] == job;
            }
            if (live && job->gesture_csv == NULL) {
                job->gesture_csv = copy;
                job->gesture_len = len;
                strlcpy(job->gesture_detail, detail, sizeof(job->gesture_detail));
                copy = NULL;
            }
            taskEXIT_CRITICAL(&s_job_mux);
            free(copy);
        }
    }
    s_last_job = NULL;
    onurb_imu_gesture_done();
}

/* Quick BOOT tap: countdown, then 2.5 s of IMU samples, labelled on the
 * green/red screen (gesture / other movement) and sent to the gateway. */
static void motion_capture(void)
{
    char buf[32];

    for (int c = 3; c > 0; c--) {
        snprintf(buf, sizeof(buf), "Get ready... %d", c);
        onurb_ui_set_status(buf, ONURB_UI_COLOR_BUSY);
        vTaskDelay(pdMS_TO_TICKS(700));
    }
    onurb_ui_set_status("Move now!", ONURB_UI_COLOR_BUSY);
    int id = onurb_imu_capture_start(2500);
    ESP_LOGI(TAG, "motion capture #%d started", id);
    while (onurb_imu_capture_busy()) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    /* Label it: green = the gesture, red = some other movement (negative example). */
    int answer = onurb_ui_feedback_ask(15000);
    if (answer < 0) {
        onurb_ui_set_status("Not labelled - discarded", ONURB_UI_COLOR_BUSY);
        vTaskDelay(pdMS_TO_TICKS(1000));
        return;
    }
    size_t len;
    const char *csv = onurb_imu_capture_csv(&len);
    bool sent = onurb_turn_post_imu(csv, len, id, answer == 1 ? "gesture" : "other") == ESP_OK;
    snprintf(buf, sizeof(buf), sent ? "Captured #%d" : "Saved #%d (no Wi-Fi)", id);
    onurb_ui_set_status(buf, ONURB_UI_COLOR_BUSY);
    vTaskDelay(pdMS_TO_TICKS(1000));
}

/* ---- Power (Bruno, 2026-11-10) ---------------------------------------------
 * 1. Dynamic frequency scaling (esp_pm): 80-240 MHz, drivers raise it when busy.
 * 2. Doze: after DOZE_AFTER_S with the screen off and nothing going on (and not
 *    on USB), the Wi-Fi radio goes off. Wrist motion, the button, a gesture or
 *    anything holding the screen wakes it (Wi-Fi back in ~1-2 s; a recording
 *    started meanwhile uploads when it's back). Every DOZE_PEEK_S it wakes
 *    briefly anyway, to fetch background results and report the battery.
 * Battery/doze telemetry rides on the update check (gateway battery.csv). */
#define DOZE_AFTER_S   120
#define DOZE_PEEK_S    (10 * 60)
#define PEEK_LEN_S     45
#define WAKE_DPS       200.0f                   /* a quick raise (gestures peak 300-800 dps); not arm swing */
static int64_t s_active_us;                      /* last activity */
static int64_t s_doze_since_us;
static bool s_peeking;

static void power_wake(const char *why)
{
    s_active_us = esp_timer_get_time();
    s_peeking = false;
    if (onurb_wifi_dozing()) {
        ESP_LOGI(TAG, "wake (%s) after %lld s of doze", why, (long long)((s_active_us - s_doze_since_us) / 1000000));
        onurb_wifi_doze(false);
        onurb_ota_check_soon();
    }
}

static void power_tick(void)
{
    int64_t now = esp_timer_get_time();
    onurb_pmu_status_t st = {0};
    bool usb = onurb_pmu_get_status(&st) == ESP_OK && st.usb_in;
    bool busy = s_screen_held || onurb_screen_is_on() || jobs_pending() > 0 || s_uploading || s_ui_mode != UI_IDLE ||
                s_action.json != NULL || onurb_spool_busy() || s_inbox_busy;
    if (busy || usb) {
        s_active_us = now;
    }
    if (onurb_wifi_dozing()) {
        /* No wake on plain wrist motion: on the arm it fired all the time and every
         * wake meant a reconnect plus 2 more minutes of radio. A gesture or a button
         * turn wakes it (the reconnect overlaps the recording); queued offline
         * recordings go out on the next peek or turn. */
        if (busy) {
            power_wake("activity");
        } else if (now - s_doze_since_us >= (int64_t)DOZE_PEEK_S * 1000000) {
            ESP_LOGI(TAG, "doze peek: inbox");   /* update checks stay hourly (onurb_ota_tick) */
            onurb_wifi_doze(false);
            s_inbox_next_us = 0;
            s_peeking = true;
            s_active_us = now - (int64_t)(DOZE_AFTER_S - PEEK_LEN_S) * 1000000;   /* back to doze after the peek */
        }
        return;
    }
    if (now - s_active_us >= (int64_t)DOZE_AFTER_S * 1000000 && !onurb_ota_busy_downloading()) {
        s_doze_since_us = now;
        onurb_wifi_doze(true);
    }
}

static void power_report(void)
{
    onurb_pmu_status_t st;
    static char buf[160];      /* main task: keep big buffers off its stack */
    if (onurb_pmu_get_status(&st) == ESP_OK) {
        static char scr[64];
        onurb_screen_stats(scr, sizeof(scr));
        /* internal RAM: task stacks and Wi-Fi need it; ran out on 2026-11-11 */
        unsigned iram = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024;
        unsigned iblk = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024;
        ESP_LOGD(TAG, "internal RAM free %u KB, largest block %u KB", iram, iblk);
        snprintf(buf, sizeof(buf), "batt=%d;mv=%d;chg=%d;usb=%d;jobs=%d;spool=%d;%s;iram_kb=%u;iblk_kb=%u",
                 st.battery ? st.percent : -1, st.millivolts, st.charging, st.usb_in, jobs_pending(),
                 onurb_spool_count(), scr, iram, iblk);
        onurb_ota_set_power_status(buf);
    }
}

void app_main(void)
{
#if CONFIG_PM_ENABLE
    esp_pm_config_t pm = {.max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, .min_freq_mhz = 80,
                          .light_sleep_enable = false};
    ESP_LOGI(TAG, "power: DFS %d-%d MHz (%s)", pm.min_freq_mhz, pm.max_freq_mhz,
             esp_err_to_name(esp_pm_configure(&pm)));
#endif
    log_system_info();
    onurb_ota_boot_info();
    ESP_ERROR_CHECK(bsp_i2c_init());
    if (onurb_pmu_init() != ESP_OK) {
        ESP_LOGW(TAG, "PMU init failed - charging left at hardware defaults");
    }
    onurb_pmu_log_status();
    if (onurb_ui_start() != ESP_OK) {
        ESP_LOGW(TAG, "screen unavailable - continuing headless");
    }
    onurb_wifi_start();
    ui_refresh();
    i2c_scan();
    onurb_imu_start();
    onurb_spool_start();
    audio_init();
    button_init();
    onurb_haptic_init();

    ESP_LOGI(TAG, "B-05: 1 kHz test tone");
    play_tone(1000, 800);

    s_head = heap_caps_malloc((size_t)HEAD_FRAMES * TDM_SLOTS * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    s_rec_ui_mux = xSemaphoreCreateMutex();
    xTaskCreate(rec_ui_task, "rec_ui", 3072, NULL, 2, NULL);
    s_scratch = heap_caps_malloc(FRAMES_PER_READ * TDM_SLOTS * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (s_head == NULL || s_scratch == NULL) {
        ESP_LOGE(TAG, "buffer allocation failed");
        abort();
    }
    s_mono_frames = MAX_RECORD_FRAMES;
    s_mono = heap_caps_malloc((size_t)s_mono_frames * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (s_mono == NULL) {                        /* shouldn't happen (8 MB PSRAM); say so loudly */
        size_t room = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
        s_mono_frames = (int)((room > 2 * 1024 * 1024 ? room - 2 * 1024 * 1024 : room / 2) / sizeof(int16_t));
        s_mono_frames = s_mono_frames / FRAMES_PER_READ * FRAMES_PER_READ;
        s_mono = heap_caps_malloc((size_t)s_mono_frames * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        ESP_LOGE(TAG, "PSRAM can't hold %d s: recording limited to %.0f s", MAX_RECORD_S,
                 (double)s_mono_frames / SAMPLE_RATE);
        if (s_mono == NULL) {
            abort();
        }
    }
    onurb_ota_report_record_limit(s_mono_frames / SAMPLE_RATE,
                                  heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
    ESP_LOGI(TAG, "ready: hold BOOT to talk; recording limit %d s, PSRAM free after buffers %u KB",
             s_mono_frames / SAMPLE_RATE, (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

    int idle_ticks = 0;
    while (true) {
        if (!button_pressed()) {
            vTaskDelay(pdMS_TO_TICKS(10));
            gesture_turn();
            pipeline_tick();
            onurb_ota_tick();
            /* Main view (the circle): the usual short timeout. Threads, previews, the Wi-Fi
             * picker: 1 min, restarted by any touch (Bruno, 2026-11-11). */
            onurb_screen_set_idle_s(onurb_ui_main_view() ? CONFIG_ONURB_SCREEN_IDLE_S : SCREEN_DETAIL_IDLE_S);
            onurb_screen_tick();
            static bool screen_was_on;
            if (onurb_screen_is_on() && !screen_was_on) {
                onurb_ota_check_soon();          /* opened: check for an update (rate-limited) */
            }
            screen_was_on = onurb_screen_is_on();
            power_tick();
            if (++idle_ticks % 200 == 0) {       /* screen every 2 s */
                ui_refresh();
                power_report();
            }
            if (idle_ticks >= 3000) {            /* logs every 30 s */
                idle_ticks = 0;
                onurb_pmu_log_status();
                onurb_wifi_log_status();
            }
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(30));          /* debounce */
        if (!button_pressed()) {
            continue;
        }
        power_wake("button");
        onurb_ota_set_busy(true);                /* a background update pauses for the turn */
        onurb_screen_hold();                     /* on for the recording / capture */
        vTaskDelay(pdMS_TO_TICKS(200));
        if (!button_pressed() && jobs_pending() == 0 && s_ui_mode == UI_IDLE) {
            motion_capture();                    /* quick tap while idle = labelled IMU trace */
            onurb_ui_set_state(ONURB_UI_IDLE, 0);
        } else if (button_pressed()) {
            record_and_queue(STOP_ON_RELEASE, NULL);   /* hold to talk, also while others process */
        }
        onurb_screen_release();
        while (button_pressed()) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}
