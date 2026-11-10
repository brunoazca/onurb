#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include "bsp/esp-bsp.h"

#include "screen.h"

static const char *TAG = "onurb_screen";

/* Held from the main loop and the background OTA task. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static int s_holds;
static bool s_on = true;                 /* the UI turns the panel on at boot */
static int64_t s_idle_since_us;

static void set_on(bool on)
{
    if (on == s_on) {
        return;
    }
    bsp_display_brightness_set(on ? CONFIG_ONURB_SCREEN_BRIGHTNESS : 0);
    s_on = on;
    ESP_LOGI(TAG, "screen %s", on ? "on" : "off");
}

void onurb_screen_hold(void)
{
    taskENTER_CRITICAL(&s_mux);
    s_holds++;
    taskEXIT_CRITICAL(&s_mux);
    set_on(true);
}

void onurb_screen_release(void)
{
    taskENTER_CRITICAL(&s_mux);
    if (s_holds > 0) {
        s_holds--;
    }
    s_idle_since_us = esp_timer_get_time();
    taskEXIT_CRITICAL(&s_mux);
}

void onurb_screen_tick(void)
{
    if (s_on && s_holds == 0 &&
        esp_timer_get_time() - s_idle_since_us >= (int64_t)CONFIG_ONURB_SCREEN_IDLE_S * 1000000) {
        set_on(false);
    }
}
