#pragma once

/* Full-screen yes/no feedback after a trained-gesture match: green (left)
 * = correct, red (right) = wrong. Blocks the caller until a tap or timeout.
 * Returns 1 (green), 0 (red) or -1 (timeout). Restores the previous screen. */
int onurb_ui_feedback_ask(int timeout_ms);
