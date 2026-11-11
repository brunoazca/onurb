#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "api.h"

static const char *TAG = "onurb_api";

#ifndef CONFIG_ONURB_API_REMOTE_URL
#define CONFIG_ONURB_API_REMOTE_URL ""
#endif
#ifndef CONFIG_ONURB_CF_ACCESS_ID
#define CONFIG_ONURB_CF_ACCESS_ID ""
#endif
#ifndef CONFIG_ONURB_CF_ACCESS_SECRET
#define CONFIG_ONURB_CF_ACCESS_SECRET ""
#endif

#define LOCAL_RETRY_US (5LL * 60 * 1000000)       /* on the remote base: try home again every 5 min */

static volatile bool s_remote;
static int64_t s_remote_since_us;

static bool have_remote(void)
{
    return CONFIG_ONURB_API_REMOTE_URL[0] != '\0';
}

const char *onurb_api_base(void)
{
    if (s_remote && esp_timer_get_time() - s_remote_since_us > LOCAL_RETRY_US) {
        s_remote = false;                          /* back home? the next request finds out */
    }
    return s_remote && have_remote() ? CONFIG_ONURB_API_REMOTE_URL : CONFIG_ONURB_API_BASE_URL;
}

void onurb_api_headers(esp_http_client_handle_t client, const char *base)
{
    if (strlen(CONFIG_ONURB_API_TOKEN) > 0) {
        char auth[128];
        snprintf(auth, sizeof(auth), "Bearer %s", CONFIG_ONURB_API_TOKEN);
        esp_http_client_set_header(client, "Authorization", auth);
    }
    if (have_remote() && base && strcmp(base, CONFIG_ONURB_API_REMOTE_URL) == 0 && CONFIG_ONURB_CF_ACCESS_ID[0]) {
        esp_http_client_set_header(client, "CF-Access-Client-Id", CONFIG_ONURB_CF_ACCESS_ID);
        esp_http_client_set_header(client, "CF-Access-Client-Secret", CONFIG_ONURB_CF_ACCESS_SECRET);
    }
}

bool onurb_api_failed(const char *base)
{
    if (!have_remote()) {
        return false;
    }
    bool was_remote = strcmp(base, CONFIG_ONURB_API_REMOTE_URL) == 0;
    if (was_remote == s_remote) {                 /* not already switched by another request */
        s_remote = !was_remote;
        s_remote_since_us = esp_timer_get_time();
        ESP_LOGW(TAG, "%s gateway unreachable - using the %s one", was_remote ? "remote" : "local",
                 was_remote ? "local" : "remote");
    }
    return true;
}
