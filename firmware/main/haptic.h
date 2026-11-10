#pragma once

/* Vibration motor (GPIO18, found in the factory firmware's Vibration test app).
 * Non-blocking short patterns. The motor is plain on/off (no PWM), so
 * "lighter" means a shorter pulse: the motor barely spins up. */
typedef enum {
    ONURB_HAPTIC_TAP,      /* generic short tap */
    ONURB_HAPTIC_NOTIFY,   /* a reply or a preview arrived */
    ONURB_HAPTIC_DOUBLE,   /* done (message sent) */
    ONURB_HAPTIC_ERROR,    /* something failed */
    ONURB_HAPTIC_REC_START, /* very light single tick: mic is live */
    ONURB_HAPTIC_REC_STOP,  /* very light double tick: capture ended */
    ONURB_HAPTIC_REPLY,     /* very light triple tick: assistant reply arrived */
    ONURB_HAPTIC_COUNT,
} onurb_haptic_t;

void onurb_haptic_init(void);
void onurb_haptic(onurb_haptic_t pattern);
