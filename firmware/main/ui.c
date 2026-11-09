/*
 * Minimal status screen (SPEC §8.4 / §10.4 groundwork): black background for
 * the AMOLED, battery and Wi-Fi lines, a status circle that turns red while
 * recording, and a one-line hint. All LVGL calls hold the BSP display lock.
 */
#include <stdio.h>

#include "esp_log.h"
#include "lvgl.h"

#include "bsp/esp-bsp.h"

#include "ui.h"

static const char *TAG = "onurb_ui";

#define COLOR_IDLE      0x3A3A3A
#define COLOR_RECORDING 0xE53935
#define COLOR_BUSY      0xFFB300

static lv_obj_t *s_battery;
static lv_obj_t *s_wifi;
static lv_obj_t *s_dot;
static lv_obj_t *s_status;
static lv_obj_t *s_hint;
static bool s_ready;

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color,
                            lv_align_t align, int32_t y)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(label, align, 0, y);
    lv_label_set_text(label, "");
    return label;
}

esp_err_t onurb_ui_start(void)
{
    if (bsp_display_start() == NULL) {
        ESP_LOGE(TAG, "display start failed");
        return ESP_FAIL;
    }
    bsp_display_brightness_set(CONFIG_ONURB_SCREEN_BRIGHTNESS);

    bsp_display_lock(0);
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *title = make_label(scr, &lv_font_montserrat_40, 0xFFFFFF, LV_ALIGN_TOP_MID, 40);
    lv_label_set_text(title, "Onurb");

    s_battery = make_label(scr, &lv_font_montserrat_20, 0xB0B0B0, LV_ALIGN_TOP_MID, 100);
    s_wifi = make_label(scr, &lv_font_montserrat_20, 0xB0B0B0, LV_ALIGN_TOP_MID, 130);

    s_dot = lv_obj_create(scr);
    lv_obj_set_size(s_dot, 140, 140);
    lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_dot, 0, 0);
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(COLOR_IDLE), 0);
    lv_obj_set_scrollable(s_dot, false);
    lv_obj_align(s_dot, LV_ALIGN_CENTER, 0, 30);

    s_status = make_label(scr, &lv_font_montserrat_28, 0xFFFFFF, LV_ALIGN_BOTTOM_MID, -100);
    s_hint = make_label(scr, &lv_font_montserrat_20, 0x808080, LV_ALIGN_BOTTOM_MID, -60);
    lv_label_set_text(s_status, "Ready");
    lv_label_set_text(s_hint, "Hold BOOT to record");
    bsp_display_unlock();

    s_ready = true;
    ESP_LOGI(TAG, "screen on (brightness %d%%)", CONFIG_ONURB_SCREEN_BRIGHTNESS);
    return ESP_OK;
}

void onurb_ui_set_battery(int percent, int millivolts, bool charging, bool usb_in)
{
    char buf[64];

    if (!s_ready) {
        return;
    }
    if (percent < 0) {
        snprintf(buf, sizeof(buf), "Battery: not detected");
    } else {
        snprintf(buf, sizeof(buf), "Battery %d%%  %.2f V%s", percent, millivolts / 1000.0,
                 charging ? "  charging" : (usb_in ? "  USB" : ""));
    }
    bsp_display_lock(0);
    lv_label_set_text(s_battery, buf);
    bsp_display_unlock();
}

void onurb_ui_set_wifi(const char *text)
{
    if (!s_ready) {
        return;
    }
    bsp_display_lock(0);
    lv_label_set_text(s_wifi, text);
    bsp_display_unlock();
}

void onurb_ui_set_state(onurb_ui_state_t state, float seconds)
{
    char buf[48];
    uint32_t color = COLOR_IDLE;

    if (!s_ready) {
        return;
    }
    switch (state) {
    case ONURB_UI_RECORDING:
        snprintf(buf, sizeof(buf), "Recording %.1f s", seconds);
        color = COLOR_RECORDING;
        break;
    case ONURB_UI_PLAYING:
        snprintf(buf, sizeof(buf), "Playing back");
        color = COLOR_BUSY;
        break;
    case ONURB_UI_IDLE:
    default:
        snprintf(buf, sizeof(buf), "Ready");
        break;
    }
    bsp_display_lock(0);
    lv_label_set_text(s_status, buf);
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(color), 0);
    bsp_display_unlock();
}
