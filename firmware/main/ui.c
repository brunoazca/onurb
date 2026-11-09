/*
 * Minimal status screen (SPEC §8.4 / §10.4 groundwork): black background for
 * the AMOLED, an Apple-style battery glyph top-right, a Wi-Fi line, a status circle that turns white and ripples while
 * recording, and a one-line hint. All LVGL calls hold the BSP display lock.
 */
#include <stdio.h>

#include "esp_log.h"
#include "lvgl.h"

#include "bsp/esp-bsp.h"

#include "ui.h"

static const char *TAG = "onurb_ui";

/* Monochrome palette: everything is black, white or a gray. */
#define COLOR_IDLE      0x3A3A3A
#define COLOR_RECORDING 0xFFFFFF
#define COLOR_BUSY      ONURB_UI_COLOR_BUSY
#define COLOR_BATT      0xFFFFFF
#define COLOR_BATT_CHG  0x8E8E93 /* gray fill so the white bolt stays legible */
#define COLOR_BATT_RIM  0x8E8E93

/* Recording ripple: rings grow out of the circle and fade. */
#define DOT_SIZE        140
#define RING_MAX        230
#define RING_COUNT      2
#define RING_PERIOD_MS  1400

/* Battery glyph geometry (outer body; the nub sits outside on the right). */
#define BATT_W      46
#define BATT_H      22
#define BATT_BORDER 2
#define BATT_PAD    2
#define BATT_INNER_W (BATT_W - 2 * (BATT_BORDER + BATT_PAD))
#define BATT_INNER_H (BATT_H - 2 * (BATT_BORDER + BATT_PAD))

static lv_obj_t *s_batt_body;
static lv_obj_t *s_batt_fill;
static lv_obj_t *s_batt_bolt;
static lv_obj_t *s_batt_pct;
static lv_obj_t *s_wifi;
static lv_obj_t *s_motion;
static lv_obj_t *s_dot;
static lv_obj_t *s_rings[RING_COUNT];
static bool s_rippling;
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

static lv_obj_t *make_box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_scrollable(o, false);
    lv_obj_set_clickable(o, false);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    return o;
}

/* Status-bar battery: percentage text, rounded outline with a fill bar sized
 * to the charge level, a nub, and a bolt overlay while on USB power. Inset
 * from the corner because the 2.06" panel has rounded corners. */
static void make_battery(lv_obj_t *scr)
{
    s_batt_body = make_box(scr);
    lv_obj_set_size(s_batt_body, BATT_W, BATT_H);
    lv_obj_set_style_radius(s_batt_body, 6, 0);
    lv_obj_set_style_bg_opa(s_batt_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_batt_body, BATT_BORDER, 0);
    lv_obj_set_style_border_color(s_batt_body, lv_color_hex(COLOR_BATT_RIM), 0);
    lv_obj_set_style_pad_all(s_batt_body, BATT_PAD, 0);
    lv_obj_align(s_batt_body, LV_ALIGN_TOP_RIGHT, -44, 22);

    lv_obj_t *nub = make_box(scr);
    lv_obj_set_size(nub, 3, 8);
    lv_obj_set_style_radius(nub, 1, 0);
    lv_obj_set_style_bg_color(nub, lv_color_hex(COLOR_BATT_RIM), 0);
    lv_obj_align_to(nub, s_batt_body, LV_ALIGN_OUT_RIGHT_MID, 1, 0);

    s_batt_fill = make_box(s_batt_body);
    lv_obj_set_size(s_batt_fill, 0, BATT_INNER_H);
    lv_obj_set_style_radius(s_batt_fill, 3, 0);
    lv_obj_set_style_bg_color(s_batt_fill, lv_color_hex(COLOR_BATT), 0);
    lv_obj_align(s_batt_fill, LV_ALIGN_LEFT_MID, 0, 0);

    s_batt_bolt = lv_label_create(s_batt_body);
    lv_obj_set_style_text_font(s_batt_bolt, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_batt_bolt, lv_color_white(), 0);
    lv_label_set_text(s_batt_bolt, LV_SYMBOL_CHARGE);
    lv_obj_center(s_batt_bolt);
    lv_obj_set_hidden(s_batt_bolt, true);

    s_batt_pct = lv_label_create(scr);
    lv_obj_set_style_text_font(s_batt_pct, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_batt_pct, lv_color_white(), 0);
    lv_label_set_text(s_batt_pct, "");
    lv_obj_align_to(s_batt_pct, s_batt_body, LV_ALIGN_OUT_LEFT_MID, -6, 0);
}

static void ring_size_cb(void *obj, int32_t v)
{
    lv_obj_set_size(obj, v, v);
}

static void ring_opa_cb(void *obj, int32_t v)
{
    lv_obj_set_style_border_opa(obj, (lv_opa_t)v, 0);
}

/* Caller holds the display lock. */
static void ripple_set(bool on)
{
    if (on == s_rippling) {
        return;
    }
    s_rippling = on;
    for (int i = 0; i < RING_COUNT; i++) {
        lv_obj_t *ring = s_rings[i];
        lv_anim_delete(ring, ring_size_cb);
        lv_anim_delete(ring, ring_opa_cb);
        lv_obj_set_size(ring, DOT_SIZE, DOT_SIZE);
        lv_obj_set_style_border_opa(ring, LV_OPA_TRANSP, 0);
        lv_obj_set_hidden(ring, !on);
        if (!on) {
            continue;
        }
        uint32_t delay = (uint32_t)i * RING_PERIOD_MS / RING_COUNT;
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, ring);
        lv_anim_set_duration(&a, RING_PERIOD_MS);
        lv_anim_set_delay(&a, delay);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_set_exec_cb(&a, ring_size_cb);
        lv_anim_set_values(&a, DOT_SIZE, RING_MAX);
        lv_anim_start(&a);
        lv_anim_set_exec_cb(&a, ring_opa_cb);
        lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
        lv_anim_start(&a);
    }
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
    lv_label_set_text(title, "ONURB");

    make_battery(scr);
    s_wifi = make_label(scr, &lv_font_montserrat_20, 0xB0B0B0, LV_ALIGN_TOP_MID, 110);

    for (int i = 0; i < RING_COUNT; i++) {
        lv_obj_t *ring = lv_obj_create(scr);
        lv_obj_set_size(ring, DOT_SIZE, DOT_SIZE);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(ring, 3, 0);
        lv_obj_set_style_border_color(ring, lv_color_white(), 0);
        lv_obj_set_style_border_opa(ring, LV_OPA_TRANSP, 0);
        lv_obj_set_scrollable(ring, false);
        lv_obj_set_clickable(ring, false);
        lv_obj_set_hidden(ring, true);
        lv_obj_align(ring, LV_ALIGN_CENTER, 0, 30);
        s_rings[i] = ring;
    }

    s_dot = lv_obj_create(scr);
    lv_obj_set_size(s_dot, DOT_SIZE, DOT_SIZE);
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

void onurb_ui_set_battery(int percent, bool charging, bool usb_in)
{
    char buf[8];
    int w = 0;
    uint32_t color = COLOR_BATT;

    if (!s_ready) {
        return;
    }
    if (percent < 0) {
        snprintf(buf, sizeof(buf), "--");
    } else {
        if (percent > 100) {
            percent = 100;
        }
        snprintf(buf, sizeof(buf), "%d%%", percent);
        w = BATT_INNER_W * percent / 100;
        if (percent > 0 && w < 2) {
            w = 2;
        }
        if (charging) {
            color = COLOR_BATT_CHG;
        }
    }
    bsp_display_lock(0);
    lv_label_set_text(s_batt_pct, buf);
    lv_obj_align_to(s_batt_pct, s_batt_body, LV_ALIGN_OUT_LEFT_MID, -6, 0);
    lv_obj_set_width(s_batt_fill, w);
    lv_obj_set_style_bg_color(s_batt_fill, lv_color_hex(color), 0);
    lv_obj_set_hidden(s_batt_bolt, !(charging || usb_in));
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

void onurb_ui_set_motion(const char *text)
{
    if (!s_ready) {
        return;
    }
    bsp_display_lock(0);
    lv_label_set_text(s_motion, text);
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
    ripple_set(state == ONURB_UI_RECORDING);
    bsp_display_unlock();
}

void onurb_ui_set_status(const char *text, uint32_t color)
{
    if (!s_ready) {
        return;
    }
    bsp_display_lock(0);
    lv_label_set_text(s_status, text);
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(color), 0);
    ripple_set(false);
    bsp_display_unlock();
}
