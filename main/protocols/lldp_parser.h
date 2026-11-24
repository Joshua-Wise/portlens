#pragma once

#include <stdint.h>
#include <stdbool.h>

// Maximum field lengths for parsed LLDP data
#define LLDP_MAX_STR     256
#define LLDP_MAX_DESC    512
#define LLDP_MAX_VLAN_NAME 64

// LLDP Chassis ID subtypes
#define LLDP_CHASSIS_COMPONENT   1
#define LLDP_CHASSIS_IFACE_ALIAS 2
#define LLDP_CHASSIS_PORT_COMP   3
#define LLDP_CHASSIS_MAC_ADDR    4
#define LLDP_CHASSIS_NET_ADDR    5
#define LLDP_CHASSIS_IFACE_NAME  6
#define LLDP_CHASSIS_LOCAL       7

// LLDP Port ID subtypes
#define LLDP_PORT_IFACE_ALIAS    1
#define LLDP_PORT_PORT_COMP      2
#define LLDP_PORT_MAC_ADDR       3
#define LLDP_PORT_NET_ADDR       4
#define LLDP_PORT_IFACE_NAME     5
#define LLDP_PORT_CIRCUIT_ID     6
#define LLDP_PORT_LOCAL          7

// Parsed LLDP information
typedef struct {
    bool received;              // true once we've parsed at least one LLDP frame

    // Mandatory TLVs
    uint8_t chassis_id_subtype;
    char    chassis_id[LLDP_MAX_STR];
    uint8_t port_id_subtype;
    char    port_id[LLDP_MAX_STR];
    uint16_t ttl;

    // Optional TLVs
    char    port_desc[LLDP_MAX_STR];
    char    system_name[LLDP_MAX_STR];
    char    system_desc[LLDP_MAX_DESC];

    // Management address (TLV type 8)
    bool    has_mgmt_addr;
    uint8_t mgmt_addr_subtype;  // 1=IPv4, 2=IPv6
    uint8_t mgmt_addr[16];
    uint8_t mgmt_addr_len;

    // Org-specific: IEEE 802.1 Port VLAN ID
    bool     has_vlan;
    uint16_t port_vlan_id;

    // Org-specific: IEEE 802.1 VLAN Name
    bool    has_vlan_name;
    uint16_t vlan_name_id;
    char    vlan_name[LLDP_MAX_VLAN_NAME];

    // Org-specific: IEEE 802.3 Power via MDI
    bool    has_poe;
    uint8_t poe_mdi_power_support; // raw byte: bits indicate capability
} lldp_info_t;

/**
 * Parse an LLDP PDU (the payload after the Ethernet header).
 * Results are stored in an internal static struct and logged via ESP_LOG.
 */
void lldp_parse(const uint8_t *data, uint32_t length);

/**
 * Get a pointer to the most recently parsed LLDP info.
 * Returns NULL if no LLDP frame has been received yet.
 */
const lldp_info_t *lldp_get_info(void);

/** Forget the last LLDP neighbour (call on link down). */
void lldp_reset(void);

/**
 * Best human-readable port name for the display.
 *
 * Some switches (HPE/Aruba ProCurve 5400/3800/2930 etc.) send the Port ID as
 * a locally-assigned ifIndex number (e.g. "127") and carry the real port name
 * ("E22") in the Port Description TLV. When the Port ID is locally assigned
 * or purely numeric and a Port Description exists, return the description;
 * otherwise return the Port ID. Never returns NULL (may return "").
 */
const char *lldp_port_label(const lldp_info_t *info);

/** True when lldp_port_label() substituted Port Description for Port ID. */
bool lldp_port_label_is_desc(const lldp_info_t *info);

#include "config.h"
#if FEATURE_DEMO_MODE
/** Inject fake LLDP info for demo / UI iteration. */
void lldp_info_set_demo(void);
#endif
