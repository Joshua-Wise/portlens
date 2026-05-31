#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_netif_types.h"

typedef struct {
    bool     valid;           // true once DHCP lease received
    char     ip[16];          // e.g. "192.168.1.100"
    char     netmask[16];
    char     gateway[16];
    char     dns[16];         // primary DNS server
} dhcp_info_t;

/**
 * Save DHCP lease info from an IP_EVENT_ETH_GOT_IP event.
 * Extracts IP/mask/GW from the event data and queries the primary DNS server.
 */
void dhcp_info_save(const esp_netif_ip_info_t *ip_info, esp_netif_t *netif);

/**
 * Get current DHCP info. Returns NULL if no lease has been received.
 */
const dhcp_info_t *dhcp_info_get(void);

/**
 * Reset DHCP info (call on link down).
 */
void dhcp_info_reset(void);

#include "config.h"
#if FEATURE_DEMO_MODE
/** Inject fake DHCP info for demo / UI iteration. */
void dhcp_info_set_demo(void);
#endif
