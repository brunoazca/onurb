#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include "bsp/esp-bsp.h"
#include "lvgl.h"

#include "screen.h"

static const char *TAG = "onurb_screen";

/* Held from the main loop and the background OTA task. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static int s_holds;
static bool s_on = true;                 /* the UI turns the panel on at boot */
static int64_t s_idle_since_us;

/* While the panel is dark, a transparent layer above everything takes the
 * touches: a stray tap can't press a hidden button (cancel X, mic, Wi-Fi), and
 * a double tap turns the screen on (Bruno, 2026-11-10).
 *
 * The screen also lit up with nobody tapping: skin or a sleeve resting on the
 * glass makes the touch controller flicker press/release, which looked like a
 * double tap. A tap now counts only when it is short (TAP_MAX_MS) and the two
 * taps are a real double tap: a gap of TAP_GAP_MIN_MS..DOUBLE_TAP_MS and close
 * together on the glass. Rejected ones are counted (onurb_screen_stats). */
#define DOUBLE_TAP_MS 450
#define TAP_MAX_MS 250
#define TAP_GAP_MIN_MS 60
#define TAP_NEAR_PX 70
static lv_obj_t *s_shield;
static int64_t s_last_tap_us;            /* release of the last valid single tap */
static int64_t s_press_us;
static lv_point_t s_press_pt, s_last_tap_pt;
static int s_n_dtap, s_n_rejected, s_n_held_on;

static void set_on(bool on);

static void shield_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    int64_t now = esp_timer_get_time();
    lv_indev_t *indev = lv_indev_active();
    lv_point_t pt = {0, 0};
    if (indev) {
        lv_indev_get_point(indev, &pt);
    }
    if (code == LV_EVENT_PRESSED) {
        s_press_us = now;
        s_press_pt = pt;
        return;
    }
    if (code != LV_EVENT_RELEASED || s_press_us == 0) {
        return;
    }
    int64_t held = now - s_press_us;
    s_press_us = 0;
    if (held > (int64_t)TAP_MAX_MS * 1000) {     /* resting contact, not a tap */
        s_last_tap_us = 0;
        s_n_rejected++;
        return;
    }
    int64_t gap = now - held - s_last_tap_us;   /* last release -> this press */
    int dx = s_press_pt.x - s_last_tap_pt.x, dy = s_press_pt.y - s_last_tap_pt.y;
    if (s_last_tap_us != 0 && gap >= (int64_t)TAP_GAP_MIN_MS * 1000 && gap <= (int64_t)DOUBLE_TAP_MS * 1000 &&
        dx * dx + dy * dy <= TAP_NEAR_PX * TAP_NEAR_PX) {
        s_last_tap_us = 0;
        s_n_dtap++;
        taskENTER_CRITICAL(&s_mux);
        s_idle_since_us = now;               /* the usual idle countdown from now */
        taskEXIT_CRITICAL(&s_mux);
        ESP_LOGI(TAG, "double tap");
        set_on(true);
        return;
    }
    if (s_last_tap_us != 0 && gap < (int64_t)TAP_GAP_MIN_MS * 1000) {
        s_n_rejected++;                      /* contact flicker */
    }
    s_last_tap_us = now;
    s_last_tap_pt = s_press_pt;
}

void onurb_screen_stats(char *buf, int len)
{
    snprintf(buf, len, "scr_dtap=%d;scr_rej=%d;scr_hold=%d", s_n_dtap, s_n_rejected, s_n_held_on);
}

static void shield_set(bool dark)
{
    if (!bsp_display_lock(0)) {
        return;
    }
    if (s_shield == NULL && dark) {
        s_shield = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(s_shield);
        lv_obj_set_size(s_shield, LV_PCT(100), LV_PCT(100));
        lv_obj_set_clickable(s_shield, true);
        lv_obj_set_scrollable(s_shield, false);
        lv_obj_add_event_cb(s_shield, shield_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(s_shield, shield_cb, LV_EVENT_RELEASED, NULL);
    }
    if (s_shield) {
        if (dark) {
            lv_obj_set_hidden(s_shield, false);
            lv_obj_move_foreground(s_shield);
        } else {
            lv_obj_set_hidden(s_shield, true);
        }
    }
    bsp_display_unlock();
}

static void set_on(bool on)
{
    if (on == s_on) {
        return;
    }
    if (on) {
        shield_set(false);
    }
    bsp_display_brightness_set(on ? CONFIG_ONURB_SCREEN_BRIGHTNESS : 0);
    s_on = on;
    if (!on) {
        s_last_tap_us = 0;
        shield_set(true);
    }
    ESP_LOGI(TAG, "screen %s", on ? "on" : "off");
}

bool onurb_screen_is_on(void)
{
    return s_on;
}

void onurb_screen_hold(void)
{
    taskENTER_CRITICAL(&s_mux);
    s_holds++;
    taskEXIT_CRITICAL(&s_mux);
    if (!s_on) {
        s_n_held_on++;                       /* lit by the firmware, not by a tap */
    }
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

static volatile int s_idle_s = CONFIG_ONURB_SCREEN_IDLE_S;

void onurb_screen_set_idle_s(int seconds)
{
    s_idle_s = seconds > 0 ? seconds : CONFIG_ONURB_SCREEN_IDLE_S;
}

void onurb_screen_touch(void)
{
    if (s_on) {
        taskENTER_CRITICAL(&s_mux);
        s_idle_since_us = esp_timer_get_time();
        taskEXIT_CRITICAL(&s_mux);
    }
}

void onurb_screen_tick(void)
{
    if (s_on && s_holds == 0 && esp_timer_get_time() - s_idle_since_us >= (int64_t)s_idle_s * 1000000) {
        set_on(false);
    }
}
