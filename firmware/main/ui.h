#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    ONURB_UI_IDLE,
    ONURB_UI_RECORDING,
    ONURB_UI_PLAYING,
    ONURB_UI_PROCESSING, /* recording done, turn in flight: "Processing...", no ripple */
    ONURB_UI_CANCELLED,  /* red circle + X; a tap means "that gesture was a false trigger" */
} onurb_ui_state_t;

#define ONURB_UI_HAS_PROCESSING 1
#define ONURB_UI_HAS_CANCELLED 1

/* OTA screen: a ring around the circle, spinning for percent < 0 ("Updating...")
 * or filling with 0-100 (% in the circle; 100 shows "Restarting..."). Any
 * onurb_ui_set_state() hides it (e.g. IDLE after a failed update). */
#define ONURB_UI_HAS_UPDATING 1
void onurb_ui_show_updating(int percent);

/* In ONURB_UI_PROCESSING and ONURB_UI_CANCELLED the main circle shows an X and
 * is tappable. Returns true once per tap (latched; cleared on read and when
 * set_state enters PROCESSING or CANCELLED from another state). */
#define ONURB_UI_HAS_CANCEL 1
bool onurb_ui_cancel_taken(void);

/* Start the display + LVGL via the BSP and draw the status screen.
 * Requires bsp_i2c_init() first (touch shares the bus). */
esp_err_t onurb_ui_start(void);

/* percent < 0 means no battery detected. */
void onurb_ui_set_battery(int percent, bool charging, bool usb_in);
typedef enum {
    ONURB_UI_WIFI_OFF,       /* not configured or connecting: red */
    ONURB_UI_WIFI_CONNECTED, /* associated, internet not confirmed: yellow */
    ONURB_UI_WIFI_ONLINE,    /* SNTP confirmed internet: white */
} onurb_ui_wifi_t;

void onurb_ui_set_wifi(onurb_ui_wifi_t level);

/* Assistant replies (UTF-8, Latin-1 glyphs incl. pt-BR accents). Text adds a
 * reply on top of a scrollable history (newest first, up to 8) and shows it
 * full screen - title, circle and status hide. NULL/"" hides the view (the
 * history is kept), as does any onurb_ui_set_state() other than PLAYING.
 * onurb_ui_clear_replies() empties the history. */
#define ONURB_UI_HAS_REPLY 1
#define ONURB_UI_HAS_REPLY_LIST 1
void onurb_ui_set_reply(const char *utf8);
void onurb_ui_clear_replies(void);
void onurb_ui_set_state(onurb_ui_state_t state, float seconds);

/* Circle tones (gray family); errors also turn the status text red. */
#define ONURB_UI_COLOR_BUSY  0x9E9E9E
#define ONURB_UI_COLOR_ERROR 0x5A5A5A

/* Free-form status line + circle color (e.g. "Thinking...", errors). */
void onurb_ui_set_status(const char *text, uint32_t color);

/* WhatsApp send-approval preview: full-screen modal with the contact (96x96
 * RGB565 little-endian avatar, or NULL for an initial), the proposed messages
 * as outgoing bubbles (copied; n <= 8), and Edit / Cancel / Confirm buttons.
 * Any onurb_ui_set_state() hides it. onurb_ui_action_taken() returns the
 * tapped button once (latched; cleared on read and on show_whatsapp). */
#define ONURB_UI_HAS_WHATSAPP 1
enum {
    ONURB_UI_ACTION_NONE = 0,
    ONURB_UI_ACTION_CONFIRM = 1,
    ONURB_UI_ACTION_CANCEL = 2,
    ONURB_UI_ACTION_EDIT = 3,
};
void onurb_ui_show_whatsapp(const char *name, const uint16_t *avatar_rgb565, const char *const *msgs, int n);
/* Purchase approval (Onurb shopping): same modal with the product photo
 * (img_px x img_px RGB565, 96 or 120; NULL = none) in a rounded square, the
 * title, then msgs[0] (the total) large and the rest (considerations) as
 * gray notes; Cancel / Confirm only. onurb_ui_action_taken() as above. */
void onurb_ui_show_purchase(const char *title, const uint16_t *img_rgb565, int img_px, const char *const *msgs,
                            int n);
int onurb_ui_action_taken(void);

/* Pending-request indicator: short horizontal dashes in a column on the left
 * edge, max 6 total: `processing` light dashes first, then `queued` (offline
 * spool) dark-gray ones; hidden when both are 0. Drawn above every view,
 * including the reply history and the WhatsApp modal. */
#define ONURB_UI_HAS_PENDING 1
void onurb_ui_set_pending(int processing, int queued);

/* Threads: one per request, newest first (max ONURB_UI_MAX_THREADS; extra
 * ones are ignored). The left rail shows a white dot (main view) above one
 * horizontal dash per thread, colored by state (OPEN = yellow), with the
 * open view highlighted. Swiping up on the main view opens the newest
 * thread, further swipes walk to older ones, swiping down walks back to the
 * main view (a long thread body scrolls first). Each thread shows what was
 * heard, a spinner while PROCESSING/WORKING, and its reply; OPEN threads add
 * a mic button. The UI copies the strings (NULL/"" = not known yet; each is
 * cut to ONURB_UI_THREAD_TEXT_MAX bytes). If the open thread disappears
 * from the list, the view goes back to the main screen. This supersedes
 * onurb_ui_set_pending() (ignored once set_threads has been called) and the
 * reply history. */
#define ONURB_UI_HAS_THREADS 1
#define ONURB_UI_MAX_THREADS 10
#define ONURB_UI_THREAD_TEXT_MAX 511
typedef enum {
    ONURB_THREAD_PROCESSING,
    ONURB_THREAD_WORKING, /* background task: also spins */
    ONURB_THREAD_DONE,
    ONURB_THREAD_OPEN,    /* needs the user: yellow + mic */
    ONURB_THREAD_ERROR,
} onurb_thread_state_t;
typedef struct {
    uint32_t id; /* nonzero, stable for the thread's life */
    onurb_thread_state_t state;
    const char *heard;
    const char *reply;
    /* Earlier exchanges of this thread, oldest first, shown above heard/reply:
     * "heard\x1freply\x1e" per exchange (NULL/"" = none). */
    const char *hist;
} onurb_thread_t;
#define ONURB_UI_THREAD_HIST_MAX 2047
void onurb_ui_set_threads(const onurb_thread_t *t, int n);
/* Id of the thread whose mic was tapped (latched; read clears), 0 = none.
 * While that thread stays open, set_state(RECORDING) keeps the thread view
 * (its mic turns red) instead of switching to the main view. */
uint32_t onurb_ui_thread_reply_taken(void);
/* Thread view X (shown only while the open thread is PROCESSING): id of the
 * thread whose X was tapped (latched; read clears), 0 = none. The hint then
 * reads "Cancelling..." until the thread leaves PROCESSING or the list. */
#define ONURB_UI_HAS_THREAD_CANCEL 1
uint32_t onurb_ui_thread_cancel_taken(void);
/* Thread open right now, 0 = main view. */
uint32_t onurb_ui_thread_view(void);
/* While answering a thread by voice: the red mic was tapped (stop and send) or
 * the X beside it (discard, back to "Tap to answer"). Latched; read clears. */
bool onurb_ui_rec_stop_taken(void);
bool onurb_ui_rec_cancel_taken(void);
/* Main view (the circle) vs anything else (a thread, the WhatsApp preview,
 * the Wi-Fi picker): the screen stays on longer outside the main view. */
bool onurb_ui_main_view(void);
