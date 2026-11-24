#include "lldp_parser.h"
#include "proto_lock.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "lldp";

// TLV type constants
#define TLV_END                 0
#define TLV_CHASSIS_ID          1
#define TLV_PORT_ID             2
#define TLV_TTL                 3
#define TLV_PORT_DESC           4
#define TLV_SYSTEM_NAME         5
#define TLV_SYSTEM_DESC         6
#define TLV_MGMT_ADDR           8
#define TLV_ORG_SPECIFIC        127

// Org-specific OUIs
static const uint8_t OUI_IEEE_8021[3] = {0x00, 0x80, 0xC2}; // IEEE 802.1
static const uint8_t OUI_IEEE_8023[3] = {0x00, 0x12, 0x0F}; // IEEE 802.3

// IEEE 802.1 subtypes
#define IEEE8021_PORT_VLAN_ID   1
#define IEEE8021_VLAN_NAME      3

// IEEE 802.3 subtypes
#define IEEE8023_POWER_VIA_MDI  2

// Persistent parsed data
static lldp_info_t s_lldp_info;

// Format a MAC address into a string buffer. Returns number of chars written.
static int format_mac(char *buf, size_t buflen, const uint8_t *mac)
{
    return snprintf(buf, buflen, "%02X:%02X:%02X:%02X:%02X:%02X",
                    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// Format an IPv4 address into a string buffer.
static int format_ipv4(char *buf, size_t buflen, const uint8_t *ip)
{
    return snprintf(buf, buflen, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

// Copy TLV value bytes as a null-terminated string, clamped to dst_size-1.
static void tlv_to_str(char *dst, size_t dst_size,
                       const uint8_t *value, uint16_t value_len)
{
    size_t copy_len = (value_len < dst_size - 1) ? value_len : dst_size - 1;
    memcpy(dst, value, copy_len);
    dst[copy_len] = '\0';
}

// Parse Chassis ID TLV (type 1)
static void parse_chassis_id(const uint8_t *value, uint16_t len)
{
    if (len < 2) return;

    s_lldp_info.chassis_id_subtype = value[0];
    const uint8_t *id_data = value + 1;
    uint16_t id_len = len - 1;

    switch (s_lldp_info.chassis_id_subtype) {
    case LLDP_CHASSIS_MAC_ADDR:
        if (id_len >= 6) {
            format_mac(s_lldp_info.chassis_id, sizeof(s_lldp_info.chassis_id),
                       id_data);
        }
        break;
    case LLDP_CHASSIS_NET_ADDR:
        // First byte is address family (1=IPv4, 2=IPv6)
        if (id_len >= 5 && id_data[0] == 1) {
            format_ipv4(s_lldp_info.chassis_id, sizeof(s_lldp_info.chassis_id),
                        id_data + 1);
        } else {
            tlv_to_str(s_lldp_info.chassis_id, sizeof(s_lldp_info.chassis_id),
                       id_data, id_len);
        }
        break;
    default:
        // Component, interface alias/name, locally assigned — treat as string
        tlv_to_str(s_lldp_info.chassis_id, sizeof(s_lldp_info.chassis_id),
                   id_data, id_len);
        break;
    }
}

// Parse Port ID TLV (type 2)
static void parse_port_id(const uint8_t *value, uint16_t len)
{
    if (len < 2) return;

    s_lldp_info.port_id_subtype = value[0];
    const uint8_t *id_data = value + 1;
    uint16_t id_len = len - 1;

    switch (s_lldp_info.port_id_subtype) {
    case LLDP_PORT_MAC_ADDR:
        if (id_len >= 6) {
            format_mac(s_lldp_info.port_id, sizeof(s_lldp_info.port_id),
                       id_data);
        }
        break;
    default:
        // Interface alias/name, locally assigned, etc. — treat as string
        tlv_to_str(s_lldp_info.port_id, sizeof(s_lldp_info.port_id),
                   id_data, id_len);
        break;
    }
}

// Parse Management Address TLV (type 8)
static void parse_mgmt_addr(const uint8_t *value, uint16_t len)
{
    // Format: addr_str_len(1) + addr_subtype(1) + addr(N) + iface_subtype(1)
    //         + iface_num(4) + oid_str_len(1) + oid(N)
    if (len < 3) return;

    uint8_t addr_str_len = value[0]; // includes subtype byte
    if (addr_str_len < 2 || (uint16_t)(addr_str_len + 1) > len) return;

    s_lldp_info.mgmt_addr_subtype = value[1];
    uint8_t addr_len = addr_str_len - 1;

    if (addr_len > sizeof(s_lldp_info.mgmt_addr)) {
        addr_len = sizeof(s_lldp_info.mgmt_addr);
    }

    memcpy(s_lldp_info.mgmt_addr, value + 2, addr_len);
    s_lldp_info.mgmt_addr_len = addr_len;
    s_lldp_info.has_mgmt_addr = true;
}

// Parse org-specific TLV (type 127)
static void parse_org_specific(const uint8_t *value, uint16_t len)
{
    // OUI (3 bytes) + subtype (1 byte) + info string
    if (len < 4) return;

    const uint8_t *oui = value;
    uint8_t subtype = value[3];
    const uint8_t *info = value + 4;
    uint16_t info_len = len - 4;

    // --- IEEE 802.1 ---
    if (memcmp(oui, OUI_IEEE_8021, 3) == 0) {
        switch (subtype) {
        case IEEE8021_PORT_VLAN_ID:
            if (info_len >= 2) {
                s_lldp_info.port_vlan_id = (info[0] << 8) | info[1];
                s_lldp_info.has_vlan = true;
            }
            break;
        case IEEE8021_VLAN_NAME:
            // Format: VLAN ID (2 bytes) + name length (1 byte) + name
            if (info_len >= 3) {
                s_lldp_info.vlan_name_id = (info[0] << 8) | info[1];
                uint8_t name_len = info[2];
                if (info_len >= (uint16_t)(3 + name_len)) {
                    tlv_to_str(s_lldp_info.vlan_name,
                               sizeof(s_lldp_info.vlan_name),
                               info + 3, name_len);
                    s_lldp_info.has_vlan_name = true;
                }
            }
            break;
        default:
            ESP_LOGD(TAG, "Unknown 802.1 subtype %u", subtype);
            break;
        }
    }
    // --- IEEE 802.3 ---
    else if (memcmp(oui, OUI_IEEE_8023, 3) == 0) {
        switch (subtype) {
        case IEEE8023_POWER_VIA_MDI:
            if (info_len >= 1) {
                s_lldp_info.poe_mdi_power_support = info[0];
                s_lldp_info.has_poe = true;
            }
            break;
        default:
            ESP_LOGD(TAG, "Unknown 802.3 subtype %u", subtype);
            break;
        }
    }
    else {
        ESP_LOGD(TAG, "Unknown org OUI %02X:%02X:%02X subtype %u",
                 oui[0], oui[1], oui[2], subtype);
    }
}

// Log all parsed LLDP fields
static void log_lldp_info(void)
{
    ESP_LOGI(TAG, "========== LLDP INFO ==========");
    ESP_LOGI(TAG, "  Chassis ID (sub %u): %s",
             s_lldp_info.chassis_id_subtype, s_lldp_info.chassis_id);
    ESP_LOGI(TAG, "  Port ID    (sub %u): %s",
             s_lldp_info.port_id_subtype, s_lldp_info.port_id);
    ESP_LOGI(TAG, "  TTL:        %u sec", s_lldp_info.ttl);

    if (s_lldp_info.port_desc[0]) {
        ESP_LOGI(TAG, "  Port Desc:  %s", s_lldp_info.port_desc);
    }
    if (s_lldp_info.system_name[0]) {
        ESP_LOGI(TAG, "  System Name: %s", s_lldp_info.system_name);
    }
    if (s_lldp_info.system_desc[0]) {
        ESP_LOGI(TAG, "  System Desc: %s", s_lldp_info.system_desc);
    }

    if (s_lldp_info.has_mgmt_addr) {
        if (s_lldp_info.mgmt_addr_subtype == 1 && s_lldp_info.mgmt_addr_len >= 4) {
            ESP_LOGI(TAG, "  Mgmt Addr:  %u.%u.%u.%u",
                     s_lldp_info.mgmt_addr[0], s_lldp_info.mgmt_addr[1],
                     s_lldp_info.mgmt_addr[2], s_lldp_info.mgmt_addr[3]);
        } else {
            ESP_LOGI(TAG, "  Mgmt Addr:  (subtype %u, %u bytes)",
                     s_lldp_info.mgmt_addr_subtype, s_lldp_info.mgmt_addr_len);
        }
    }

    if (s_lldp_info.has_vlan) {
        ESP_LOGI(TAG, "  Port VLAN:  %u", s_lldp_info.port_vlan_id);
    }
    if (s_lldp_info.has_vlan_name) {
        ESP_LOGI(TAG, "  VLAN Name:  %u (%s)",
                 s_lldp_info.vlan_name_id, s_lldp_info.vlan_name);
    }
    if (s_lldp_info.has_poe) {
        uint8_t ps = s_lldp_info.poe_mdi_power_support;
        ESP_LOGI(TAG, "  PoE MDI:    support=0x%02X [%s%s%s%s]", ps,
                 (ps & 0x01) ? "port-class " : "",
                 (ps & 0x02) ? "PSE-supported " : "",
                 (ps & 0x04) ? "PSE-enabled " : "",
                 (ps & 0x08) ? "pair-ctrl " : "");
    }
    ESP_LOGI(TAG, "===============================");
}

void lldp_parse(const uint8_t *data, uint32_t length)
{
    proto_lock();

    // Clear previous data on each new frame
    memset(&s_lldp_info, 0, sizeof(s_lldp_info));

    uint32_t offset = 0;

    while (offset + 2 <= length) {
        // TLV header: 7-bit type, 9-bit length
        uint16_t header = (data[offset] << 8) | data[offset + 1];
        uint8_t  tlv_type = (header >> 9) & 0x7F;
        uint16_t tlv_len  = header & 0x01FF;
        offset += 2;

        // Bounds check
        if (offset + tlv_len > length) {
            ESP_LOGW(TAG, "TLV type %u overflows frame (need %u, have %lu)",
                     tlv_type, tlv_len, (unsigned long)(length - offset));
            break;
        }

        const uint8_t *value = data + offset;

        switch (tlv_type) {
        case TLV_END:
            goto done;

        case TLV_CHASSIS_ID:
            parse_chassis_id(value, tlv_len);
            break;

        case TLV_PORT_ID:
            parse_port_id(value, tlv_len);
            break;

        case TLV_TTL:
            if (tlv_len >= 2) {
                s_lldp_info.ttl = (value[0] << 8) | value[1];
            }
            break;

        case TLV_PORT_DESC:
            tlv_to_str(s_lldp_info.port_desc, sizeof(s_lldp_info.port_desc),
                       value, tlv_len);
            break;

        case TLV_SYSTEM_NAME:
            tlv_to_str(s_lldp_info.system_name, sizeof(s_lldp_info.system_name),
                       value, tlv_len);
            break;

        case TLV_SYSTEM_DESC:
            tlv_to_str(s_lldp_info.system_desc, sizeof(s_lldp_info.system_desc),
                       value, tlv_len);
            break;

        case TLV_MGMT_ADDR:
            parse_mgmt_addr(value, tlv_len);
            break;

        case TLV_ORG_SPECIFIC:
            parse_org_specific(value, tlv_len);
            break;

        default:
            ESP_LOGD(TAG, "Skipping TLV type %u (len %u)", tlv_type, tlv_len);
            break;
        }

        offset += tlv_len;
    }

done:
    s_lldp_info.received = true;
    proto_unlock();
    log_lldp_info();
}

const lldp_info_t *lldp_get_info(void)
{
    return s_lldp_info.received ? &s_lldp_info : NULL;
}

void lldp_reset(void)
{
    proto_lock();
    memset(&s_lldp_info, 0, sizeof(s_lldp_info));
    proto_unlock();
}

static bool str_is_numeric(const char *s)
{
    if (!*s) return false;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return false;
    }
    return true;
}

bool lldp_port_label_is_desc(const lldp_info_t *info)
{
    if (!info || !info->port_desc[0]) return false;
    if (!info->port_id[0]) return true;
    return info->port_id_subtype == LLDP_PORT_LOCAL ||
           str_is_numeric(info->port_id);
}

const char *lldp_port_label(const lldp_info_t *info)
{
    if (!info) return "";
    return lldp_port_label_is_desc(info) ? info->port_desc : info->port_id;
}

#if FEATURE_DEMO_MODE
void lldp_info_set_demo(void)
{
    memset(&s_lldp_info, 0, sizeof(s_lldp_info));
    strcpy(s_lldp_info.system_name, "sw-core-01.lab");
    strcpy(s_lldp_info.system_desc, "Catalyst 9300-48P (demo)");
    strcpy(s_lldp_info.port_id,   "Gi1/0/24");
    strcpy(s_lldp_info.port_desc, "Uplink to access");
    s_lldp_info.ttl = 120;
    s_lldp_info.has_vlan = true;
    s_lldp_info.port_vlan_id = 100;
    s_lldp_info.has_vlan_name = true;
    strcpy(s_lldp_info.vlan_name, "CORP-DATA");
    s_lldp_info.has_mgmt_addr = true;
    s_lldp_info.mgmt_addr_subtype = 1;  // IPv4
    s_lldp_info.mgmt_addr_len = 4;
    s_lldp_info.mgmt_addr[0] = 10;
    s_lldp_info.mgmt_addr[1] = 0;
    s_lldp_info.mgmt_addr[2] = 0;
    s_lldp_info.mgmt_addr[3] = 1;
    s_lldp_info.has_poe = true;
    s_lldp_info.poe_mdi_power_support = 0x06;  // supported + enabled
    s_lldp_info.received = true;
}
#endif
