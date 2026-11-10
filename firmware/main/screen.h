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
