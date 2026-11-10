/*
 * Gesture feedback screen, kept separate from ui.c: loads its own LVGL screen
 * with two half-screen buttons and puts the status screen back afterwards.
 */
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lvgl.h"

#include "bsp/esp-bsp.h"

#include "ui_feedback.h"

static const char *TAG = "onurb_feedback";

#define COLOR_YES 0x34C759
#define COLOR_NO  0xFF3B30

static volatile int s_choice = -1;

static void on_click(lv_event_t *e)
{
    s_choice = (int)(intptr_t)lv_event_get_user_data(e);
}

static void make_half(lv_obj_t *scr, lv_align_t align, uint32_t color, const char *symbol, int value)
{
    lv_obj_t *btn = lv_button_create(scr);
    lv_obj_set_size(btn, LV_PCT(50), LV_PCT(100));
    lv_obj_align(btn, align, 0, 0);
    lv_obj_set_style_radius(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(btn, on_click, LV_EVENT_CLICKED, (void *)(intptr_t)value);

    lv_obj_t *label = lv_label_create(btn);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_label_set_text(label, symbol);
    lv_obj_center(label);
}

int onurb_ui_feedback_ask(int timeout_ms)
{
    s_choice = -1;

    bsp_display_lock(0);
    lv_obj_t *prev = lv_screen_active();
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_scrollable(scr, false);
    make_half(scr, LV_ALIGN_LEFT_MID, COLOR_YES, LV_SYMBOL_OK, 1);
    make_half(scr, LV_ALIGN_RIGHT_MID, COLOR_NO, LV_SYMBOL_CLOSE, 0);
    lv_screen_load(scr);
    bsp_display_unlock();

    for (int waited = 0; s_choice < 0 && waited < timeout_ms; waited += 20) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    int choice = s_choice;

    bsp_display_lock(0);
    lv_screen_load(prev);
    lv_obj_delete(scr);
    bsp_display_unlock();

    ESP_LOGI(TAG, "answer: %s", choice == 1 ? "correct" : choice == 0 ? "wrong" : "timeout");
    return choice;
}
