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
#include "proto_lock.h"
#include "eth/eth_init.h"
#include "protocols/lldp_parser.h"
#include "protocols/cdp_parser.h"
#include "protocols/dhcp_info.h"
#include "protocols/eapol_detect.h"
#include "tests/connectivity.h"
#include "web/web_server.h"

#if FEATURE_DISPLAY
#include "ui/lvgl_ui.h"
#endif

static const char *TAG = "portlens";

typedef enum {
    STATE_BOOT,
    STATE_WAIT_FOR_LINK,
    STATE_LINK_UP,
} app_state_t;

static volatile app_state_t s_state = STATE_BOOT;

#if !FEATURE_DEMO_MODE
static void app_eth_event_handler(void *arg, esp_event_base_t event_base,
                                  int32_t event_id, void *event_data)
{
    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        s_state = STATE_LINK_UP;
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        s_state = STATE_WAIT_FOR_LINK;
        dhcp_info_reset();
        connectivity_reset();
        eapol_reset();
        break;
    default:
        break;
    }
}

static void app_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data)
{
    if (event_id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));

        esp_netif_t *netif = eth_get_netif();
        dhcp_info_save(&event->ip_info, netif);

        connectivity_start_tests(&event->ip_info.gw, netif);
    }
}
#endif // !FEATURE_DEMO_MODE

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
}

void app_main(void)
{
    ESP_LOGI(TAG, "=================================");
    ESP_LOGI(TAG, " PortLens — Pocket Ethernet Tester");
    ESP_LOGI(TAG, " Board: %s", BOARD_NAME);
    ESP_LOGI(TAG, "=================================");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    proto_lock_init();

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());

#if FEATURE_DISPLAY
    bool display_ok = (lvgl_ui_init() == ESP_OK);
    if (display_ok) {
        lvgl_ui_show_splash();
        vTaskDelay(pdMS_TO_TICKS(1500));
    } else {
        ESP_LOGW(TAG, "Display init failed — continuing in serial-only mode");
    }
#endif

#if FEATURE_DEMO_MODE
    ESP_LOGW(TAG, "DEMO MODE — skipping eth_init, loading fake LLDP/DHCP data");
    lldp_info_set_demo();
    dhcp_info_set_demo();
    s_state = STATE_LINK_UP;
#else
    ESP_ERROR_CHECK(esp_event_handler_register(
        ETH_EVENT, ESP_EVENT_ANY_ID, &app_eth_event_handler, NULL));

    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT, IP_EVENT_ETH_GOT_IP, &app_ip_event_handler, NULL));

    // Set initial state before eth_init() — esp_eth_start() may fire
    // ETHERNET_EVENT_CONNECTED immediately if cable is already plugged in.
    s_state = STATE_WAIT_FOR_LINK;
    ESP_LOGI(TAG, "Waiting for Ethernet link...");

    ESP_ERROR_CHECK(eth_init());
#endif

#if FEATURE_WIFI_AP
    esp_err_t web_ret = web_server_start();
    if (web_ret != ESP_OK) {
        ESP_LOGW(TAG, "Web server start failed: %s — continuing without WiFi AP",
                 esp_err_to_name(web_ret));
    }
#endif

    app_state_t prev_state = STATE_BOOT;
    bool phy_read = false;
    bool got_protocol_data = false;

    link_info_t link = {0};
    uint32_t tick_count = 0;

#if FEATURE_DEMO_MODE
    // Fake a 100M full-duplex link; skip real PHY read in the main loop.
    link.link_up = true;
    link.is_100m = true;
    link.full_duplex = true;
    phy_read = true;
#endif

#if FEATURE_DISPLAY
    bool display_dirty = true;
#endif

    while (1) {
        app_state_t current = s_state;
        bool state_changed = (current != prev_state);

        if (state_changed) {
            switch (current) {
            case STATE_WAIT_FOR_LINK:
                ESP_LOGW(TAG, "STATE: WAIT_FOR_LINK");
                phy_read = false;
                got_protocol_data = false;
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

        if (current == STATE_LINK_UP && !phy_read) {
            vTaskDelay(pdMS_TO_TICKS(500));
            read_phy_info(&link);
            phy_read = true;
#if FEATURE_DISPLAY
            display_dirty = true;
#endif
        }

        if (current == STATE_LINK_UP && !got_protocol_data) {
            if (lldp_get_info() || cdp_get_info() || dhcp_info_get()) {
                got_protocol_data = true;
                ESP_LOGI(TAG, "Protocol data ready — showing results");
#if FEATURE_DISPLAY
                display_dirty = true;
#endif
            }
        }

#if FEATURE_DISPLAY
        if (display_ok) {
            if (current == STATE_WAIT_FOR_LINK) {
                if (display_dirty || state_changed) {
                    lvgl_ui_show_waiting_link();
                    display_dirty = false;
                }
            } else if (current == STATE_LINK_UP && got_protocol_data) {
                // Refresh every ~5s so DHCP / later LLDP frames show up
                if (display_dirty || (tick_count % 100) == 0) {
                    lvgl_ui_show_data(&link);
                    display_dirty = false;
                }
            }
            // STATE_LINK_UP && !got_protocol_data: leave splash overlay up
            // until data arrives — avoids a flicker from overlay→scanning→data.
        }
#endif

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
