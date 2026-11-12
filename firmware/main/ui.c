/*
 * Minimal status screen (SPEC §8.4 / §10.4 groundwork): black background for
 * the AMOLED, a status bar (Wi-Fi icon, iOS-style battery), a status circle
 * that turns white and ripples while recording, and a one-line hint. All LVGL calls hold the BSP display lock.
 */
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lvgl.h"

#include "bsp/esp-bsp.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

#include "screen.h"
#include "ui.h"
#include "wifi.h"

static const char *TAG = "onurb_ui";

/* The agent circle stays in black/gray tones; color is reserved for status
 * accents (battery state, error text). */
#define COLOR_IDLE      0x3A3A3A
#define COLOR_RECORDING 0xFFFFFF
#define COLOR_BUSY      ONURB_UI_COLOR_BUSY
#define COLOR_BATT_FILL 0xFFFFFF
#define COLOR_BATT_LOW  0xFF453A /* iOS red */
#define COLOR_BATT_CHG  0x30D158 /* iOS green */
#define COLOR_ERROR_TXT 0xFF453A
#define COLOR_WIFI_WAIT 0xFFD60A /* iOS yellow: connecting / no internet yet */
#define COLOR_WIFI_OFF  0xFF453A
#define COLOR_BATT_BODY 0x5A5A5A /* unfilled part of the battery, like iOS */
#define COLOR_ICON_ON   0xFFFFFF
#define COLOR_ICON_MID  0x8E8E93
#define COLOR_ICON_OFF  0x4A4A4A

/* Title wordmark (option 1, "Espaçado fino"). */
#define COLOR_TITLE     0xD8D8D8
#define TITLE_Y         96
#define TITLE_TRACKING  18
#define TITLE_BAR_Y     140

/* Status bar: Wi-Fi top-left, battery top-right, inset for the rounded corners. */
#define BAR_Y       22
#define BAR_INSET   46

/* Recording ripple: rings grow out of the circle and fade. */
#define DOT_SIZE        140
#define DOT_Y           0    /* main circle sits at the screen center */
#define RING_MAX        230
#define RING_COUNT      2
#define RING_PERIOD_MS  1400

/* Reply history (onurb_ui_set_reply): a scrollable column under the status bar
 * that takes over the screen while shown (title, circle, status and hint
 * hide). Newest reply on top in white; older ones below in gray. */
#define REPLY_W         350
#define REPLY_TOP       62   /* below the Wi-Fi / battery bar */
#define REPLY_BOTTOM    36   /* margin above the rounded bottom edge */
#define REPLY_MAX       8    /* replies kept; the oldest drops off */
#define REPLY_GAP       22
#define COLOR_REPLY_OLD 0x9E9E9E
#define REPLY_BIG_CHARS 110  /* UTF-8 bytes; newest reply above this uses 20 px */

/* OTA "Updating" ring around the circle: spins while indeterminate, fills
 * clockwise from 12 o'clock with the download percentage. */
#define UPD_SIZE        (DOT_SIZE + 44)
#define UPD_WIDTH       6
#define UPD_SPIN_MS     1100
#define COLOR_UPD_TRACK 0x2A2A2A

/* While processing, the main circle itself is the cancel control (X icon). */
#define CANCEL_EXT_CLICK  24
#define COLOR_DOT_PRESSED 0x6A6A6A
#define HINT_IDLE   ""   /* idle: no hint (Bruno, 2026-11-11) */
#define HINT_CANCEL "Tap to cancel"
#define HINT_WRONG  "Tap if the gesture was wrong" /* ~290 px; fits the rounded bottom */
#define COLOR_DOT_CANCELLED 0xFF453A /* iOS red */
#define COLOR_DOT_CANCELLED_PRESSED 0xB3302A

LV_FONT_DECLARE(onurb_font_reply_26);
LV_FONT_DECLARE(onurb_font_reply_20);

/* Battery glyph: solid rounded body, white fill from the left, number inside. */
#define BATT_W      48   /* ~80% of the original 60x28 */
#define BATT_H      22

static lv_obj_t *s_batt_body;
static lv_obj_t *s_batt_fill;
static lv_obj_t *s_batt_text;
static lv_obj_t *s_wifi;
static lv_obj_t *s_dot;
static lv_obj_t *s_title;
static lv_obj_t *s_title_bar;
static lv_obj_t *s_reply; /* scrollable column of reply labels, newest first */
static lv_obj_t *s_upd_arc;
static lv_obj_t *s_upd_pct;
static bool s_updating;
static lv_obj_t *s_cancel_x;
static volatile bool s_cancel_taken; /* set by the LVGL task, read by the main task */
static onurb_ui_state_t s_state = ONURB_UI_IDLE;
static lv_obj_t *s_rings[RING_COUNT];
static bool s_rippling;
static lv_obj_t *s_status;
static lv_obj_t *s_hint;
static lv_obj_t *s_pend; /* pending-request bars, always on top */
static bool s_th_used;   /* onurb_ui_set_threads() has been called */
static lv_obj_t *s_thv;  /* thread view root */
static lv_obj_t *s_rail; /* thread rail, always on top */
static bool s_swiped;    /* the touch in progress turned into a swipe: ignore its click */
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

/* iOS-style battery: the charge number sits inside the glyph, dark over the
 * white fill; a bolt follows the number while on USB power. */
static void make_battery(lv_obj_t *scr)
{
    s_batt_body = make_box(scr);
    lv_obj_set_size(s_batt_body, BATT_W, BATT_H);
    lv_obj_set_style_radius(s_batt_body, 6, 0);
    lv_obj_set_style_bg_color(s_batt_body, lv_color_hex(COLOR_BATT_BODY), 0);
    lv_obj_set_style_clip_corner(s_batt_body, true, 0);
    lv_obj_align(s_batt_body, LV_ALIGN_TOP_RIGHT, -BAR_INSET, BAR_Y);

    lv_obj_t *nub = make_box(scr);
    lv_obj_set_size(nub, 3, 8);
    lv_obj_set_style_radius(nub, 2, 0);
    lv_obj_set_style_bg_color(nub, lv_color_hex(COLOR_BATT_BODY), 0);
    lv_obj_align_to(nub, s_batt_body, LV_ALIGN_OUT_RIGHT_MID, 1, 0);

    s_batt_fill = make_box(s_batt_body);
    lv_obj_set_size(s_batt_fill, 0, BATT_H);
    lv_obj_set_style_radius(s_batt_fill, 0, 0);
    lv_obj_set_style_bg_color(s_batt_fill, lv_color_hex(COLOR_BATT_FILL), 0);
    lv_obj_align(s_batt_fill, LV_ALIGN_LEFT_MID, 0, 0);

    s_batt_text = lv_label_create(s_batt_body);
    lv_obj_set_style_text_font(s_batt_text, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_batt_text, lv_color_black(), 0);
    lv_label_set_text(s_batt_text, "");
    lv_obj_center(s_batt_text);
}

static void make_wifi(lv_obj_t *scr)
{
    s_wifi = lv_label_create(scr);
    lv_obj_set_style_text_font(s_wifi, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_wifi, lv_color_hex(COLOR_ICON_OFF), 0);
    lv_label_set_text(s_wifi, LV_SYMBOL_WIFI);
    lv_obj_align(s_wifi, LV_ALIGN_TOP_LEFT, BAR_INSET, BAR_Y);
    /* Tap opens the network picker. 16 px of slop keeps the target above
     * the reply history (REPLY_TOP) and clear of the pending dashes. */
    lv_obj_set_clickable(s_wifi, true);
    lv_obj_set_ext_click_area(s_wifi, 16);
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

static void cancel_clicked_cb(lv_event_t *e)
{
    (void)e;
    if (!s_swiped) {
        s_cancel_taken = true;
    }
}

/* Call after s_dot exists. */
static void make_cancel(void)
{
    lv_obj_set_clickable(s_dot, false);
    lv_obj_set_ext_click_area(s_dot, CANCEL_EXT_CLICK);
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(COLOR_DOT_PRESSED), LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_dot, cancel_clicked_cb, LV_EVENT_CLICKED, NULL);

    s_cancel_x = lv_label_create(s_dot);
    lv_obj_set_style_text_font(s_cancel_x, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_color(s_cancel_x, lv_color_white(), 0);
    lv_label_set_text(s_cancel_x, LV_SYMBOL_CLOSE);
    lv_obj_center(s_cancel_x);
    lv_obj_set_hidden(s_cancel_x, true);
}

/* Caller holds the display lock. The circle is a tap target with an X while
 * processing (tap = cancel) and while cancelled (tap = "wrong gesture"). */
static void cancel_set(onurb_ui_state_t state)
{
    bool on = state == ONURB_UI_PROCESSING || state == ONURB_UI_CANCELLED;

    lv_obj_set_hidden(s_cancel_x, !on);
    lv_obj_set_clickable(s_dot, on);
    if (!on) {
        lv_obj_remove_state(s_dot, LV_STATE_PRESSED);
    }
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(state == ONURB_UI_CANCELLED ? COLOR_DOT_CANCELLED_PRESSED
                                                                               : COLOR_DOT_PRESSED),
                              LV_STATE_PRESSED);
    lv_label_set_text(s_hint, state == ONURB_UI_PROCESSING ? HINT_CANCEL
                              : state == ONURB_UI_CANCELLED ? HINT_WRONG : HINT_IDLE);
}

bool onurb_ui_cancel_taken(void)
{
    bool taken = s_cancel_taken;

    s_cancel_taken = false;
    return taken;
}

/* ---- WhatsApp send-approval preview (onurb_ui_show_whatsapp) -------------
 * Full-screen modal over the status screen: contact avatar + name on top,
 * the proposed messages as outgoing bubbles (right-aligned, WhatsApp dark
 * green) in a scrollable column, and Edit / Cancel / Confirm buttons. */
#define WA_AVATAR_SRC    96   /* gateway sends 96x96 RGB565 */
#define WA_AVATAR        72   /* drawn size */
#define WA_AVATAR_Y      22
#define WA_NAME_Y        (WA_AVATAR_Y + WA_AVATAR + 6)
#define WA_LIST_Y        (WA_NAME_Y + 34)
#define WA_BTN           68
#define WA_BTN_GAP       108  /* center-to-center */
#define WA_BTN_BOTTOM    (-34)
#define WA_LIST_W        360
#define WA_LIST_H        (BSP_LCD_V_RES + WA_BTN_BOTTOM - WA_BTN - 12 - WA_LIST_Y)
#define WA_BUBBLE_MAX_W  290
#define WA_MAX_MSGS      8
#define COLOR_WA_BUBBLE  0x005C4B /* WhatsApp outgoing bubble, dark theme */
#define COLOR_WA_GREEN   0x00A884
#define COLOR_WA_AVATAR  0x6B7C85
#define COLOR_WA_BTN     0x2E2E2E


static lv_obj_t *s_wa;          /* overlay root */
static lv_obj_t *s_wa_avatar;   /* circular clip container */
static lv_obj_t *s_wa_img;
static lv_obj_t *s_wa_initial;
static lv_obj_t *s_wa_name;
static lv_obj_t *s_wa_list;
static lv_image_dsc_t s_wa_dsc;
static uint16_t *s_wa_pixels;   /* PSRAM copy of the avatar */
static volatile int s_wa_action; /* ONURB_UI_ACTION_* set by the LVGL task */
static lv_obj_t *s_wa_btn_edit, *s_wa_btn_cancel, *s_wa_btn_ok;
#define PU_IMG      120  /* purchase: product photo, drawn 1:1 */
#define PU_NAME_Y   (WA_AVATAR_Y + PU_IMG + 8)
#define PU_NAME_H   50   /* two lines of title */
#define PU_LIST_Y   (PU_NAME_Y + PU_NAME_H + 6)

static void wa_btn_cb(lv_event_t *e)
{
    s_wa_action = (int)(intptr_t)lv_event_get_user_data(e);
}

static lv_obj_t *wa_button(lv_obj_t *parent, const char *symbol, uint32_t bg, int32_t x, int action)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, WA_BTN, WA_BTN);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x5A5A5A), LV_STATE_PRESSED);
    lv_obj_set_ext_click_area(b, 10);
    lv_obj_align(b, LV_ALIGN_BOTTOM_MID, x, WA_BTN_BOTTOM);
    lv_obj_add_event_cb(b, wa_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)action);

    lv_obj_t *icon = lv_label_create(b);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(icon, lv_color_white(), 0);
    lv_label_set_text(icon, symbol);
    lv_obj_center(icon);
    return b;
}

static void make_whatsapp(lv_obj_t *scr)
{
    s_wa = lv_obj_create(scr);
    lv_obj_set_size(s_wa, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_align(s_wa, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(s_wa, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_wa, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_wa, 0, 0);
    lv_obj_set_style_radius(s_wa, 0, 0);
    lv_obj_set_style_pad_all(s_wa, 0, 0);
    lv_obj_set_scrollable(s_wa, false);

    s_wa_avatar = make_box(s_wa);
    lv_obj_set_size(s_wa_avatar, WA_AVATAR, WA_AVATAR);
    lv_obj_set_style_radius(s_wa_avatar, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(s_wa_avatar, true, 0);
    lv_obj_set_style_bg_color(s_wa_avatar, lv_color_hex(COLOR_WA_AVATAR), 0);
    lv_obj_align(s_wa_avatar, LV_ALIGN_TOP_MID, 0, WA_AVATAR_Y);

    s_wa_img = lv_image_create(s_wa_avatar);
    lv_image_set_scale(s_wa_img, LV_SCALE_NONE * WA_AVATAR / WA_AVATAR_SRC);
    lv_obj_center(s_wa_img);
    lv_obj_set_hidden(s_wa_img, true);

    s_wa_initial = lv_label_create(s_wa_avatar);
    lv_obj_set_style_text_font(s_wa_initial, &onurb_font_reply_26, 0);
    lv_obj_set_style_text_color(s_wa_initial, lv_color_white(), 0);
    lv_obj_center(s_wa_initial);

    s_wa_name = lv_label_create(s_wa);
    lv_obj_set_width(s_wa_name, WA_LIST_W);
    lv_label_set_long_mode(s_wa_name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(s_wa_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_wa_name, &onurb_font_reply_20, 0);
    lv_obj_set_style_text_color(s_wa_name, lv_color_white(), 0);
    lv_obj_align(s_wa_name, LV_ALIGN_TOP_MID, 0, WA_NAME_Y);

    s_wa_list = lv_obj_create(s_wa);
    lv_obj_set_size(s_wa_list, WA_LIST_W, WA_LIST_H);
    lv_obj_align(s_wa_list, LV_ALIGN_TOP_MID, 0, WA_LIST_Y);
    lv_obj_set_style_bg_opa(s_wa_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_wa_list, 0, 0);
    lv_obj_set_style_pad_all(s_wa_list, 0, 0);
    lv_obj_set_style_pad_row(s_wa_list, 8, 0);
    lv_obj_set_flex_flow(s_wa_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_wa_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_scroll_dir(s_wa_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_wa_list, LV_SCROLLBAR_MODE_ACTIVE);

    s_wa_btn_edit = wa_button(s_wa, LV_SYMBOL_EDIT, COLOR_WA_BTN, -WA_BTN_GAP, ONURB_UI_ACTION_EDIT);
    s_wa_btn_cancel = wa_button(s_wa, LV_SYMBOL_CLOSE, COLOR_WA_BTN, 0, ONURB_UI_ACTION_CANCEL);
    s_wa_btn_ok = wa_button(s_wa, LV_SYMBOL_OK, COLOR_WA_GREEN, WA_BTN_GAP, ONURB_UI_ACTION_CONFIRM);

    lv_obj_set_hidden(s_wa, true);
}

/* First UTF-8 code point of name, ASCII upper-cased; "?" if empty. */
static void wa_initial(const char *name, char out[5])
{
    size_t n = 1;
    unsigned char c = name ? (unsigned char)name[0] : 0;

    if (c == 0) {
        strcpy(out, "?");
        return;
    }
    if (c >= 0xF0) {
        n = 4;
    } else if (c >= 0xE0) {
        n = 3;
    } else if (c >= 0xC0) {
        n = 2;
    }
    for (size_t i = 0; i < n; i++) {
        if (name[i] == '\0') {
            n = i;
            break;
        }
        out[i] = name[i];
    }
    out[n] = '\0';
    if (n == 1 && out[0] >= 'a' && out[0] <= 'z') {
        out[0] -= 'a' - 'A';
    }
}

/* Caller holds the display lock. */
static void wa_set_avatar_px(const uint16_t *rgb565, int px)
{
    lv_obj_set_hidden(s_wa_img, true);
    if (s_wa_pixels != NULL) {
        lv_image_set_src(s_wa_img, NULL); /* detach before freeing (image cache is off) */
        heap_caps_free(s_wa_pixels);
        s_wa_pixels = NULL;
    }
    if (rgb565 == NULL || px <= 0) {
        return;
    }
    size_t bytes = (size_t)px * px * sizeof(uint16_t);
    s_wa_pixels = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (s_wa_pixels == NULL) {
        ESP_LOGW(TAG, "avatar: out of PSRAM, showing initial");
        return;
    }
    memcpy(s_wa_pixels, rgb565, bytes);
    memset(&s_wa_dsc, 0, sizeof(s_wa_dsc));
    s_wa_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_wa_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_wa_dsc.header.w = px;
    s_wa_dsc.header.h = px;
    s_wa_dsc.header.stride = px * sizeof(uint16_t);
    s_wa_dsc.data_size = bytes;
    s_wa_dsc.data = (const uint8_t *)s_wa_pixels;
    lv_image_set_src(s_wa_img, &s_wa_dsc);
    lv_obj_center(s_wa_img);
    lv_obj_set_hidden(s_wa_img, false);
}

static void wa_set_avatar(const uint16_t *rgb565)
{
    wa_set_avatar_px(rgb565, WA_AVATAR_SRC);
}

/* Caller holds the display lock. WhatsApp layout (circle avatar, 3 buttons) or
 * purchase layout (rounded product photo, 2-line title, Cancel / Confirm). */
static void wa_layout(bool purchase)
{
    int img = purchase ? PU_IMG : WA_AVATAR;
    lv_obj_set_size(s_wa_avatar, img, img);
    lv_obj_set_style_radius(s_wa_avatar, purchase ? 14 : LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_wa_avatar, lv_color_hex(purchase ? 0xFFFFFF : COLOR_WA_AVATAR), 0);
    lv_image_set_scale(s_wa_img, purchase ? LV_SCALE_NONE : LV_SCALE_NONE * WA_AVATAR / WA_AVATAR_SRC);
    lv_obj_set_height(s_wa_name, purchase ? PU_NAME_H : LV_SIZE_CONTENT);
    lv_label_set_long_mode(s_wa_name, purchase ? LV_LABEL_LONG_MODE_WRAP : LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(s_wa_name, LV_ALIGN_TOP_MID, 0, purchase ? PU_NAME_Y : WA_NAME_Y);
    int list_y = purchase ? PU_LIST_Y : WA_LIST_Y;
    lv_obj_set_size(s_wa_list, WA_LIST_W, BSP_LCD_V_RES + WA_BTN_BOTTOM - WA_BTN - 12 - list_y);
    lv_obj_align(s_wa_list, LV_ALIGN_TOP_MID, 0, list_y);
    lv_obj_set_flex_align(s_wa_list, LV_FLEX_ALIGN_START, purchase ? LV_FLEX_ALIGN_CENTER : LV_FLEX_ALIGN_END,
                          purchase ? LV_FLEX_ALIGN_CENTER : LV_FLEX_ALIGN_END);
    lv_obj_set_hidden(s_wa_btn_edit, purchase);
    lv_obj_align(s_wa_btn_cancel, LV_ALIGN_BOTTOM_MID, purchase ? -WA_BTN_GAP / 2 - 6 : 0, WA_BTN_BOTTOM);
    lv_obj_align(s_wa_btn_ok, LV_ALIGN_BOTTOM_MID, purchase ? WA_BTN_GAP / 2 + 6 : WA_BTN_GAP, WA_BTN_BOTTOM);
}

static void wa_bubble(const char *utf8)
{
    lv_obj_t *b = lv_label_create(s_wa_list);
    lv_obj_set_width(b, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(b, WA_BUBBLE_MAX_W, 0);
    lv_label_set_long_mode(b, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(b, &onurb_font_reply_20, 0);
    lv_obj_set_style_text_color(b, lv_color_white(), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(COLOR_WA_BUBBLE), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 14, 0);
    lv_obj_set_style_pad_hor(b, 14, 0);
    lv_obj_set_style_pad_ver(b, 9, 0);
    lv_label_set_text(b, utf8); /* LVGL copies the text */
}

/* Caller holds the display lock. */
static void wa_hide(void)
{
    if (s_wa != NULL) {
        lv_obj_set_hidden(s_wa, true);
    }
}

void onurb_ui_show_whatsapp(const char *name, const uint16_t *avatar_rgb565, const char *const *msgs, int n)
{
    char initial[5];

    if (!s_ready) {
        return;
    }
    if (n > WA_MAX_MSGS) {
        n = WA_MAX_MSGS;
    }
    wa_initial(name, initial);
    bsp_display_lock(0);
    s_wa_action = ONURB_UI_ACTION_NONE;
    wa_layout(false);
    lv_label_set_text(s_wa_name, name ? name : "");
    lv_label_set_text(s_wa_initial, initial);
    wa_set_avatar(avatar_rgb565);
    lv_obj_clean(s_wa_list);
    for (int i = 0; i < n; i++) {
        if (msgs[i] != NULL && msgs[i][0] != '\0') {
            wa_bubble(msgs[i]);
        }
    }
    lv_obj_set_hidden(s_wa, false);
    lv_obj_move_foreground(s_wa);
    lv_obj_move_foreground(s_pend); /* pending bars stay on top of the modal */
    lv_obj_move_foreground(s_rail);
    lv_obj_update_layout(s_wa_list);
    lv_obj_scroll_to_y(s_wa_list, 0, LV_ANIM_OFF);
    bsp_display_unlock();
}

void onurb_ui_show_purchase(const char *title, const uint16_t *img_rgb565, int img_px, const char *const *msgs,
                            int n)
{
    if (!s_ready) {
        return;
    }
    if (n > WA_MAX_MSGS) {
        n = WA_MAX_MSGS;
    }
    bsp_display_lock(0);
    s_wa_action = ONURB_UI_ACTION_NONE;
    wa_layout(true);
    lv_label_set_text(s_wa_name, title ? title : "");
    lv_label_set_text(s_wa_initial, img_rgb565 ? "" : "?");
    wa_set_avatar_px(img_rgb565, img_px == 96 || img_px == 120 ? img_px : 0);
    if (img_rgb565 && img_px == 96) {
        lv_image_set_scale(s_wa_img, LV_SCALE_NONE * PU_IMG / 96);
    }
    lv_obj_clean(s_wa_list);
    for (int i = 0; i < n; i++) {
        if (msgs[i] == NULL || msgs[i][0] == '\0') {
            continue;
        }
        lv_obj_t *l = lv_label_create(s_wa_list);
        lv_obj_set_width(l, WA_LIST_W - 20);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(l, i == 0 ? &onurb_font_reply_26 : &onurb_font_reply_20, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(i == 0 ? COLOR_WA_GREEN : 0xBDBDBD), 0);
        lv_label_set_text(l, msgs[i]);
    }
    lv_obj_set_hidden(s_wa, false);
    lv_obj_move_foreground(s_wa);
    lv_obj_move_foreground(s_pend);
    lv_obj_move_foreground(s_rail);
    lv_obj_update_layout(s_wa_list);
    lv_obj_scroll_to_y(s_wa_list, 0, LV_ANIM_OFF);
    bsp_display_unlock();
}

/* Pending-request dashes: one short horizontal dash per request along the
 * left edge, light for processing, dark gray for queued offline (spool).
 * Drawn above every view (including the WhatsApp modal). */
#define PEND_MAX 6
#define PEND_W 18
#define PEND_H 4
#define PEND_GAP 6
#define PEND_X 14
#define COLOR_PEND_BUSY 0xD8D8D8
#define COLOR_PEND_QUEUED 0x4A4A4A
static lv_obj_t *s_pend_bar[PEND_MAX];

static void make_pending(lv_obj_t *scr)
{
    s_pend = lv_obj_create(scr);
    lv_obj_remove_style_all(s_pend);
    lv_obj_set_size(s_pend, PEND_W, PEND_MAX * PEND_H + (PEND_MAX - 1) * PEND_GAP);
    lv_obj_align(s_pend, LV_ALIGN_LEFT_MID, PEND_X, 0);
    lv_obj_set_flex_flow(s_pend, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_pend, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_pend, PEND_GAP, 0);
    lv_obj_set_clickable(s_pend, false);
    lv_obj_set_scrollable(s_pend, false);
    for (int i = 0; i < PEND_MAX; i++) {
        lv_obj_t *b = lv_obj_create(s_pend);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, PEND_W, PEND_H);
        lv_obj_set_style_radius(b, PEND_H / 2, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(COLOR_PEND_BUSY), 0);
        lv_obj_set_clickable(b, false);
        lv_obj_set_hidden(b, true);
        s_pend_bar[i] = b;
    }
    lv_obj_set_hidden(s_pend, true);
}

void onurb_ui_set_pending(int processing, int queued)
{
    if (!s_ready || s_th_used) { /* the thread rail replaces the dashes */
        return;
    }
    if (processing < 0) {
        processing = 0;
    }
    if (queued < 0) {
        queued = 0;
    }
    if (processing > PEND_MAX) {
        processing = PEND_MAX;
    }
    if (queued > PEND_MAX - processing) {
        queued = PEND_MAX - processing;
    }
    int n = processing + queued;

    bsp_display_lock(0);
    for (int i = 0; i < PEND_MAX; i++) {
        lv_obj_set_hidden(s_pend_bar[i], i >= n);
        if (i < n) {
            lv_obj_set_style_bg_color(s_pend_bar[i],
                lv_color_hex(i < processing ? COLOR_PEND_BUSY : COLOR_PEND_QUEUED), 0);
        }
    }
    lv_obj_set_hidden(s_pend, n == 0);
    lv_obj_move_foreground(s_pend);
    bsp_display_unlock();
}

int onurb_ui_action_taken(void)
{
    int a = s_wa_action;

    s_wa_action = ONURB_UI_ACTION_NONE;
    return a;
}

/* ---- Wi-Fi picker ------------------------------------------------------
 * Tap the Wi-Fi icon (main view only) -> scan -> list (name + signal) ->
 * tap a network -> on-screen keyboard for the password -> connect. wifi.c
 * keeps the new network (NVS) only if it gets an IP, otherwise it goes back
 * to the previous one. Built on open and deleted on close to keep the LVGL
 * pool free; the screen is held on while it's open and it closes itself
 * after WP_IDLE_MS without touches. The password never leaves the textarea
 * and onurb_wifi_join(), and is never logged. */
#define WP_MAX_APS   10
#define WP_LIST_W    340
#define WP_ROW_H     54
#define WP_LIST_Y    108
#define WP_LIST_H    (BSP_LCD_V_RES - WP_LIST_Y - 112)
#define WP_BTN       68
#define WP_IDLE_MS   60000
#define WP_KB_H      290
#define COLOR_WP_ROW 0x1E1E1E

static lv_obj_t *s_wp;          /* picker root (NULL when closed) */
static lv_obj_t *s_wp_cur;      /* "Connected to X" line */
static lv_obj_t *s_wp_list;
static lv_obj_t *s_wp_msg;      /* centered status ("Scanning...") */
static lv_obj_t *s_wp_btns[2];
static lv_obj_t *s_wp_kb_view;  /* password view (NULL when not shown) */
static lv_obj_t *s_wp_ta;
static lv_timer_t *s_wp_timer;
static onurb_wifi_ap_t s_wp_aps[WP_MAX_APS];
static volatile int s_wp_scan_n;
static volatile bool s_wp_scan_busy, s_wp_scan_done;
static int s_wp_sel = -1;       /* index into s_wp_aps being joined */
static bool s_wp_joining;
static int64_t s_wp_done_at;    /* close a moment after a successful join */

static void wp_close(void)
{
    if (s_wp == NULL) {
        return;
    }
    if (s_wp_timer != NULL) {
        lv_timer_delete(s_wp_timer);
        s_wp_timer = NULL;
    }
    if (s_wp_ta != NULL) {
        lv_textarea_set_text(s_wp_ta, "");
    }
    lv_obj_delete_async(s_wp); /* may be called from one of its own buttons */
    s_wp = s_wp_cur = s_wp_list = s_wp_msg = s_wp_kb_view = s_wp_ta = NULL;
    s_wp_btns[0] = s_wp_btns[1] = NULL;
    s_wp_joining = false;
    onurb_screen_release();
}

static void wp_scan_task(void *arg)
{
    int n = onurb_wifi_scan(s_wp_aps, WP_MAX_APS);

    s_wp_scan_n = n;
    s_wp_scan_done = true;
    s_wp_scan_busy = false;
    vTaskDelete(NULL);
}

static void wp_msg(const char *text, uint32_t color)
{
    lv_label_set_text(s_wp_msg, text);
    lv_obj_set_style_text_color(s_wp_msg, lv_color_hex(color), 0);
    lv_obj_set_hidden(s_wp_msg, text[0] == '\0');
}

static void wp_show_current(void)
{
    const char *ssid = onurb_wifi_ssid();

    if (ssid[0] == '\0') {
        lv_label_set_text(s_wp_cur, "Not connected");
    } else {
        lv_label_set_text_fmt(s_wp_cur, "%s %s", onurb_wifi_level() >= 1 ? "On" : "Trying", ssid);
    }
}

static void wp_start_scan(void)
{
    lv_obj_clean(s_wp_list);
    lv_obj_align(s_wp_msg, LV_ALIGN_CENTER, 0, 0);
    wp_msg("Scanning...", 0x9E9E9E);
    if (s_wp_scan_busy) {
        return; /* the running scan will deliver */
    }
    s_wp_scan_busy = true;
    s_wp_scan_done = false;
    if (xTaskCreate(wp_scan_task, "wifi_scan", 4096, NULL, 3, NULL) != pdPASS) {
        s_wp_scan_busy = false;
        wp_msg("Scan failed", COLOR_ERROR_TXT);
    }
}

static void wp_kb_close(void)
{
    if (s_wp_kb_view != NULL) {
        lv_textarea_set_text(s_wp_ta, "");
        lv_obj_delete_async(s_wp_kb_view);
        s_wp_kb_view = s_wp_ta = NULL;
    }
    lv_obj_set_hidden(s_wp_list, false);
    lv_obj_set_hidden(s_wp_btns[0], false);
    lv_obj_set_hidden(s_wp_btns[1], false);
}

static bool wp_join(const char *password)
{
    const onurb_wifi_ap_t *ap = &s_wp_aps[s_wp_sel];

    if (onurb_wifi_join(ap->ssid, password) != ESP_OK) {
        wp_msg("Couldn't start - try again", COLOR_ERROR_TXT);
        return false;
    }
    s_wp_joining = true;
    lv_obj_set_hidden(s_wp_list, true);
    lv_obj_set_hidden(s_wp_btns[0], true);
    lv_obj_set_hidden(s_wp_btns[1], true);
    lv_label_set_text_fmt(s_wp_msg, "Connecting to\n%s...", ap->ssid);
    lv_obj_set_style_text_color(s_wp_msg, lv_color_white(), 0);
    lv_obj_set_hidden(s_wp_msg, false);
    return true;
}

static void wp_kb_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_READY) {
        const char *pw = lv_textarea_get_text(s_wp_ta);
        if (strlen(pw) < 8) {
            wp_msg("At least 8 characters", COLOR_ERROR_TXT);
            lv_obj_align(s_wp_msg, LV_ALIGN_TOP_MID, 0, 128);
            return;
        }
        if (!wp_join(pw)) { /* wifi.c copies it */
            lv_obj_align(s_wp_msg, LV_ALIGN_TOP_MID, 0, 128);
            return;
        }
        lv_obj_align(s_wp_msg, LV_ALIGN_CENTER, 0, 0);
        wp_kb_close();
        lv_obj_set_hidden(s_wp_list, true);
        lv_obj_set_hidden(s_wp_btns[0], true);
        lv_obj_set_hidden(s_wp_btns[1], true);
    } else if (code == LV_EVENT_CANCEL) {
        lv_obj_align(s_wp_msg, LV_ALIGN_CENTER, 0, 0);
        wp_msg("", 0xFFFFFF);
        wp_kb_close();
    }
}

static void wp_eye_cb(lv_event_t *e)
{
    lv_obj_t *icon = lv_obj_get_child(lv_event_get_target(e), 0);
    bool hidden = lv_textarea_get_password_mode(s_wp_ta);

    lv_textarea_set_password_mode(s_wp_ta, !hidden);
    lv_label_set_text(icon, hidden ? LV_SYMBOL_EYE_CLOSE : LV_SYMBOL_EYE_OPEN);
}

static void wp_open_keyboard(void)
{
    const onurb_wifi_ap_t *ap = &s_wp_aps[s_wp_sel];

    lv_obj_set_hidden(s_wp_list, true);
    lv_obj_set_hidden(s_wp_btns[0], true);
    lv_obj_set_hidden(s_wp_btns[1], true);
    wp_msg("", 0xFFFFFF);

    s_wp_kb_view = make_box(s_wp);
    lv_obj_set_size(s_wp_kb_view, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_style_bg_color(s_wp_kb_view, lv_color_black(), 0);
    lv_obj_set_clickable(s_wp_kb_view, true); /* swallow stray taps */
    lv_obj_align(s_wp_kb_view, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *name = lv_label_create(s_wp_kb_view);
    lv_obj_set_width(name, 300);
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(name, &onurb_font_reply_20, 0);
    lv_obj_set_style_text_color(name, lv_color_white(), 0);
    lv_label_set_text(name, ap->ssid);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 26);

    s_wp_ta = lv_textarea_create(s_wp_kb_view);
    lv_obj_set_size(s_wp_ta, 270, 52);
    lv_obj_align(s_wp_ta, LV_ALIGN_TOP_MID, -30, 62);
    lv_textarea_set_one_line(s_wp_ta, true);
    lv_textarea_set_password_mode(s_wp_ta, true);
    lv_textarea_set_max_length(s_wp_ta, 63);
    lv_textarea_set_placeholder_text(s_wp_ta, "Password");
    lv_obj_set_style_text_font(s_wp_ta, &onurb_font_reply_20, 0);
    lv_obj_set_style_text_color(s_wp_ta, lv_color_white(), 0);
    lv_obj_set_style_bg_color(s_wp_ta, lv_color_hex(COLOR_WP_ROW), 0);
    lv_obj_set_style_border_color(s_wp_ta, lv_color_hex(0x5A5A5A), 0);
    lv_obj_set_style_radius(s_wp_ta, 14, 0);

    lv_obj_t *eye = lv_button_create(s_wp_kb_view);
    lv_obj_set_size(eye, 52, 52);
    lv_obj_set_style_radius(eye, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(eye, 0, 0);
    lv_obj_set_style_bg_color(eye, lv_color_hex(0x2E2E2E), 0);
    lv_obj_align_to(eye, s_wp_ta, LV_ALIGN_OUT_RIGHT_MID, 8, 0);
    lv_obj_add_event_cb(eye, wp_eye_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *eye_icon = lv_label_create(eye);
    lv_obj_set_style_text_font(eye_icon, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(eye_icon, lv_color_white(), 0);
    lv_label_set_text(eye_icon, LV_SYMBOL_EYE_OPEN);
    lv_obj_center(eye_icon);

    lv_obj_t *kb = lv_keyboard_create(s_wp_kb_view);
    lv_obj_set_size(kb, BSP_LCD_H_RES - 24, WP_KB_H);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, -18);
    lv_obj_set_style_text_font(kb, &lv_font_montserrat_20, 0);
    lv_obj_set_style_bg_color(kb, lv_color_black(), 0);
    lv_obj_set_style_border_width(kb, 0, 0);
    lv_keyboard_set_textarea(kb, s_wp_ta);
    lv_obj_add_event_cb(kb, wp_kb_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(kb, wp_kb_cb, LV_EVENT_CANCEL, NULL);

    lv_obj_move_foreground(s_wp_msg); /* "At least 8 characters" shows over the view */
}

static void wp_row_cb(lv_event_t *e)
{
    if (s_wp_joining || s_wp_kb_view != NULL) {
        return;
    }
    s_wp_sel = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_wp_aps[s_wp_sel].secure) {
        wp_open_keyboard();
    } else {
        wp_join(NULL);
    }
}

static uint32_t wp_signal_color(int rssi)
{
    return rssi >= -60 ? 0xFFFFFF : rssi >= -72 ? 0x9E9E9E : 0x5A5A5A;
}

static void wp_fill_list(int n)
{
    lv_obj_clean(s_wp_list);
    if (n < 0) {
        wp_msg("Scan failed", COLOR_ERROR_TXT);
        return;
    }
    if (n == 0) {
        wp_msg("No networks found", 0x9E9E9E);
        return;
    }
    wp_msg("", 0xFFFFFF);
    for (int i = 0; i < n; i++) {
        const onurb_wifi_ap_t *ap = &s_wp_aps[i];
        lv_obj_t *row = lv_button_create(s_wp_list);
        lv_obj_set_size(row, WP_LIST_W, WP_ROW_H);
        lv_obj_set_style_radius(row, 14, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_pad_hor(row, 16, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(COLOR_WP_ROW), 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x3A3A3A), LV_STATE_PRESSED);
        lv_obj_add_event_cb(row, wp_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *name = lv_label_create(row);
        lv_obj_set_width(name, ap->secure ? 250 : 200);
        lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_font(name, &onurb_font_reply_20, 0);
        lv_obj_set_style_text_color(name, lv_color_white(), 0);
        lv_label_set_text(name, ap->ssid);
        lv_obj_align(name, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *sig = lv_label_create(row);
        lv_obj_set_style_text_font(sig, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(sig, lv_color_hex(wp_signal_color(ap->rssi)), 0);
        lv_label_set_text(sig, LV_SYMBOL_WIFI);
        lv_obj_align(sig, LV_ALIGN_RIGHT_MID, 0, 0);

        if (!ap->secure) {
            lv_obj_t *open = lv_label_create(row);
            lv_obj_set_style_text_font(open, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(open, lv_color_hex(0x9E9E9E), 0);
            lv_label_set_text(open, "open");
            lv_obj_align(open, LV_ALIGN_RIGHT_MID, -32, 0);
        }
    }
    lv_obj_scroll_to_y(s_wp_list, 0, LV_ANIM_OFF);
}

static void wp_tick(lv_timer_t *t)
{
    if (s_wp == NULL) {
        return;
    }
    if (s_wp_scan_done) {
        s_wp_scan_done = false;
        if (!s_wp_joining && s_wp_kb_view == NULL) {
            wp_fill_list(s_wp_scan_n);
        }
    }
    if (s_wp_joining) {
        int st = onurb_wifi_join_state();
        if (st == ONURB_WIFI_JOIN_OK) {
            s_wp_joining = false;
            wp_msg("Connected", 0x30D158);
            wp_show_current();
            s_wp_done_at = esp_timer_get_time();
        } else if (st == ONURB_WIFI_JOIN_FAILED) {
            s_wp_joining = false;
            wp_msg("Couldn't connect.\nCheck the password.", COLOR_ERROR_TXT);
            lv_obj_align(s_wp_msg, LV_ALIGN_BOTTOM_MID, 0, -110);
            lv_obj_set_hidden(s_wp_list, false);
            lv_obj_set_hidden(s_wp_btns[0], false);
            lv_obj_set_hidden(s_wp_btns[1], false);
            wp_show_current();
        }
        return;
    }
    if (s_wp_done_at != 0) {
        if (esp_timer_get_time() - s_wp_done_at > 1500000) {
            s_wp_done_at = 0;
            wp_close();
        }
        return;
    }
    wp_show_current();
    if (lv_display_get_inactive_time(NULL) > WP_IDLE_MS) {
        wp_close();
    }
}

static void wp_btn_cb(lv_event_t *e)
{
    if ((intptr_t)lv_event_get_user_data(e) == 0) {
        wp_close();
    } else {
        lv_obj_set_hidden(s_wp_list, false);
        wp_start_scan();
    }
}

static lv_obj_t *wp_button(const char *symbol, int32_t x, intptr_t id)
{
    lv_obj_t *b = lv_button_create(s_wp);
    lv_obj_set_size(b, WP_BTN, WP_BTN);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x2E2E2E), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x5A5A5A), LV_STATE_PRESSED);
    lv_obj_set_ext_click_area(b, 10);
    lv_obj_align(b, LV_ALIGN_BOTTOM_MID, x, -24);
    lv_obj_add_event_cb(b, wp_btn_cb, LV_EVENT_CLICKED, (void *)id);

    lv_obj_t *icon = lv_label_create(b);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(icon, lv_color_white(), 0);
    lv_label_set_text(icon, symbol);
    lv_obj_center(icon);
    return b;
}

static void wp_open(void)
{
    lv_obj_t *scr = lv_screen_active();

    if (s_wp != NULL) {
        return;
    }
    onurb_screen_hold();
    s_wp_sel = -1;
    s_wp_joining = false;
    s_wp_done_at = 0;

    s_wp = make_box(scr);
    lv_obj_set_size(s_wp, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_style_bg_color(s_wp, lv_color_black(), 0);
    lv_obj_set_style_radius(s_wp, 0, 0);
    lv_obj_set_clickable(s_wp, true); /* nothing underneath gets taps */
    lv_obj_align(s_wp, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *title = lv_label_create(s_wp);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_label_set_text(title, "Wi-Fi");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

    s_wp_cur = lv_label_create(s_wp);
    lv_obj_set_width(s_wp_cur, 320);
    lv_label_set_long_mode(s_wp_cur, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(s_wp_cur, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_wp_cur, &onurb_font_reply_20, 0);
    lv_obj_set_style_text_color(s_wp_cur, lv_color_hex(0x9E9E9E), 0);
    lv_obj_align(s_wp_cur, LV_ALIGN_TOP_MID, 0, 70);
    wp_show_current();

    s_wp_list = lv_obj_create(s_wp);
    lv_obj_set_size(s_wp_list, WP_LIST_W, WP_LIST_H);
    lv_obj_align(s_wp_list, LV_ALIGN_TOP_MID, 0, WP_LIST_Y);
    lv_obj_set_style_bg_opa(s_wp_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_wp_list, 0, 0);
    lv_obj_set_style_pad_all(s_wp_list, 0, 0);
    lv_obj_set_style_pad_row(s_wp_list, 8, 0);
    lv_obj_set_flex_flow(s_wp_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_wp_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_wp_list, LV_SCROLLBAR_MODE_ACTIVE);

    s_wp_msg = lv_label_create(s_wp);
    lv_obj_set_width(s_wp_msg, 320);
    lv_obj_set_style_text_align(s_wp_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_wp_msg, &onurb_font_reply_20, 0);
    lv_obj_align(s_wp_msg, LV_ALIGN_CENTER, 0, 0);

    s_wp_btns[0] = wp_button(LV_SYMBOL_CLOSE, -45, 0);
    s_wp_btns[1] = wp_button(LV_SYMBOL_REFRESH, 45, 1);

    lv_obj_move_foreground(s_pend); /* pending dashes stay on top */
    lv_obj_move_foreground(s_rail);
    s_wp_timer = lv_timer_create(wp_tick, 250, NULL);
    wp_start_scan();
}

/* Wi-Fi icon tap: only from the main view, so it can never steal a tap
 * meant for the reply history or the WhatsApp preview. */
static void wifi_icon_cb(lv_event_t *e)
{
    if (s_swiped || s_wp != NULL || !lv_obj_is_hidden(s_wa) || !lv_obj_is_hidden(s_reply) ||
        !lv_obj_is_hidden(s_thv)) {
        return;
    }
    wp_open();
}

/* ---- Threads -------------------------------------------------------------
 * One thread per request. The left rail (s_rail) shows a dot for the main
 * view above one horizontal dash per thread, newest first, colored by state;
 * the open view is drawn at full opacity and wider, the rest dimmed. A
 * vertical swipe anywhere moves between views (up = next older, down = back
 * toward the main view). A long thread body scrolls first: the swipe only
 * changes view if the body was already at that edge when the finger landed.
 * Swipes are tracked on the input device itself (PRESSED/RELEASED reach it
 * before the object), and a swipe sets s_swiped so the CLICKED that LVGL
 * sends on release is ignored by the cancel X, the mic and the Wi-Fi icon. */
#define TH_MAX         ONURB_UI_MAX_THREADS
#define TH_TEXT        (ONURB_UI_THREAD_TEXT_MAX + 1)
#define TH_HIST        (ONURB_UI_THREAD_HIST_MAX + 1)
#define TH_X           56   /* small X beside the mic */
#define RAIL_X         14
#define RAIL_DOT       10
#define RAIL_DASH_W    18
#define RAIL_DASH_SEL  28
#define RAIL_DASH_H    4
#define RAIL_GAP       8
#define TH_BODY_W      330
#define TH_BODY_Y      74
#define TH_MIC         84
#define TH_BUBBLE_MAX  280
#define SWIPE_MIN      50
#define COLOR_TH_BUSY  0xD8D8D8
#define COLOR_TH_DONE  0x8A8A8A
#define COLOR_TH_OPEN  0xFFD60A
#define COLOR_TH_ERR   0xFF453A
#define COLOR_TH_HEARD 0x2E2E2E
#define COLOR_TH_ADD   0xBDBDBD /* mic on a thread not waiting for the user: add / follow up */

typedef struct {
    uint32_t id;
    onurb_thread_state_t state;
    char *heard; /* TH_TEXT bytes each, in PSRAM */
    char *reply;
    char *hist;  /* TH_HIST bytes, PSRAM: earlier exchanges ("heard\x1freply\x1e"...) */
    bool addable; /* mic shown (see onurb_thread_t) */
} th_item_t;

static th_item_t s_th[TH_MAX];
static int s_th_n;
static volatile uint32_t s_view_id; /* 0 = main view */
static uint32_t s_th_shown_id;      /* what the thread view currently renders */
static uint32_t s_th_shown_hash;
static volatile uint32_t s_mic_taken;
static uint32_t s_mic_rec_id;       /* thread whose mic started the current recording */
static bool s_mic_recording;

static lv_obj_t *s_rail_dot;
static lv_obj_t *s_rail_dash[TH_MAX];
static lv_obj_t *s_thv_spin;
static lv_obj_t *s_thv_head;
static lv_obj_t *s_thv_body;
static lv_obj_t *s_thv_mic;
static lv_obj_t *s_thv_mic_hint;
static lv_obj_t *s_thv_cancel;   /* X while PROCESSING */
static lv_obj_t *s_thv_x;        /* small X beside the mic: close the thread / discard the recording */
static volatile bool s_rec_stop, s_rec_cancel;
static volatile uint32_t s_th_cancel_taken;
static uint32_t s_th_cancel_sent; /* tapped, waiting for the thread to go */

static lv_point_t s_sw_start;
static bool s_sw_at_top, s_sw_at_bottom;

static uint32_t th_color(onurb_thread_state_t st)
{
    switch (st) {
    case ONURB_THREAD_OPEN:
        return COLOR_TH_OPEN;
    case ONURB_THREAD_ERROR:
        return COLOR_TH_ERR;
    case ONURB_THREAD_DONE:
        return COLOR_TH_DONE;
    default:
        return COLOR_TH_BUSY;
    }
}

static int th_find(uint32_t id)
{
    for (int i = 0; i < s_th_n; i++) {
        if (s_th[i].id == id) {
            return i;
        }
    }
    return -1;
}

/* strlcpy that never leaves half a UTF-8 sequence at the cut. */
static void copy_utf8(char *dst, const char *src, size_t cap)
{
    size_t n;

    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) {
            n--; /* src[n] is a continuation byte: cut before its lead */
        }
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static uint32_t th_hash(const th_item_t *t)
{
    uint32_t h = 2166136261u ^ (uint32_t)t->state ^ (t->addable ? 0x100u : 0u);

    for (const char *p = t->heard; *p; p++) {
        h = (h ^ (uint8_t)*p) * 16777619u;
    }
    h = (h ^ 0xFF) * 16777619u;
    for (const char *p = t->reply; *p; p++) {
        h = (h ^ (uint8_t)*p) * 16777619u;
    }
    h = (h ^ 0xFE) * 16777619u;
    for (const char *p = t->hist; *p; p++) {
        h = (h ^ (uint8_t)*p) * 16777619u;
    }
    return h;
}

static void rail_update(void)
{
    int sel = th_find(s_view_id);

    lv_obj_set_hidden(s_rail, s_th_n == 0);
    lv_obj_set_style_bg_opa(s_rail_dot, sel < 0 ? LV_OPA_COVER : LV_OPA_40, 0);
    for (int i = 0; i < TH_MAX; i++) {
        lv_obj_t *d = s_rail_dash[i];
        lv_obj_set_hidden(d, i >= s_th_n);
        if (i >= s_th_n) {
            continue;
        }
        lv_obj_set_style_bg_color(d, lv_color_hex(th_color(s_th[i].state)), 0);
        lv_obj_set_style_bg_opa(d, i == sel ? LV_OPA_COVER : LV_OPA_50, 0);
        lv_obj_set_width(d, i == sel ? RAIL_DASH_SEL : RAIL_DASH_W);
    }
    lv_obj_move_foreground(s_rail);
}

/* Buttons under the thread body:
 *   OPEN            yellow mic (tap = answer) + small X (close the thread)
 *   OPEN, recording red mic (tap = stop and send) + small X (discard, back to OPEN)
 *   WORKING/DONE/   gray mic (tap = add to this task / follow up); recording as above
 *   ERROR, addable  (the X then only discards; these threads aren't closed by hand)
 *   PROCESSING      big X (cancel the request) */
static bool th_can_add(const th_item_t *t)
{
    return t->addable && t->state != ONURB_THREAD_PROCESSING;
}

static void thv_controls(const th_item_t *t)
{
    bool open = t->state == ONURB_THREAD_OPEN;
    bool add = th_can_add(t);
    bool rec = add && s_mic_recording && s_mic_rec_id == t->id;
    bool cancellable = t->state == ONURB_THREAD_PROCESSING;

    lv_obj_set_hidden(s_thv_mic, !add);
    lv_obj_set_hidden(s_thv_x, !(open || rec));
    lv_obj_set_hidden(s_thv_cancel, !cancellable);
    lv_obj_set_hidden(s_thv_mic_hint, !add && !cancellable);
    if (add) {
        lv_obj_set_style_bg_color(s_thv_mic, lv_color_hex(rec ? COLOR_TH_ERR : open ? COLOR_TH_OPEN : COLOR_TH_ADD), 0);
        lv_label_set_text(s_thv_mic_hint, rec ? "Listening... tap to send" : open ? "Tap to answer" : "Tap to add");
    } else if (cancellable) {
        lv_label_set_text(s_thv_mic_hint, s_th_cancel_sent == t->id ? "Cancelling..." : "Tap to cancel");
    }
}

static void mic_style(void)
{
    int i = th_find(s_view_id);

    if (i >= 0) {
        thv_controls(&s_th[i]);
    }
}

static lv_obj_t *th_bubble_heard(const char *text)
{
    lv_obj_t *b = make_box(s_thv_body);
    lv_obj_set_size(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(b, TH_BUBBLE_MAX, 0);
    lv_obj_set_style_radius(b, 16, 0);
    lv_obj_set_style_pad_hor(b, 14, 0);
    lv_obj_set_style_pad_ver(b, 9, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(COLOR_TH_HEARD), 0);
    lv_obj_set_align(b, LV_ALIGN_TOP_RIGHT);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_width(l, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(l, TH_BUBBLE_MAX - 28, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(l, &onurb_font_reply_20, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(text ? 0xD8D8D8 : 0x8A8A8A), 0);
    lv_label_set_text(l, text ? text : "...");   /* NULL: sent, transcript not back yet */
    return b;
}

static void th_reply_label(const char *text, bool latest)
{
    lv_obj_t *l = lv_label_create(s_thv_body);
    lv_obj_set_width(l, TH_BODY_W);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(l, latest && strlen(text) <= REPLY_BIG_CHARS ? &onurb_font_reply_26
                                                                           : &onurb_font_reply_20, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(latest ? 0xFFFFFF : 0xBDBDBD), 0);
    lv_label_set_text(l, text);
}

static void thv_render(const th_item_t *t)
{
    static const char *const heads[] = {
        [ONURB_THREAD_PROCESSING] = "Processing...",
        [ONURB_THREAD_WORKING] = "Working...",
        [ONURB_THREAD_DONE] = "Done",
        [ONURB_THREAD_OPEN] = "Waiting for you",
        [ONURB_THREAD_ERROR] = "Something went wrong",
    };
    bool busy = t->state == ONURB_THREAD_PROCESSING || t->state == ONURB_THREAD_WORKING;
    uint32_t h = th_hash(t);

    if (t->id == s_th_shown_id && h == s_th_shown_hash) {
        return; /* unchanged: keep the body's scroll position */
    }
    bool same = t->id == s_th_shown_id;
    int32_t keep_y = same ? lv_obj_get_scroll_y(s_thv_body) : 0;
    static uint32_t shown_text_len;            /* only a state change keeps the reading position */
    uint32_t text_len = (uint32_t)(strlen(t->heard) + strlen(t->reply) + strlen(t->hist));
    bool changed_text = !same || text_len != shown_text_len;
    shown_text_len = text_len;
    s_th_shown_id = t->id;
    s_th_shown_hash = h;

    lv_label_set_text(s_thv_head, (unsigned)t->state < 5 ? heads[t->state] : "");
    lv_obj_set_style_text_color(s_thv_head, lv_color_hex(busy ? 0x9E9E9E : th_color(t->state)), 0);
    lv_obj_set_hidden(s_thv_spin, !busy);

    lv_obj_set_height(s_thv_body, BSP_LCD_V_RES - TH_BODY_Y -
                                   (th_can_add(t) || t->state == ONURB_THREAD_PROCESSING ? TH_MIC + 64 : 36));
    lv_obj_clean(s_thv_body);
    /* The whole exchange, oldest first: earlier turns (dimmer), then the latest. */
    static char seg[TH_TEXT];
    lv_obj_t *last_heard = NULL;
    for (const char *p = t->hist; *p;) {
        const char *us = strchr(p, '\x1f'), *rs = strchr(p, '\x1e');
        if (rs == NULL) {
            break;
        }
        const char *hend = us && us < rs ? us : rs;
        size_t n = (size_t)(hend - p) < sizeof(seg) - 1 ? (size_t)(hend - p) : sizeof(seg) - 1;
        memcpy(seg, p, n);
        seg[n] = '\0';
        if (seg[0]) {
            last_heard = th_bubble_heard(seg);
        }
        if (us && us < rs) {
            n = (size_t)(rs - us - 1) < sizeof(seg) - 1 ? (size_t)(rs - us - 1) : sizeof(seg) - 1;
            memcpy(seg, us + 1, n);
            seg[n] = '\0';
            if (seg[0]) {
                th_reply_label(seg, false);
            }
        }
        p = rs + 1;
    }
    if (t->heard[0] != '\0') {
        last_heard = th_bubble_heard(t->heard);
    } else if (t->hist[0] != '\0' && busy) {
        last_heard = th_bubble_heard(NULL);   /* the answer just sent: "..." until its transcript */
    }
    if (t->reply[0] != '\0') {
        th_reply_label(t->reply, true);
    }
    thv_controls(t);
    lv_obj_update_layout(s_thv_body);
    if (same && keep_y >= 0 && !changed_text) {
        lv_obj_scroll_to_y(s_thv_body, keep_y, LV_ANIM_OFF);
    } else {
        /* New content: show the latest exchange, from its question if it doesn't fit. */
        lv_obj_scroll_to_y(s_thv_body, lv_obj_get_scroll_y(s_thv_body) + lv_obj_get_scroll_bottom(s_thv_body),
                           LV_ANIM_OFF);
        if (last_heard) {
            lv_obj_scroll_to_view(last_heard, LV_ANIM_OFF);
        }
    }
}

static void thv_slide_cb(void *obj, int32_t v)
{
    lv_obj_set_style_translate_y(obj, v, 0);
}

/* Show view `id` (0 = main). dir > 0 slides in from below, < 0 from above. */
static void th_go(uint32_t id, int dir)
{
    int i = th_find(id);

    if (i < 0) {
        id = 0;
    }
    s_view_id = id;
    if (id == 0) {
        lv_obj_set_hidden(s_thv, true);
        s_th_shown_id = 0;
    } else {
        thv_render(&s_th[i]);
        lv_obj_set_hidden(s_thv, false);
        lv_obj_move_foreground(s_thv);
        if (!lv_obj_is_hidden(s_wa)) {
            lv_obj_move_foreground(s_wa); /* the WhatsApp preview stays above */
        }
        if (dir != 0) {
            lv_anim_t a;
            lv_anim_init(&a);
            lv_anim_set_var(&a, s_thv);
            lv_anim_set_exec_cb(&a, thv_slide_cb);
            lv_anim_set_values(&a, dir > 0 ? 70 : -70, 0);
            lv_anim_set_duration(&a, 160);
            lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
            lv_anim_start(&a);
        }
    }
    rail_update();
}

static bool swipes_allowed(void)
{
    return s_th_n > 0 && s_wp == NULL && lv_obj_is_hidden(s_wa);
}

static void indev_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_event_get_target(e);
    lv_point_t p;

    lv_indev_get_point(indev, &p);
    if (code == LV_EVENT_PRESSED) {
        onurb_screen_touch();                 /* any touch restarts the screen-off timer */
        s_swiped = false;
        s_sw_start = p;
        s_sw_at_top = s_view_id == 0 || lv_obj_get_scroll_top(s_thv_body) <= 0;
        s_sw_at_bottom = s_view_id == 0 || lv_obj_get_scroll_bottom(s_thv_body) <= 0;
        return;
    }
    if (code != LV_EVENT_RELEASED || !swipes_allowed()) {
        return;
    }
    int32_t dx = p.x - s_sw_start.x, dy = p.y - s_sw_start.y;
    if (LV_ABS(dy) < SWIPE_MIN || LV_ABS(dy) < 2 * LV_ABS(dx)) {
        return;
    }
    s_swiped = true; /* swallow the CLICKED that follows this release */
    int cur = th_find(s_view_id); /* -1 = main */
    if (dy < 0 && s_sw_at_bottom && cur + 1 < s_th_n) {
        th_go(s_th[cur + 1].id, +1);           /* finger up: older */
    } else if (dy > 0 && s_sw_at_top && cur >= 0) {
        th_go(cur == 0 ? 0 : s_th[cur - 1].id, -1); /* finger down: newer / main */
    }
}

static void mic_cb(lv_event_t *e)
{
    int i = th_find(s_view_id);

    if (s_swiped || i < 0 || !th_can_add(&s_th[i])) {
        return;
    }
    if (s_mic_recording && s_mic_rec_id == s_view_id) {
        s_rec_stop = true;                    /* red mic: stop and send */
        return;
    }
    s_mic_taken = s_view_id;
    s_mic_rec_id = s_view_id;
}

static void th_x_cb(lv_event_t *e)
{
    int i = th_find(s_view_id);

    if (s_swiped || i < 0 || !th_can_add(&s_th[i])) {
        return;
    }
    if (s_mic_recording && s_mic_rec_id == s_view_id) {
        s_rec_cancel = true;                  /* discard the answer, back to "Tap to answer" */
        return;
    }
    if (s_th[i].state != ONURB_THREAD_OPEN) {
        return;                               /* only "waiting for you" threads are closed by hand */
    }
    s_th_cancel_taken = s_view_id;            /* close the thread (the main task removes it) */
}

static void th_cancel_cb(lv_event_t *e)
{
    int i = th_find(s_view_id);

    if (s_swiped || i < 0 || s_th[i].state != ONURB_THREAD_PROCESSING) {
        return;
    }
    s_th_cancel_taken = s_view_id;
    s_th_cancel_sent = s_view_id;
    lv_label_set_text(s_thv_mic_hint, "Cancelling...");
}

static void make_mic_glyph(lv_obj_t *btn)
{
    lv_obj_t *cap = make_box(btn); /* capsule */
    lv_obj_set_size(cap, 16, 28);
    lv_obj_set_style_radius(cap, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(cap, lv_color_black(), 0);
    lv_obj_align(cap, LV_ALIGN_CENTER, 0, -8);

    lv_obj_t *arc = lv_arc_create(btn); /* cradle */
    lv_obj_set_size(arc, 30, 30);
    lv_arc_set_bg_angles(arc, 0, 180);
    lv_obj_set_style_arc_width(arc, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(arc, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(arc, 0, LV_PART_KNOB);
    lv_obj_set_clickable(arc, false);
    lv_obj_align(arc, LV_ALIGN_CENTER, 0, -6);

    lv_obj_t *stem = make_box(btn);
    lv_obj_set_size(stem, 3, 8);
    lv_obj_set_style_bg_color(stem, lv_color_black(), 0);
    lv_obj_align(stem, LV_ALIGN_CENTER, 0, 13);

    lv_obj_t *base = make_box(btn);
    lv_obj_set_size(base, 16, 3);
    lv_obj_set_style_radius(base, 1, 0);
    lv_obj_set_style_bg_color(base, lv_color_black(), 0);
    lv_obj_align(base, LV_ALIGN_CENTER, 0, 18);
}

static void make_threads(lv_obj_t *scr)
{
    for (int i = 0; i < TH_MAX; i++) {
        s_th[i].heard = heap_caps_calloc(1, TH_TEXT, MALLOC_CAP_SPIRAM);
        s_th[i].reply = heap_caps_calloc(1, TH_TEXT, MALLOC_CAP_SPIRAM);
        s_th[i].hist = heap_caps_calloc(1, TH_HIST, MALLOC_CAP_SPIRAM);
        if (s_th[i].heard == NULL || s_th[i].reply == NULL || s_th[i].hist == NULL) {
            ESP_LOGE(TAG, "thread buffers: out of PSRAM");
        }
    }

    s_thv = make_box(scr);
    lv_obj_set_size(s_thv, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_style_bg_color(s_thv, lv_color_black(), 0);
    lv_obj_set_style_radius(s_thv, 0, 0);
    lv_obj_set_clickable(s_thv, true); /* nothing underneath gets taps */
    lv_obj_align(s_thv, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *hdr = make_box(s_thv);
    lv_obj_set_size(hdr, 330, 32);
    lv_obj_set_style_bg_opa(hdr, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hdr, 10, 0);
    lv_obj_align(hdr, LV_ALIGN_TOP_MID, 0, 26);

    s_thv_spin = lv_spinner_create(hdr);
    lv_obj_set_size(s_thv_spin, 24, 24);
    lv_spinner_set_anim_params(s_thv_spin, 1000, 270);
    lv_obj_set_style_arc_width(s_thv_spin, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_thv_spin, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_thv_spin, lv_color_hex(0x3A3A3A), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_thv_spin, lv_color_white(), LV_PART_INDICATOR);
    lv_obj_set_clickable(s_thv_spin, false);

    s_thv_head = lv_label_create(hdr);
    lv_obj_set_style_text_font(s_thv_head, &lv_font_montserrat_20, 0);
    lv_label_set_text(s_thv_head, "");

    s_thv_body = lv_obj_create(s_thv);
    lv_obj_set_size(s_thv_body, TH_BODY_W, BSP_LCD_V_RES - TH_BODY_Y - 36);
    lv_obj_align(s_thv_body, LV_ALIGN_TOP_MID, 8, TH_BODY_Y);
    lv_obj_set_style_bg_opa(s_thv_body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_thv_body, 0, 0);
    lv_obj_set_style_pad_all(s_thv_body, 0, 0);
    lv_obj_set_style_pad_row(s_thv_body, 16, 0);
    lv_obj_set_flex_flow(s_thv_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_thv_body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_scroll_dir(s_thv_body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_thv_body, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_scroll_chain_ver(s_thv_body, false); /* indev_cb does the chaining */

    s_thv_mic = lv_button_create(s_thv);
    lv_obj_set_size(s_thv_mic, TH_MIC, TH_MIC);
    lv_obj_set_style_radius(s_thv_mic, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(s_thv_mic, 0, 0);
    lv_obj_set_style_bg_color(s_thv_mic, lv_color_hex(COLOR_TH_OPEN), 0);
    lv_obj_set_style_bg_color(s_thv_mic, lv_color_hex(0xB89A00), LV_STATE_PRESSED);
    lv_obj_set_ext_click_area(s_thv_mic, 10);
    lv_obj_align(s_thv_mic, LV_ALIGN_BOTTOM_MID, 0, -46);
    lv_obj_add_event_cb(s_thv_mic, mic_cb, LV_EVENT_CLICKED, NULL);
    make_mic_glyph(s_thv_mic);

    s_thv_cancel = lv_button_create(s_thv);
    lv_obj_set_size(s_thv_cancel, TH_MIC, TH_MIC);
    lv_obj_set_style_radius(s_thv_cancel, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(s_thv_cancel, 0, 0);
    lv_obj_set_style_bg_color(s_thv_cancel, lv_color_hex(0x2E2E2E), 0);
    lv_obj_set_style_bg_color(s_thv_cancel, lv_color_hex(0xB3302A), LV_STATE_PRESSED);
    lv_obj_set_ext_click_area(s_thv_cancel, 10);
    lv_obj_align(s_thv_cancel, LV_ALIGN_BOTTOM_MID, 0, -46);
    lv_obj_add_event_cb(s_thv_cancel, th_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *x = lv_label_create(s_thv_cancel);
    lv_obj_set_style_text_font(x, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_color(x, lv_color_white(), 0);
    lv_label_set_text(x, LV_SYMBOL_CLOSE);
    lv_obj_center(x);
    lv_obj_set_hidden(s_thv_cancel, true);

    s_thv_x = lv_button_create(s_thv);
    lv_obj_set_size(s_thv_x, TH_X, TH_X);
    lv_obj_set_style_radius(s_thv_x, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(s_thv_x, 0, 0);
    lv_obj_set_style_bg_color(s_thv_x, lv_color_hex(0x2E2E2E), 0);
    lv_obj_set_style_bg_color(s_thv_x, lv_color_hex(0xB3302A), LV_STATE_PRESSED);
    lv_obj_set_ext_click_area(s_thv_x, 8);
    lv_obj_align(s_thv_x, LV_ALIGN_BOTTOM_MID, TH_MIC / 2 + 22 + TH_X / 2, -46 - (TH_MIC - TH_X) / 2);
    lv_obj_add_event_cb(s_thv_x, th_x_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sx = lv_label_create(s_thv_x);
    lv_obj_set_style_text_font(sx, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(sx, lv_color_white(), 0);
    lv_label_set_text(sx, LV_SYMBOL_CLOSE);
    lv_obj_center(sx);
    lv_obj_set_hidden(s_thv_x, true);

    s_thv_mic_hint = lv_label_create(s_thv);
    lv_obj_set_style_text_font(s_thv_mic_hint, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_thv_mic_hint, lv_color_hex(0x9E9E9E), 0);
    lv_label_set_text(s_thv_mic_hint, "");
    lv_obj_align(s_thv_mic_hint, LV_ALIGN_BOTTOM_MID, 0, -22);
    lv_obj_set_hidden(s_thv, true);

    s_rail = lv_obj_create(scr);
    lv_obj_remove_style_all(s_rail);
    lv_obj_set_size(s_rail, RAIL_DASH_SEL, LV_SIZE_CONTENT);
    lv_obj_align(s_rail, LV_ALIGN_LEFT_MID, RAIL_X, 0);
    lv_obj_set_flex_flow(s_rail, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_rail, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_rail, RAIL_GAP, 0);
    lv_obj_set_clickable(s_rail, false);
    lv_obj_set_scrollable(s_rail, false);
    s_rail_dot = make_box(s_rail);
    lv_obj_set_size(s_rail_dot, RAIL_DOT, RAIL_DOT);
    lv_obj_set_style_radius(s_rail_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_rail_dot, lv_color_white(), 0);
    lv_obj_set_style_margin_bottom(s_rail_dot, 4, 0);
    for (int i = 0; i < TH_MAX; i++) {
        lv_obj_t *d = make_box(s_rail);
        lv_obj_set_size(d, RAIL_DASH_W, RAIL_DASH_H);
        lv_obj_set_style_radius(d, RAIL_DASH_H / 2, 0);
        lv_obj_set_hidden(d, true);
        s_rail_dash[i] = d;
    }
    lv_obj_set_hidden(s_rail, true);

    for (lv_indev_t *in = lv_indev_get_next(NULL); in != NULL; in = lv_indev_get_next(in)) {
        if (lv_indev_get_type(in) == LV_INDEV_TYPE_POINTER) {
            lv_indev_add_event_cb(in, indev_cb, LV_EVENT_PRESSED, NULL);
            lv_indev_add_event_cb(in, indev_cb, LV_EVENT_RELEASED, NULL);
        }
    }
}

void onurb_ui_set_threads(const onurb_thread_t *t, int n)
{
    if (!s_ready || s_th[TH_MAX - 1].reply == NULL) {
        return;
    }
    if (t == NULL || n < 0) {
        n = 0;
    }
    if (n > TH_MAX) {
        n = TH_MAX;
    }
    bsp_display_lock(0);
    if (!s_th_used) {
        s_th_used = true;
        lv_obj_set_hidden(s_pend, true); /* the rail replaces the pending dashes */
    }
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (t[i].id == 0) {
            continue;
        }
        s_th[k].id = t[i].id;
        s_th[k].state = t[i].state;
        copy_utf8(s_th[k].heard, t[i].heard, TH_TEXT);
        copy_utf8(s_th[k].reply, t[i].reply, TH_TEXT);
        copy_utf8(s_th[k].hist, t[i].hist, TH_HIST);
        s_th[k].addable = t[i].addable;
        k++;
    }
    s_th_n = k;
    if (s_th_cancel_sent != 0) {
        int c = th_find(s_th_cancel_sent);
        if (c < 0 || s_th[c].state != ONURB_THREAD_PROCESSING) {
            s_th_cancel_sent = 0;
        }
    }
    int cur = th_find(s_view_id);
    if (s_view_id != 0 && cur < 0) {
        th_go(0, 0); /* the open thread is gone */
    } else if (cur >= 0) {
        thv_render(&s_th[cur]);
        rail_update();
    } else {
        rail_update();
    }
    bsp_display_unlock();
}

uint32_t onurb_ui_thread_reply_taken(void)
{
    uint32_t id = s_mic_taken;

    s_mic_taken = 0;
    return id;
}

uint32_t onurb_ui_thread_cancel_taken(void)
{
    uint32_t id = s_th_cancel_taken;

    s_th_cancel_taken = 0;
    return id;
}

uint32_t onurb_ui_thread_view(void)
{
    return s_view_id;
}

bool onurb_ui_open_thread(uint32_t id)
{
    if (!s_ready) {
        return false;
    }
    bsp_display_lock(0);
    /* Not over the WhatsApp/purchase preview or the Wi-Fi picker: the caller retries later. */
    bool ok = th_find(id) >= 0 && s_wp == NULL && (s_wa == NULL || lv_obj_is_hidden(s_wa));
    if (ok && s_view_id != id) {
        th_go(id, +1);
    }
    bsp_display_unlock();
    return ok;
}

bool onurb_ui_rec_stop_taken(void)
{
    bool v = s_rec_stop;
    s_rec_stop = false;
    return v;
}

bool onurb_ui_rec_cancel_taken(void)
{
    bool v = s_rec_cancel;
    s_rec_cancel = false;
    return v;
}

bool onurb_ui_main_view(void)
{
    return s_view_id == 0 && s_wp == NULL && (s_wa == NULL || lv_obj_is_hidden(s_wa));
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

    /* Title "Espaçado fino": thin, widely tracked wordmark with a short bar.
     * Trailing letter-space shifts the text left, so nudge right by half. */
    s_title = make_label(scr, &lv_font_montserrat_28, COLOR_TITLE, LV_ALIGN_TOP_MID, TITLE_Y);
    lv_obj_set_style_text_letter_space(s_title, TITLE_TRACKING, 0);
    lv_label_set_text(s_title, "ONURB");
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, TITLE_TRACKING / 2, TITLE_Y);

    s_title_bar = make_box(scr);
    lv_obj_set_size(s_title_bar, 36, 3);
    lv_obj_set_style_radius(s_title_bar, 2, 0);
    lv_obj_set_style_bg_color(s_title_bar, lv_color_white(), 0);
    lv_obj_align(s_title_bar, LV_ALIGN_TOP_MID, 0, TITLE_BAR_Y);

    make_battery(scr);
    make_wifi(scr);

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
        lv_obj_align(ring, LV_ALIGN_CENTER, 0, DOT_Y);
        s_rings[i] = ring;
    }

    s_dot = lv_obj_create(scr);
    lv_obj_set_size(s_dot, DOT_SIZE, DOT_SIZE);
    lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_dot, 0, 0);
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(COLOR_IDLE), 0);
    lv_obj_set_scrollable(s_dot, false);
    lv_obj_align(s_dot, LV_ALIGN_CENTER, 0, DOT_Y);

    s_status = make_label(scr, &lv_font_montserrat_28, 0xFFFFFF, LV_ALIGN_BOTTOM_MID, -100);
    s_hint = make_label(scr, &lv_font_montserrat_20, 0x808080, LV_ALIGN_BOTTOM_MID, -60);
    lv_label_set_text(s_status, "");
    lv_label_set_text(s_hint, HINT_IDLE);

    make_cancel();

    s_upd_arc = lv_arc_create(scr);
    lv_obj_set_size(s_upd_arc, UPD_SIZE, UPD_SIZE);
    lv_obj_align(s_upd_arc, LV_ALIGN_CENTER, 0, DOT_Y);
    lv_obj_remove_style(s_upd_arc, NULL, LV_PART_KNOB);
    lv_obj_set_clickable(s_upd_arc, false);
    lv_obj_set_style_arc_width(s_upd_arc, UPD_WIDTH, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_upd_arc, lv_color_hex(COLOR_UPD_TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_upd_arc, UPD_WIDTH, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_upd_arc, lv_color_white(), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_upd_arc, true, LV_PART_INDICATOR);
    lv_arc_set_bg_angles(s_upd_arc, 0, 360);
    lv_arc_set_range(s_upd_arc, 0, 100);
    lv_obj_set_hidden(s_upd_arc, true);

    s_upd_pct = lv_label_create(scr);
    lv_obj_set_style_text_font(s_upd_pct, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_upd_pct, lv_color_white(), 0);
    lv_label_set_text(s_upd_pct, "");
    lv_obj_align(s_upd_pct, LV_ALIGN_CENTER, 0, DOT_Y);
    lv_obj_set_hidden(s_upd_pct, true);

    s_reply = lv_obj_create(scr);
    lv_obj_set_size(s_reply, REPLY_W, BSP_LCD_V_RES - REPLY_TOP - REPLY_BOTTOM);
    lv_obj_align(s_reply, LV_ALIGN_TOP_MID, 0, REPLY_TOP);
    lv_obj_set_style_bg_opa(s_reply, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_reply, 0, 0);
    lv_obj_set_style_pad_all(s_reply, 0, 0);
    lv_obj_set_style_pad_row(s_reply, REPLY_GAP, 0);
    lv_obj_set_flex_flow(s_reply, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_reply, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(s_reply, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_reply, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_hidden(s_reply, true);

    make_threads(scr);  /* thread view over the main view; rail made here too */
    make_whatsapp(scr); /* over everything except the pending bars */
    make_pending(scr);  /* last: always on top */
    lv_obj_move_foreground(s_rail);
    lv_obj_add_event_cb(s_wifi, wifi_icon_cb, LV_EVENT_CLICKED, NULL);
    bsp_display_unlock();

    s_ready = true;
    ESP_LOGI(TAG, "screen on (brightness %d%%)", CONFIG_ONURB_SCREEN_BRIGHTNESS);
    return ESP_OK;
}

void onurb_ui_set_battery(int percent, bool charging, bool usb_in)
{
    char buf[16];
    int w = 0;
    uint32_t fill = COLOR_BATT_FILL;

    if (!s_ready) {
        return;
    }
    if (percent < 0) {
        snprintf(buf, sizeof(buf), "--");
    } else {
        if (percent > 100) {
            percent = 100;
        }
        snprintf(buf, sizeof(buf), "%d%s", percent, (charging || usb_in) ? LV_SYMBOL_CHARGE : "");
        w = BATT_W * percent / 100;
        if (charging) {
            fill = COLOR_BATT_CHG;
        } else if (percent <= 20) {
            fill = COLOR_BATT_LOW;
        }
    }
    bsp_display_lock(0);
    lv_label_set_text(s_batt_text, buf);
    lv_obj_set_width(s_batt_fill, w);
    lv_obj_set_style_bg_color(s_batt_fill, lv_color_hex(fill), 0);
    bsp_display_unlock();
}

void onurb_ui_set_wifi(onurb_ui_wifi_t level)
{
    uint32_t color = COLOR_WIFI_OFF;

    if (!s_ready) {
        return;
    }
    if (level == ONURB_UI_WIFI_ONLINE) {
        color = COLOR_ICON_ON;
    } else if (level == ONURB_UI_WIFI_CONNECTED) {
        color = COLOR_WIFI_WAIT;
    }
    bsp_display_lock(0);
    lv_obj_set_style_text_color(s_wifi, lv_color_hex(color), 0);
    bsp_display_unlock();
}

/* Caller holds the display lock. Long messages ("Offline - will send later")
 * drop to the 20 px font and wrap so they don't clip on the rounded panel. */
#define STATUS_BIG_CHARS 18
#define STATUS_W         360

static void status_text(const char *text)
{
    bool small = strlen(text) > STATUS_BIG_CHARS;

    lv_obj_set_style_text_font(s_status, small ? &lv_font_montserrat_20 : &lv_font_montserrat_28, 0);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(s_status, small ? STATUS_W : LV_SIZE_CONTENT);
    lv_label_set_text(s_status, text);
    lv_obj_align(s_status, LV_ALIGN_BOTTOM_MID, 0, -100);
}

static void reply_style(lv_obj_t *label, bool newest)
{
    const char *text = lv_label_get_text(label);
    bool big = newest && strlen(text) <= REPLY_BIG_CHARS;

    lv_obj_set_style_text_font(label, big ? &onurb_font_reply_26 : &onurb_font_reply_20, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(newest ? 0xFFFFFF : COLOR_REPLY_OLD), 0);
}

/* Caller holds the display lock. Adds utf8 on top of the history. */
static void reply_push(const char *utf8)
{
    if (lv_obj_get_child_count(s_reply) >= REPLY_MAX) {
        lv_obj_delete(lv_obj_get_child(s_reply, -1));
    }
    lv_obj_t *prev = lv_obj_get_child(s_reply, 0);
    if (prev != NULL) {
        reply_style(prev, false);
    }
    lv_obj_t *label = lv_label_create(s_reply);
    lv_obj_set_width(label, REPLY_W);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(label, 2, 0);
    lv_label_set_text(label, utf8);
    lv_obj_move_to_index(label, 0);
    reply_style(label, true);
}

/* Caller holds the display lock. Shows the history (newest on top) or, with
 * on = false, hides it and brings the idle screen back. History is kept. */
static void reply_view(bool on)
{
    lv_obj_set_hidden(s_reply, !on);
    lv_obj_set_hidden(s_title, on);
    lv_obj_set_hidden(s_title_bar, on);
    lv_obj_set_hidden(s_status, on);
    lv_obj_set_hidden(s_dot, on);
    lv_obj_set_hidden(s_hint, on);
    if (!on) {
        return;
    }
    ripple_set(false);
    for (int i = 0; i < RING_COUNT; i++) {
        lv_obj_set_hidden(s_rings[i], true);
    }
    lv_obj_update_layout(s_reply);
    lv_obj_scroll_to_y(s_reply, 0, LV_ANIM_OFF);
}

/* Caller holds the display lock. Text pushes + shows; NULL or "" hides. */
static void reply_show(const char *utf8)
{
    if (utf8 == NULL || utf8[0] == '\0') {
        reply_view(false);
        return;
    }
    reply_push(utf8);
    reply_view(true);
}

static void upd_spin_cb(void *obj, int32_t v)
{
    lv_arc_set_rotation(obj, v);
}

/* Caller holds the display lock. percent: -1 spin, 0-100 progress. */
static void updating_show(int percent)
{
    bool spin = percent < 0;

    if (!s_updating) {
        s_updating = true;
        reply_show(NULL);
        ripple_set(false);
        lv_obj_set_hidden(s_hint, true);
        lv_obj_set_style_bg_color(s_dot, lv_color_hex(COLOR_IDLE), 0);
        lv_obj_set_hidden(s_upd_arc, false);
        lv_obj_set_hidden(s_upd_pct, false);
        cancel_set(ONURB_UI_IDLE);
    }
    lv_anim_delete(s_upd_arc, upd_spin_cb);
    if (spin) {
        lv_arc_set_mode(s_upd_arc, LV_ARC_MODE_NORMAL);
        lv_arc_set_angles(s_upd_arc, 0, 90);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_upd_arc);
        lv_anim_set_exec_cb(&a, upd_spin_cb);
        lv_anim_set_values(&a, 0, 360);
        lv_anim_set_duration(&a, UPD_SPIN_MS);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
        lv_label_set_text(s_upd_pct, "");
        status_text("Updating...");
        return;
    }
    if (percent > 100) {
        percent = 100;
    }
    lv_arc_set_rotation(s_upd_arc, 270); /* start at 12 o'clock */
    lv_arc_set_value(s_upd_arc, percent);
    lv_label_set_text_fmt(s_upd_pct, "%d%%", percent);
    status_text(percent >= 100 ? "Restarting..." : "Updating...");
}

/* Caller holds the display lock. */
static void updating_hide(void)
{
    if (!s_updating) {
        return;
    }
    s_updating = false;
    lv_anim_delete(s_upd_arc, upd_spin_cb);
    lv_obj_set_hidden(s_upd_arc, true);
    lv_obj_set_hidden(s_upd_pct, true);
    lv_obj_set_hidden(s_hint, false);
}

void onurb_ui_show_updating(int percent)
{
    if (!s_ready) {
        return;
    }
    bsp_display_lock(0);
    updating_show(percent);
    bsp_display_unlock();
}

void onurb_ui_set_reply(const char *utf8)
{
    if (!s_ready) {
        return;
    }
    bsp_display_lock(0);
    reply_show(utf8);
    bsp_display_unlock();
}

void onurb_ui_clear_replies(void)
{
    if (!s_ready) {
        return;
    }
    bsp_display_lock(0);
    lv_obj_clean(s_reply);
    reply_view(false);
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
    case ONURB_UI_PROCESSING:
        snprintf(buf, sizeof(buf), "Processing...");
        color = COLOR_BUSY;
        break;
    case ONURB_UI_CANCELLED:
        snprintf(buf, sizeof(buf), "Cancelled");
        color = COLOR_DOT_CANCELLED;
        break;
    case ONURB_UI_IDLE:
    default:
        buf[0] = '\0';                     /* idle: no "Ready" text (Bruno, 2026-11-11) */
        break;
    }
    bsp_display_lock(0);
    status_text(buf);
    lv_obj_set_style_text_color(s_status, lv_color_white(), 0);
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(color), 0);
    ripple_set(state == ONURB_UI_RECORDING);
    updating_hide();
    wa_hide();
    if (state == ONURB_UI_RECORDING && s_state == ONURB_UI_RECORDING) {
        /* timer refresh (every 100 ms): leave the view alone, the user may be scrolling threads */
    } else if (state == ONURB_UI_RECORDING) {
        wp_close(); /* talking takes over the screen; a pending join carries on */
        if (s_mic_rec_id != 0 && s_mic_rec_id == s_view_id) {
            s_mic_recording = true; /* answering this thread: stay on it */
            mic_style();
        } else {
            s_mic_rec_id = 0;
            if (s_view_id != 0) {
                th_go(0, 0);
            }
        }
    } else if (s_mic_recording) {
        s_mic_recording = false;
        s_mic_rec_id = 0;
        mic_style();
    }
    if (state != ONURB_UI_PLAYING) {
        reply_show(NULL);
    }
    /* A tap must survive unrelated state changes until the main task polls
     * it; only a fresh entry into PROCESSING resets it. */
    if ((state == ONURB_UI_PROCESSING || state == ONURB_UI_CANCELLED) && s_state != state) {
        s_cancel_taken = false; /* the tap that cancelled must not count again */
    }
    s_state = state;
    cancel_set(state);
    bsp_display_unlock();
}

void onurb_ui_set_status(const char *text, uint32_t color)
{
    if (!s_ready) {
        return;
    }
    bsp_display_lock(0);
    status_text(text);
    lv_obj_set_style_text_color(s_status,
                                lv_color_hex(color == ONURB_UI_COLOR_ERROR ? COLOR_ERROR_TXT : 0xFFFFFF), 0);
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(color), 0);
    ripple_set(false);
    cancel_set(ONURB_UI_IDLE);
    bsp_display_unlock();
}
