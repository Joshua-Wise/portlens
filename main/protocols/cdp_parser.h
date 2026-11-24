#pragma once

#include <stdint.h>
#include <stdbool.h>

#define CDP_MAX_STR  256

// Parsed CDP information
typedef struct {
    bool     received;          // true once we've parsed at least one CDP frame
    uint8_t  version;
    uint8_t  ttl;

    char     device_id[CDP_MAX_STR];    // 0x0001 — switch hostname
    char     port_id[CDP_MAX_STR];      // 0x0003 — switch port name
    char     platform[CDP_MAX_STR];     // 0x0006 — switch model
    uint16_t native_vlan;               // 0x000A — access VLAN
    bool     has_native_vlan;

    // Management address (0x0016)
    bool     has_mgmt_addr;
    uint8_t  mgmt_addr[16];
    uint8_t  mgmt_addr_len;
    uint8_t  mgmt_addr_proto;           // 1 = NLPID (IPv4), 2 = 802.2

    // Power (0x001A)
    bool     has_power;
    uint32_t power_available_mw;        // milliwatts
} cdp_info_t;

/**
 * Parse a CDP payload (after the SNAP header).
 * Input starts at: [version(1)][TTL(1)][checksum(2)][TLVs...]
 */
void cdp_parse(const uint8_t *data, uint32_t length);

/**
 * Get a pointer to the most recently parsed CDP info.
 * Returns NULL if no CDP frame has been received yet.
 */
const cdp_info_t *cdp_get_info(void);

/** Forget the last CDP neighbour (call on link down). */
void cdp_reset(void);
