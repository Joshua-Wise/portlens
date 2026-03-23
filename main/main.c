#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "esp_eth.h"
#include "esp_heap_caps.h"

#include "config.h"
#include "eth/eth_init.h"
#include "protocols/lldp_parser.h"
#include "protocols/cdp_parser.h"
#include "protocols/dhcp_info.h"
#include "protocols/eapol_detect.h"
#include "tests/connectivity.h"
#include "tests/cable_test.h"
#include "web/web_server.h"
#include "storage/sd_card.h"
#include "storage/sd_log.h"
#include "esp_mac.h"

#if FEATURE_DISPLAY
#include "ui/lvgl_ui.h"
#include "ui/screens.h"
#endif

#if FEATURE_BUTTON_NAV
#include "ui/button.h"
#endif

static const char *TAG = "lldpocket";

// Application states
typedef enum {
    STATE_BOOT,
    STATE_WAIT_FOR_LINK,
    STATE_LINK_UP,
} app_state_t;

static volatile app_state_t s_state = STATE_BOOT;

// Ethernet event handler — updates application state on link changes
static void app_eth_event_handler(void *arg, esp_event_base_t event_base,
                                  int32_t event_id, void *event_data)
{
    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        s_state = STATE_LINK_UP;
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        s_state = STATE_WAIT_FOR_LINK;
        sd_log_link_down();
        dhcp_info_reset();
        connectivity_reset();
        eapol_reset();
        cable_test_reset();
        break;
    default:
        break;
    }
}

// IP event handler — called when DHCP lease is obtained
static void app_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data)
{
    if (event_id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));

        esp_netif_t *netif = eth_get_netif();
        dhcp_info_save(&event->ip_info, netif);

        // Trigger connectivity tests with the gateway address
        connectivity_start_tests(&event->ip_info.gw, netif);
    }
}

// Read PHY link speed and duplex, log to serial, and populate link_info
static void read_phy_info(link_info_t *link)
{
    esp_eth_handle_t eth = eth_get_handle();
    if (!eth) return;

    eth_speed_t speed;
    eth_duplex_t duplex;
    esp_eth_ioctl(eth, ETH_CMD_G_SPEED, &speed);
    esp_eth_ioctl(eth, ETH_CMD_G_DUPLEX_MODE, &duplex);

    ESP_LOGI(TAG, "PHY Link: %s %s",
             (speed == ETH_SPEED_100M) ? "100 Mbps" : "10 Mbps",
             (duplex == ETH_DUPLEX_FULL) ? "Full-Duplex" : "Half-Duplex");

    link->link_up = true;
    link->is_100m = (speed == ETH_SPEED_100M);
    link->full_duplex = (duplex == ETH_DUPLEX_FULL);

    // Read raw LAN8720 PHY registers for additional detail
    uint32_t reg_val = 0;
    esp_eth_phy_reg_rw_data_t reg_data;

    // Register 1 (Basic Status): bit 2 = link status, bit 5 = autoneg complete
    reg_data.reg_addr = 1;
    reg_data.reg_value_p = &reg_val;
    if (esp_eth_ioctl(eth, ETH_CMD_READ_PHY_REG, &reg_data) == ESP_OK) {
        ESP_LOGI(TAG, "PHY Reg 1 (Basic Status): 0x%04lX [link=%s, autoneg=%s]",
                 (unsigned long)reg_val,
                 (reg_val & (1 << 2)) ? "up" : "down",
                 (reg_val & (1 << 5)) ? "complete" : "in-progress");
    }

    // Register 31 (PHY Special Control/Status): bits 4:2 = speed indication
    reg_val = 0;
    reg_data.reg_addr = 31;
    reg_data.reg_value_p = &reg_val;
    if (esp_eth_ioctl(eth, ETH_CMD_READ_PHY_REG, &reg_data) == ESP_OK) {
        uint8_t speed_ind = (reg_val >> 2) & 0x07;
        const char *speed_str;
        switch (speed_ind) {
        case 0x01: speed_str = "10BASE-T half-duplex";  break;
        case 0x05: speed_str = "10BASE-T full-duplex";  break;
        case 0x02: speed_str = "100BASE-TX half-duplex"; break;
        case 0x06: speed_str = "100BASE-TX full-duplex"; break;
        default:   speed_str = "unknown";                break;
        }
        ESP_LOGI(TAG, "PHY Reg 31 (Special Status): 0x%04lX [%s]",
                 (unsigned long)reg_val, speed_str);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=================================");
    ESP_LOGI(TAG, " LLDPocket — Pocket LLDP/CDP Tool");
    ESP_LOGI(TAG, " Board: %s", BOARD_NAME);
    ESP_LOGI(TAG, "=================================");

    // Initialize NVS (required by some ESP-IDF components)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Create default event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Initialize TCP/IP stack (required for DHCP, DNS, HTTP)
    ESP_ERROR_CHECK(esp_netif_init());

#if FEATURE_DISPLAY
    // Initialize TFT display + LVGL FIRST (before WiFi/Ethernet, needs DMA-capable internal RAM)
    bool display_ok = (lvgl_ui_init() == ESP_OK);
    if (display_ok) {
        lvgl_ui_show_splash();
        vTaskDelay(pdMS_TO_TICKS(1500));
    } else {
        ESP_LOGW(TAG, "Display init failed — continuing in serial-only mode");
    }
#endif

    // Mount SD card (shares SPI bus with TFT — must init after display)
    if (sd_card_init() == ESP_OK) {
        ESP_LOGI(TAG, "SD card ready at /sdcard/");
        sd_log_init();
        uint8_t mac[6];
        char mac_str[18];
        esp_read_mac(mac, ESP_MAC_ETH);
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        sd_log_boot(mac_str);
    } else {
        ESP_LOGW(TAG, "SD card not available — continuing without logging");
    }

#if FEATURE_BUTTON_NAV
    ESP_ERROR_CHECK(button_init());
#endif

    // Register our app-level Ethernet event handler (for state machine)
    ESP_ERROR_CHECK(esp_event_handler_register(
        ETH_EVENT, ESP_EVENT_ANY_ID, &app_eth_event_handler, NULL));

    // Register IP event handler for DHCP lease acquisition
    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT, IP_EVENT_ETH_GOT_IP, &app_ip_event_handler, NULL));

    // Set initial state before starting Ethernet — eth_init() calls esp_eth_start()
    // which may fire ETHERNET_EVENT_CONNECTED immediately if cable is already plugged
    // in (e.g. PoE-powered boot). Setting state after eth_init() would overwrite it.
    s_state = STATE_WAIT_FOR_LINK;
    ESP_LOGI(TAG, "Waiting for Ethernet link...");

    // Initialize Ethernet — this starts the PHY and raw frame capture
    // EMAC DMA buffers require internal RAM, so this must happen before WiFi
    ESP_ERROR_CHECK(eth_init());

#if FEATURE_WIFI_AP
    // Start WiFi AP AFTER Ethernet — both need internal DMA-capable RAM,
    // and Ethernet is the core function. WiFi AP is optional.
    esp_err_t web_ret = web_server_start();
    if (web_ret != ESP_OK) {
        ESP_LOGW(TAG, "Web server start failed: %s — continuing without WiFi AP",
                 esp_err_to_name(web_ret));
    }
#endif

    // Main loop state
    app_state_t prev_state = STATE_BOOT;
    bool phy_read = false;
    bool got_protocol_data = false;
    bool sd_logged_network = false;

    link_info_t link = {0};
    uint32_t tick_count = 0;

#if FEATURE_DISPLAY
    bool display_dirty = true;    // force initial draw
#endif

    while (1) {
        app_state_t current = s_state;
        bool state_changed = (current != prev_state);

        // ---- State transitions ----
        if (state_changed) {
            switch (current) {
            case STATE_WAIT_FOR_LINK:
                ESP_LOGW(TAG, "STATE: WAIT_FOR_LINK");
                phy_read = false;
                got_protocol_data = false;
                sd_logged_network = false;
                link.link_up = false;
#if FEATURE_DISPLAY
                display_dirty = true;
#endif
                break;
            case STATE_LINK_UP:
                ESP_LOGI(TAG, "STATE: LINK_UP — listening for LLDP/CDP frames...");
#if FEATURE_DISPLAY
                display_dirty = true;
#endif
                break;
            default:
                break;
            }
            prev_state = current;
        }

        // ---- Read PHY info once after link up (give autoneg a moment) ----
        if (current == STATE_LINK_UP && !phy_read) {
            vTaskDelay(pdMS_TO_TICKS(500));
            read_phy_info(&link);
            cable_test_run(eth_get_handle());
            sd_log_link_up(link.is_100m, link.full_duplex);
            phy_read = true;
#if FEATURE_DISPLAY
            display_dirty = true;
#endif
        }

        // ---- Check for new protocol data ----
        if (current == STATE_LINK_UP && !got_protocol_data) {
            if (lldp_get_info() || cdp_get_info() || dhcp_info_get()) {
                got_protocol_data = true;
                ESP_LOGI(TAG, "Protocol data received — showing results");
#if FEATURE_DISPLAY
                display_dirty = true;
#endif
            }
        }

        // ---- SD card: log network data once connectivity tests finish ----
        if (got_protocol_data && !sd_logged_network) {
            const connectivity_results_t *conn = connectivity_get_results();
            // Wait until tests are done (none still RUNNING)
            if (conn->ping_gw != TEST_RUNNING &&
                conn->dns != TEST_RUNNING &&
                conn->http != TEST_RUNNING) {
                sd_log_network_data(lldp_get_info(), cdp_get_info(),
                                    dhcp_info_get(), eapol_get_status(),
                                    conn, cable_test_get_result());
                sd_logged_network = true;
            }
        }

        // ---- Button handling ----
#if FEATURE_BUTTON_NAV
        button_event_t evt = button_poll();
        if (evt == BUTTON_EVENT_SHORT_PRESS || evt == BUTTON_EVENT_LONG_PRESS) {
            ESP_LOGI(TAG, "Button: refresh display");
#if FEATURE_DISPLAY
            display_dirty = true;
#endif
        }
#endif // FEATURE_BUTTON_NAV

        // ---- Display update ----
#if FEATURE_DISPLAY
        if (display_ok) {
            if (current == STATE_WAIT_FOR_LINK) {
                if (display_dirty || state_changed) {
                    lvgl_ui_show_waiting_link();
                    display_dirty = false;
                }
            } else if (current == STATE_LINK_UP && !got_protocol_data) {
                if (display_dirty || state_changed) {
                    lvgl_ui_show_scanning(&link);
                    display_dirty = false;
                }
            } else if (current == STATE_LINK_UP && got_protocol_data) {
                // Refresh card values every ~5s for updated connectivity
                if (display_dirty || (tick_count % 100) == 0) {
                    lvgl_ui_show_screen(SCREEN_SUMMARY, &link);
                    display_dirty = false;
                }
            }
        }
#endif

        // ---- Serial status (periodic, every ~10s) ----
        if ((tick_count % 200) == 0) {
            ESP_LOGI(TAG, "heap: free=%lu min=%lu internal=%lu",
                     (unsigned long)esp_get_free_heap_size(),
                     (unsigned long)esp_get_minimum_free_heap_size(),
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        }

        tick_count++;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
