#pragma once

#include <stdbool.h>

/* Screen power policy: off by default; on while something is happening
 * (gesture feedback, button turn, capture), off again after
 * CONFIG_ONURB_SCREEN_IDLE_S of idle. Only touches BSP brightness. */

/* Keep the screen on (nestable); release starts the idle countdown. */
void onurb_screen_hold(void);
void onurb_screen_release(void);

/* Call periodically from the main loop; turns the screen off when idle. */
void onurb_screen_tick(void);

/* Panel lit (held, idle countdown, or woken by a double tap on the dark screen). */
bool onurb_screen_is_on(void);

/* "scr_dtap=N;scr_rej=N;scr_hold=N": double taps that lit the screen, touches
 * rejected as resting contact/flicker, and times the firmware lit it (holds). */
void onurb_screen_stats(char *buf, int len);

/* Idle seconds before the screen goes off (CONFIG_ONURB_SCREEN_IDLE_S on the
 * main view; longer while reading a thread). Any touch restarts the countdown. */
void onurb_screen_set_idle_s(int seconds);
void onurb_screen_touch(void);
