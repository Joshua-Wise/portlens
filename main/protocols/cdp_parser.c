#include "cdp_parser.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "cdp";

// CDP TLV type codes
#define CDP_TLV_DEVICE_ID       0x0001
#define CDP_TLV_ADDRESSES       0x0002
#define CDP_TLV_PORT_ID         0x0003
#define CDP_TLV_CAPABILITIES    0x0004
#define CDP_TLV_SOFTWARE_VER    0x0005
#define CDP_TLV_PLATFORM        0x0006
#define CDP_TLV_NATIVE_VLAN     0x000A
#define CDP_TLV_MGMT_ADDR       0x0016
#define CDP_TLV_POWER_AVAILABLE 0x001A

// Persistent parsed data
static cdp_info_t s_cdp_info;

// Copy TLV value as null-terminated string
static void tlv_to_str(char *dst, size_t dst_size,
                       const uint8_t *value, uint16_t value_len)
{
    size_t copy_len = (value_len < dst_size - 1) ? value_len : dst_size - 1;
    memcpy(dst, value, copy_len);
    dst[copy_len] = '\0';
}

// Parse a CDP address structure.
// CDP addresses TLV format:
//   number_of_addresses (4 bytes, big-endian)
//   For each address:
//     protocol_type (1 byte): 1=NLPID, 2=802.2
//     protocol_length (1 byte)
//     protocol (N bytes): for IPv4 NLPID, this is 0xCC (1 byte)
//     address_length (2 bytes, big-endian)
//     address (N bytes)
static void parse_addresses(const uint8_t *value, uint16_t len, bool is_mgmt)
{
    if (len < 4) return;

    uint32_t num_addrs = ((uint32_t)value[0] << 24) | ((uint32_t)value[1] << 16) |
                         ((uint32_t)value[2] << 8)  | value[3];
    uint32_t offset = 4;

    for (uint32_t i = 0; i < num_addrs && offset < len; i++) {
        if (offset + 2 > len) break;
        uint8_t proto_type = value[offset];
        uint8_t proto_len  = value[offset + 1];
        offset += 2;

        if (offset + proto_len > len) break;
        // Skip protocol bytes (e.g., 0xCC for IPv4 NLPID)
        offset += proto_len;

        if (offset + 2 > len) break;
        uint16_t addr_len = (value[offset] << 8) | value[offset + 1];
        offset += 2;

        if (offset + addr_len > len) break;

        // Store the first address we find
        if (i == 0) {
            if (is_mgmt) {
                s_cdp_info.mgmt_addr_proto = proto_type;
                uint8_t copy_len = (addr_len > sizeof(s_cdp_info.mgmt_addr))
                                       ? sizeof(s_cdp_info.mgmt_addr) : addr_len;
                memcpy(s_cdp_info.mgmt_addr, value + offset, copy_len);
                s_cdp_info.mgmt_addr_len = copy_len;
                s_cdp_info.has_mgmt_addr = true;
            }
        }

        offset += addr_len;
    }
}

// Parse CDP Power Available TLV (0x001A)
// Format: request-id(4) + management-id(4) + power-available(4) [+ ...]
// All values in milliwatts, big-endian
static void parse_power_available(const uint8_t *value, uint16_t len)
{
    if (len >= 12) {
        s_cdp_info.power_available_mw =
            ((uint32_t)value[8] << 24) | ((uint32_t)value[9] << 16) |
            ((uint32_t)value[10] << 8) | value[11];
        s_cdp_info.has_power = true;
    }
}

// Log all parsed CDP fields
static void log_cdp_info(void)
{
    ESP_LOGI(TAG, "========== CDP INFO ===========");
    ESP_LOGI(TAG, "  Version:    %u", s_cdp_info.version);
    ESP_LOGI(TAG, "  TTL:        %u sec", s_cdp_info.ttl);

    if (s_cdp_info.device_id[0]) {
        ESP_LOGI(TAG, "  Device ID:  %s", s_cdp_info.device_id);
    }
    if (s_cdp_info.port_id[0]) {
        ESP_LOGI(TAG, "  Port ID:    %s", s_cdp_info.port_id);
    }
    if (s_cdp_info.platform[0]) {
        ESP_LOGI(TAG, "  Platform:   %s", s_cdp_info.platform);
    }
    if (s_cdp_info.has_native_vlan) {
        ESP_LOGI(TAG, "  Native VLAN: %u", s_cdp_info.native_vlan);
    }

    if (s_cdp_info.has_mgmt_addr && s_cdp_info.mgmt_addr_len >= 4) {
        ESP_LOGI(TAG, "  Mgmt Addr:  %u.%u.%u.%u",
                 s_cdp_info.mgmt_addr[0], s_cdp_info.mgmt_addr[1],
                 s_cdp_info.mgmt_addr[2], s_cdp_info.mgmt_addr[3]);
    }

    if (s_cdp_info.has_power) {
        ESP_LOGI(TAG, "  PoE Power:  %lu mW (%.1f W)",
                 (unsigned long)s_cdp_info.power_available_mw,
                 s_cdp_info.power_available_mw / 1000.0);
    }
    ESP_LOGI(TAG, "===============================");
}

void cdp_parse(const uint8_t *data, uint32_t length)
{
    // CDP fixed header: version(1) + TTL(1) + checksum(2) = 4 bytes
    if (length < 4) {
        ESP_LOGW(TAG, "CDP frame too short (%lu bytes)", (unsigned long)length);
        return;
    }

    // Clear previous data
    memset(&s_cdp_info, 0, sizeof(s_cdp_info));

    s_cdp_info.version = data[0];
    s_cdp_info.ttl     = data[1];
    // Checksum at data[2..3] — we trust it for now, skip verification.

    uint32_t offset = 4;

    while (offset + 4 <= length) {
        uint16_t tlv_type = (data[offset] << 8) | data[offset + 1];
        uint16_t tlv_len  = (data[offset + 2] << 8) | data[offset + 3];

        // Length includes the 4-byte type+length header
        if (tlv_len < 4 || offset + tlv_len > length) {
            ESP_LOGW(TAG, "CDP TLV 0x%04X bad length %u at offset %lu",
                     tlv_type, tlv_len, (unsigned long)offset);
            break;
        }

        const uint8_t *value = data + offset + 4;
        uint16_t value_len = tlv_len - 4;

        switch (tlv_type) {
        case CDP_TLV_DEVICE_ID:
            tlv_to_str(s_cdp_info.device_id, sizeof(s_cdp_info.device_id),
                       value, value_len);
            break;

        case CDP_TLV_PORT_ID:
            tlv_to_str(s_cdp_info.port_id, sizeof(s_cdp_info.port_id),
                       value, value_len);
            break;

        case CDP_TLV_PLATFORM:
            tlv_to_str(s_cdp_info.platform, sizeof(s_cdp_info.platform),
                       value, value_len);
            break;

        case CDP_TLV_NATIVE_VLAN:
            if (value_len >= 2) {
                s_cdp_info.native_vlan = (value[0] << 8) | value[1];
                s_cdp_info.has_native_vlan = true;
            }
            break;

        case CDP_TLV_ADDRESSES:
            parse_addresses(value, value_len, false);
            break;

        case CDP_TLV_MGMT_ADDR:
            parse_addresses(value, value_len, true);
            break;

        case CDP_TLV_POWER_AVAILABLE:
            parse_power_available(value, value_len);
            break;

        default:
            ESP_LOGD(TAG, "Skipping CDP TLV 0x%04X (len %u)", tlv_type, value_len);
            break;
        }

        offset += tlv_len;
    }

    s_cdp_info.received = true;
    log_cdp_info();
}

const cdp_info_t *cdp_get_info(void)
{
    return s_cdp_info.received ? &s_cdp_info : NULL;
}
