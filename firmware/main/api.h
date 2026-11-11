#pragma once

#include <stdbool.h>
#include "esp_http_client.h"

/* Gateway endpoints (docs/SERVER.md). Two bases:
 *   local  = CONFIG_ONURB_API_BASE_URL   (the Mac's gateway on home Wi-Fi, plain HTTP)
 *   remote = CONFIG_ONURB_API_REMOTE_URL (the server, HTTPS behind Cloudflare Access)
 * Local first; a connection failure switches to the other one, and the local one
 * is tried again after a while. The server forwards to the Mac when it's up. */
const char *onurb_api_base(void);

/* Authorization (gateway token) and, for the remote base, the Cloudflare Access
 * service-token headers. */
void onurb_api_headers(esp_http_client_handle_t client, const char *base);

/* A request to `base` could not connect (no HTTP status): switch bases.
 * Returns true if there is another base to retry on. */
bool onurb_api_failed(const char *base);
