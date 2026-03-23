#pragma once

#include "esp_err.h"
#include <stdbool.h>

#define FEATURE_WIFI_AP  1  // Enable WiFi AP + web interface

#if FEATURE_WIFI_AP

/**
 * Start the WiFi AP and HTTP server for the captive portal.
 * SSID: "LLDPocket-XXXX" (last 4 of MAC)
 * No password (open network for easy access).
 * Serves test results as a responsive web page.
 *
 * Call once from app_main() after esp_netif_init() and NVS init.
 */
esp_err_t web_server_start(void);

/**
 * Stop the WiFi AP and HTTP server.
 */
void web_server_stop(void);

/**
 * Toggle SSID broadcast (hidden vs visible).
 * Returns the new state: true = broadcasting, false = hidden.
 */
bool web_server_toggle_ssid_broadcast(void);

/**
 * Check if the SSID is currently being broadcast.
 */
bool web_server_is_ssid_broadcasting(void);

/**
 * Get the WiFi AP SSID. Returns empty string if WiFi not started.
 */
const char *web_server_get_ssid(void);

#endif // FEATURE_WIFI_AP
