#pragma once

#include "config.h"
#include "esp_err.h"

#if FEATURE_WIFI_AP

/**
 * Start the WiFi AP and HTTP server for the captive portal.
 * SSID: "PortLens-XXXX" (last 4 of MAC)
 * No password (open network for easy access).
 * Serves test results as a responsive web page.
 *
 * Call once from app_main() after esp_netif_init() and NVS init.
 */
esp_err_t web_server_start(void);

#endif // FEATURE_WIFI_AP
