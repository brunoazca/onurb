#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    ONURB_UI_IDLE,
    ONURB_UI_RECORDING,
    ONURB_UI_PLAYING,
} onurb_ui_state_t;

/* Start the display + LVGL via the BSP and draw the status screen.
 * Requires bsp_i2c_init() first (touch shares the bus). */
esp_err_t onurb_ui_start(void);

/* percent < 0 means no battery detected. */
void onurb_ui_set_battery(int percent, bool charging, bool usb_in);
void onurb_ui_set_wifi(const char *text);
void onurb_ui_set_motion(const char *text);
void onurb_ui_set_state(onurb_ui_state_t state, float seconds);

/* Monochrome: busy is mid-gray; errors are dim and rely on the status text. */
#define ONURB_UI_COLOR_BUSY  0x9E9E9E
#define ONURB_UI_COLOR_ERROR 0x5A5A5A

/* Free-form status line + circle color (e.g. "Thinking...", errors). */
void onurb_ui_set_status(const char *text, uint32_t color);
