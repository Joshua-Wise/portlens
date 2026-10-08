#include "eth_init.h"
#include "raw_frame.h"
#include "config.h"

#include "esp_eth.h"
#include "esp_eth_mac.h"
#include "esp_eth_phy.h"
#include "esp_eth_netif_glue.h"
#include "esp_netif.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "driver/gpio.h"

static const char *TAG = "eth_init";

static esp_eth_handle_t s_eth_handle = NULL;
static esp_netif_t     *s_eth_netif  = NULL;

static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;

    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED: {
        uint8_t mac[6];
        esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac);
        ESP_LOGI(TAG, "Link Up — MAC: %02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

        eth_speed_t speed;
        eth_duplex_t duplex;
        esp_eth_ioctl(eth_handle, ETH_CMD_G_SPEED, &speed);
        esp_eth_ioctl(eth_handle, ETH_CMD_G_DUPLEX_MODE, &duplex);
        ESP_LOGI(TAG, "Link Speed: %s, Duplex: %s",
                 (speed == ETH_SPEED_100M) ? "100M" : "10M",
                 (duplex == ETH_DUPLEX_FULL) ? "Full" : "Half");
        break;
    }
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Link Down");
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet driver started");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "Ethernet driver stopped");
        break;
    default:
        break;
    }
}

esp_err_t eth_init(void)
{
    ESP_LOGI(TAG, "Initializing Ethernet on %s", BOARD_NAME);

    // --- Detect WROOM vs WROVER module ---
    // WROVER has PSRAM on GPIO 16/17 — those pins are NOT available externally.
    // The RMII clock on GPIO 17 would go to PSRAM instead of the PHY!
    // WROVER needs: ETH_CLK as external input on GPIO 0 (PHY crystal provides clock).
    bool is_wrover = esp_psram_is_initialized();
    if (is_wrover) {
        ESP_LOGW(TAG, "WROVER module detected (PSRAM present)!");
        ESP_LOGW(TAG, "GPIO 16/17 are internal to PSRAM — using GPIO 0 for RMII clock input");
    } else {
        ESP_LOGI(TAG, "WROOM module (no PSRAM) — using GPIO 17 for RMII clock output");
    }

    // --- PHY power cycle ---
#ifdef ETH_PHY_PWR_GPIO
    gpio_config_t pwr_cfg = {
        .pin_bit_mask = (1ULL << ETH_PHY_PWR_GPIO),
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&pwr_cfg);
    gpio_set_level(ETH_PHY_PWR_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(ETH_PHY_PWR_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "PHY power ON (GPIO %d=%d, MDIO=%d)",
             ETH_PHY_PWR_GPIO, gpio_get_level(ETH_PHY_PWR_GPIO),
             gpio_get_level(ETH_MDIO_GPIO));
#endif

    // --- MAC configuration ---
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t esp32_emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    esp32_emac_config.smi_gpio.mdc_num  = ETH_MDC_GPIO;
    esp32_emac_config.smi_gpio.mdio_num = ETH_MDIO_GPIO;

    if (is_wrover) {
        // WROVER: GPIO 17 is internal to PSRAM, output APLL clock on GPIO 0 instead
        esp32_emac_config.clock_config.rmii.clock_mode = EMAC_CLK_OUT;
        esp32_emac_config.clock_config.rmii.clock_gpio = EMAC_APPL_CLK_OUT_GPIO;  // GPIO 0
    }
    // else: WROOM defaults from sdkconfig are correct (CLK_OUT on GPIO 17)

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&esp32_emac_config, &mac_config);
    if (!mac) {
        ESP_LOGE(TAG, "Failed to create ESP32 EMAC");
        return ESP_FAIL;
    }

    // --- PHY configuration ---
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr         = ETH_PHY_ADDR;
    phy_config.reset_gpio_num   = ETH_PHY_PWR_GPIO;  // GPIO 12 — ESP-IDF handles power cycle
    phy_config.reset_timeout_ms = 5000;

    esp_eth_phy_t *phy = esp_eth_phy_new_lan87xx(&phy_config);
    if (!phy) {
        ESP_LOGE(TAG, "Failed to create LAN87xx PHY");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Installing driver (clock=%s, timeout=5000ms)...",
             is_wrover ? "EXT_IN GPIO0" : "CLK_OUT GPIO17");
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_err_t install_err = esp_eth_driver_install(&eth_config, &s_eth_handle);
    if (install_err != ESP_OK) {
        ESP_LOGE(TAG, "Driver install failed (0x%x) — module=%s",
                 install_err, is_wrover ? "WROVER" : "WROOM");

        // Quick diagnostic scan
        for (int addr = 0; addr < 4; addr++) {
            uint32_t bmcr = 0, id1 = 0, id2 = 0;
            mac->read_phy_reg(mac, addr, 0, &bmcr);
            mac->read_phy_reg(mac, addr, 2, &id1);
            mac->read_phy_reg(mac, addr, 3, &id2);
            ESP_LOGW(TAG, "  Addr%d: BMCR=0x%04lX ID1=0x%04lX ID2=0x%04lX",
                     addr, (unsigned long)bmcr, (unsigned long)id1, (unsigned long)id2);
        }
        if (!is_wrover) {
            ESP_LOGE(TAG, "If this is actually a WROVER module, GPIO 17 goes to PSRAM not PHY!");
            ESP_LOGE(TAG, "Check your module: does it have PSRAM? Look for 'WROVER' on the label.");
        }
        return install_err;
    }
    ESP_LOGI(TAG, "Ethernet driver installed successfully");

    // --- Event handlers ---
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                   &eth_event_handler, NULL),
        TAG, "Event handler registration failed");

    // --- TCP/IP netif with DHCP client ---
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&netif_cfg);
    if (!s_eth_netif) {
        ESP_LOGE(TAG, "Failed to create esp_netif for Ethernet");
        return ESP_FAIL;
    }

    // Attach glue layer — bridges Ethernet driver ↔ TCP/IP stack
    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(s_eth_handle);
    ESP_RETURN_ON_ERROR(esp_netif_attach(s_eth_netif, glue),
                        TAG, "Failed to attach netif glue");

    // --- Raw frame capture ---
    ESP_RETURN_ON_ERROR(
        esp_eth_update_input_path(s_eth_handle, raw_frame_recv, s_eth_netif),
        TAG, "Failed to set raw frame callback");

    // LLDP (01:80:C2:00:00:0E), CDP (01:00:0C:CC:CC:CC) and EAPOL
    // (01:80:C2:00:00:03) are all multicast. ESP-IDF <= 5.4 passed all
    // multicast by default; 5.5+ filters it, so enable it explicitly.
    bool all_multicast = true;
    ESP_RETURN_ON_ERROR(
        esp_eth_ioctl(s_eth_handle, ETH_CMD_S_ALL_MULTICAST, &all_multicast),
        TAG, "Failed to enable all-multicast receive");

    // --- Start ---
    ESP_RETURN_ON_ERROR(esp_eth_start(s_eth_handle), TAG, "Ethernet start failed");

    // --- Diagnostic: dump key PHY registers ---
    vTaskDelay(pdMS_TO_TICKS(500));
    {
        esp_eth_phy_reg_rw_data_t reg_data;
        uint32_t val = 0;
        static const struct { int reg; const char *name; } regs[] = {
            {0, "BMCR"}, {1, "BMSR"}, {2, "PHYID1"}, {3, "PHYID2"},
            {4, "ANAR"}, {5, "ANLPAR"}, {6, "ANER"}, {31, "PSCSR"},
        };
        for (int i = 0; i < (int)(sizeof(regs)/sizeof(regs[0])); i++) {
            val = 0;
            reg_data.reg_addr = regs[i].reg;
            reg_data.reg_value_p = &val;
            if (esp_eth_ioctl(s_eth_handle, ETH_CMD_READ_PHY_REG, &reg_data) == ESP_OK) {
                ESP_LOGI(TAG, "PHY Reg %2d (%s): 0x%04lX", regs[i].reg, regs[i].name, (unsigned long)val);
            }
        }
    }

    ESP_LOGI(TAG, "Ethernet initialized — waiting for link");
    return ESP_OK;
}

esp_eth_handle_t eth_get_handle(void)
{
    return s_eth_handle;
}

esp_netif_t *eth_get_netif(void)
{
    return s_eth_netif;
}
