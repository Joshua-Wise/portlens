#include "raw_frame.h"
#include "protocols/lldp_parser.h"
#include "protocols/cdp_parser.h"
#include "protocols/eapol_detect.h"

#include "esp_netif.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "raw_frame";

// LLDP EtherType
#define ETHERTYPE_LLDP  0x88CC
// EAPOL EtherType (IEEE 802.1X)
#define ETHERTYPE_EAPOL 0x888E

// CDP multicast destination MAC
static const uint8_t CDP_MCAST_ADDR[6] = {0x01, 0x00, 0x0C, 0xCC, 0xCC, 0xCC};

// CDP SNAP header validation bytes (LLC AA:AA:03 + OUI 00:00:0C + PID 20:00)
static const uint8_t CDP_SNAP_HEADER[8] = {0xAA, 0xAA, 0x03, 0x00, 0x00, 0x0C, 0x20, 0x00};

// ETH_HEADER_LEN (14) is provided by esp_eth_spec.h via esp_eth.h
// SNAP header after 802.3 length field: 8 bytes
#define SNAP_HEADER_LEN 8
// CDP fixed header after SNAP: version(1) + TTL(1) + checksum(2) = 4 bytes
#define CDP_FIXED_HDR   4

esp_err_t raw_frame_recv(esp_eth_handle_t eth_handle,
                         uint8_t *buffer, uint32_t length, void *priv)
{
    if (length < ETH_HEADER_LEN) {
        free(buffer);
        return ESP_OK;
    }

    const uint8_t *dst_mac = buffer;
    uint16_t ethertype = (buffer[12] << 8) | buffer[13];

    // --- LLDP: EtherType 0x88CC ---
    if (ethertype == ETHERTYPE_LLDP) {
        ESP_LOGI(TAG, "LLDP frame received (%lu bytes)", (unsigned long)length);
        lldp_parse(buffer + ETH_HEADER_LEN, length - ETH_HEADER_LEN);
        free(buffer);
        return ESP_OK;
    }

    // --- EAPOL: EtherType 0x888E (IEEE 802.1X) ---
    if (ethertype == ETHERTYPE_EAPOL) {
        ESP_LOGI(TAG, "EAPOL frame received (%lu bytes)", (unsigned long)length);
        eapol_process(buffer + ETH_HEADER_LEN, length - ETH_HEADER_LEN);
        // Forward to stack — supplicant may need it
        goto forward;
    }

    // --- CDP: Dst MAC 01:00:0C:CC:CC:CC with SNAP encapsulation ---
    // CDP uses 802.3 framing so the "EtherType" field is actually a length.
    // We identify CDP by the destination multicast MAC instead.
    if (memcmp(dst_mac, CDP_MCAST_ADDR, 6) == 0) {
        // Verify we have enough data for the SNAP header
        uint32_t snap_offset = ETH_HEADER_LEN;
        if (length < snap_offset + SNAP_HEADER_LEN + CDP_FIXED_HDR) {
            ESP_LOGW(TAG, "CDP frame too short (%lu bytes)", (unsigned long)length);
            free(buffer);
            return ESP_OK;
        }

        // Validate the SNAP header
        if (memcmp(buffer + snap_offset, CDP_SNAP_HEADER, SNAP_HEADER_LEN) != 0) {
            ESP_LOGD(TAG, "Non-CDP frame to CDP multicast address, ignoring");
            // Not a CDP frame — fall through to forward to stack
            goto forward;
        }

        uint32_t cdp_offset = snap_offset + SNAP_HEADER_LEN;
        ESP_LOGI(TAG, "CDP frame received (%lu bytes)", (unsigned long)length);
        cdp_parse(buffer + cdp_offset, length - cdp_offset);
        free(buffer);
        return ESP_OK;
    }

forward:
    // Forward all non-protocol frames to the TCP/IP stack.
    // esp_netif_receive() takes ownership of the buffer — do NOT free it.
    if (priv) {
        return esp_netif_receive((esp_netif_t *)priv, buffer, length, NULL);
    }
    free(buffer);
    return ESP_OK;
}
