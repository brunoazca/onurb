#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "haptic.h"

#define MOTOR_GPIO GPIO_NUM_18      /* factory firmware: "Drive motor GPIO18" */

static const char *TAG = "onurb_haptic";

/* on/off durations in ms, 0-terminated */
static const uint16_t PATTERNS[][6] = {
    [ONURB_HAPTIC_TAP]    = {35, 0},
    [ONURB_HAPTIC_NOTIFY] = {90, 0},
    [ONURB_HAPTIC_DOUBLE] = {45, 90, 45, 0},
    [ONURB_HAPTIC_ERROR]  = {300, 0},
    [ONURB_HAPTIC_REC_START] = {20, 0},          /* one faint tick */
    [ONURB_HAPTIC_REC_STOP]  = {15, 70, 15, 0},  /* two faint ticks */
    [ONURB_HAPTIC_REPLY]     = {15, 60, 15, 60, 15, 0}, /* three faint ticks */
};

static esp_timer_handle_t s_timer;
static const uint16_t *s_step;
static bool s_on;
static bool s_ready;

static void step(void *arg)
{
    if (s_step == NULL || *s_step == 0) {
        gpio_set_level(MOTOR_GPIO, 0);
        s_on = false;
        s_step = NULL;
        return;
    }
    s_on = !s_on;
    gpio_set_level(MOTOR_GPIO, s_on);
    esp_timer_start_once(s_timer, (uint64_t)(*s_step++) * 1000);
}

void onurb_haptic_init(void)
{
    gpio_config_t cfg = {.pin_bit_mask = 1ULL << MOTOR_GPIO, .mode = GPIO_MODE_OUTPUT};
    if (gpio_config(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "motor GPIO init failed");
        return;
    }
    gpio_set_level(MOTOR_GPIO, 0);
    esp_timer_create_args_t t = {.callback = step, .name = "haptic"};
    s_ready = esp_timer_create(&t, &s_timer) == ESP_OK;
}

void onurb_haptic(onurb_haptic_t pattern)
{
    if (!s_ready || pattern < 0 || pattern >= ONURB_HAPTIC_COUNT) {
        return;
    }
    esp_timer_stop(s_timer);             /* a new pattern replaces a running one */
    gpio_set_level(MOTOR_GPIO, 0);
    s_on = false;
    s_step = PATTERNS[pattern];
    step(NULL);
}
