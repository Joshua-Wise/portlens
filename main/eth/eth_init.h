#pragma once

#include <stdbool.h>
#include "esp_eth.h"
#include "esp_netif.h"

/**
 * Initialize the Ethernet driver (ESP32 internal MAC + LAN8720 PHY).
 * Creates esp_netif with DHCP client, attaches glue layer, then overrides
 * the input path with raw_frame_recv for LLDP/CDP inspection.
 * Call this once from app_main() after esp_netif_init().
 */
esp_err_t eth_init(void);

/**
 * Get the Ethernet driver handle (for ioctl calls, PHY register reads, etc.).
 * Returns NULL if eth_init() hasn't been called yet.
 */
esp_eth_handle_t eth_get_handle(void);

/**
 * Get the esp_netif handle for the Ethernet interface.
 * Returns NULL if eth_init() hasn't been called yet.
 */
esp_netif_t *eth_get_netif(void);

/** True while the Ethernet link is up. */
bool eth_link_is_up(void);
