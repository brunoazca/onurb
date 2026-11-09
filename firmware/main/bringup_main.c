/*
 * Onurb hardware bring-up (SPEC §16.2, tests B-01, B-02, B-04, B-05 + a first
 * hold-to-record -> play loop).
 *
 * 0. Configures the AXP2101 charger and logs battery state (B-06), again every 30 s.
 * 1. Logs chip/flash/PSRAM info, starts Wi-Fi (auto-reconnect + SNTP check).
 * 2. Scans the BSP I2C bus and names the devices we expect (B-04).
 * 3. Brings audio up with the BSP 24 kHz voice profile BEFORE creating codec
 *    devices; otherwise the BSP silently falls back to its legacy 22.05 kHz
 *    mono mode with no ES7210 mics selected (SPEC §6.1, trap 1) (B-01).
 * 4. Plays a 1 kHz test tone (B-05).
 * 5. Loop: hold BOOT to record (up to CONFIG_ONURB_BRINGUP_MAX_RECORD_S),
 *    release to get per-TDM-slot levels (B-02) and hear the loudest slot.
 */
#include <inttypes.h>
#include <math.h>
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

#include "bsp/esp-bsp.h"

#include "pmu.h"
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

static void report_and_play(const int16_t *rec, int frames)
{
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

    ESP_LOGI(TAG, "playing back %s (loudest)", SLOT_NAMES[loudest]);
    int16_t out[FRAMES_PER_READ * 2];
    for (int f = 0; f < frames; f += FRAMES_PER_READ) {
        int n = frames - f < FRAMES_PER_READ ? frames - f : FRAMES_PER_READ;
        for (int i = 0; i < n; i++) {
            int16_t v = rec[(f + i) * TDM_SLOTS + loudest];
            out[2 * i] = v;
            out[2 * i + 1] = v;
        }
        esp_codec_dev_write(s_spk, out, n * 2 * sizeof(int16_t));
    }
}

void app_main(void)
{
    log_system_info();
    ESP_ERROR_CHECK(bsp_i2c_init());
    if (onurb_pmu_init() != ESP_OK) {
        ESP_LOGW(TAG, "PMU init failed - charging left at hardware defaults");
    }
    onurb_pmu_log_status();
    onurb_wifi_start();
    i2c_scan();
    audio_init();
    button_init();

    ESP_LOGI(TAG, "B-05: 1 kHz test tone");
    play_tone(1000, 800);

    const int max_frames = SAMPLE_RATE * CONFIG_ONURB_BRINGUP_MAX_RECORD_S;
    int16_t *rec = heap_caps_malloc((size_t)max_frames * TDM_SLOTS * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    int16_t *scratch = heap_caps_malloc(FRAMES_PER_READ * TDM_SLOTS * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (rec == NULL || scratch == NULL) {
        ESP_LOGE(TAG, "buffer allocation failed");
        abort();
    }

    ESP_LOGI(TAG, "ready: hold BOOT to record (max %d s), release to analyse + play back",
             CONFIG_ONURB_BRINGUP_MAX_RECORD_S);

    int idle_ticks = 0;
    while (true) {
        if (!button_pressed()) {
            vTaskDelay(pdMS_TO_TICKS(10));
            if (++idle_ticks >= 3000) {          /* every 30 s */
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

        play_tone(880, 120);                     /* start cue, before the mic is read */
        mic_flush(scratch, 200);
        ESP_LOGI(TAG, "recording...");

        int frames = 0;
        while (button_pressed() && frames + FRAMES_PER_READ <= max_frames) {
            esp_codec_dev_read(s_mic, rec + (size_t)frames * TDM_SLOTS,
                               FRAMES_PER_READ * TDM_SLOTS * sizeof(int16_t));
            frames += FRAMES_PER_READ;
        }
        ESP_LOGI(TAG, "stopped (%s)", button_pressed() ? "max length" : "released");
        play_tone(660, 120);                     /* stop cue, after capture */

        if (frames < SAMPLE_RATE / 4) {
            ESP_LOGW(TAG, "too short, ignored");
        } else {
            report_and_play(rec, frames);
        }
        while (button_pressed()) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}
