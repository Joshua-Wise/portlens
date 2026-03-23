#pragma once

#include "esp_eth.h"

/**
 * Raw Ethernet frame receive callback.
 * Registered via esp_eth_update_input_path() — inspects every incoming frame
 * and dispatches LLDP/CDP frames to their respective parsers.
 *
 * @param eth_handle  Ethernet driver handle
 * @param buffer      Frame data (caller-allocated, we must free it)
 * @param length      Frame length in bytes
 * @param priv        esp_netif_t* for forwarding non-protocol frames to TCP/IP stack
 */
esp_err_t raw_frame_recv(esp_eth_handle_t eth_handle,
                         uint8_t *buffer, uint32_t length, void *priv);
