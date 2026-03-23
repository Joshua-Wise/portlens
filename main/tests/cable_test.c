#include "tests/cable_test.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "cable_test";

// PHY register definitions (IEEE 802.3, LAN8710A/LAN8720)
#define PHY_REG_BMSR    1   // Basic Mode Status Register
#define PHY_REG_ANAR    4   // Auto-Negotiation Advertisement Register
#define PHY_REG_ANLPAR  5   // Auto-Negotiation Link Partner Ability Register

// BMSR bits
#define BMSR_LINK_STATUS     (1 << 2)
#define BMSR_AUTONEG_DONE    (1 << 5)

// ANAR / ANLPAR bits (same bit positions in both registers)
#define AN_10M_HDX           (1 << 5)
#define AN_10M_FDX           (1 << 6)
#define AN_100M_HDX          (1 << 7)
#define AN_100M_FDX          (1 << 8)

static cable_test_result_t s_result = {0};

static esp_err_t read_phy_reg(esp_eth_handle_t eth, int reg_addr, uint32_t *val)
{
    esp_eth_phy_reg_rw_data_t reg_data = {
        .reg_addr = reg_addr,
        .reg_value_p = val,
    };
    return esp_eth_ioctl(eth, ETH_CMD_READ_PHY_REG, &reg_data);
}

void cable_test_run(esp_eth_handle_t eth)
{
    if (!eth) return;

    memset(&s_result, 0, sizeof(s_result));

    uint32_t bmsr = 0, anar = 0, anlpar = 0;

    if (read_phy_reg(eth, PHY_REG_BMSR, &bmsr) != ESP_OK ||
        read_phy_reg(eth, PHY_REG_ANAR, &anar) != ESP_OK ||
        read_phy_reg(eth, PHY_REG_ANLPAR, &anlpar) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read PHY registers");
        return;
    }

    s_result.valid = true;
    s_result.link_up = (bmsr & BMSR_LINK_STATUS) != 0;
    s_result.autoneg_complete = (bmsr & BMSR_AUTONEG_DONE) != 0;

    // Parse local advertised capabilities
    s_result.local_10m_hdx  = (anar & AN_10M_HDX) != 0;
    s_result.local_10m_fdx  = (anar & AN_10M_FDX) != 0;
    s_result.local_100m_hdx = (anar & AN_100M_HDX) != 0;
    s_result.local_100m_fdx = (anar & AN_100M_FDX) != 0;

    // Parse link partner capabilities
    s_result.partner_10m_hdx  = (anlpar & AN_10M_HDX) != 0;
    s_result.partner_10m_fdx  = (anlpar & AN_10M_FDX) != 0;
    s_result.partner_100m_hdx = (anlpar & AN_100M_HDX) != 0;
    s_result.partner_100m_fdx = (anlpar & AN_100M_FDX) != 0;

    ESP_LOGI(TAG, "BMSR=0x%04lX ANAR=0x%04lX ANLPAR=0x%04lX",
             (unsigned long)bmsr, (unsigned long)anar, (unsigned long)anlpar);
    ESP_LOGI(TAG, "Local:   100F=%d 100H=%d 10F=%d 10H=%d",
             s_result.local_100m_fdx, s_result.local_100m_hdx,
             s_result.local_10m_fdx, s_result.local_10m_hdx);
    ESP_LOGI(TAG, "Partner: 100F=%d 100H=%d 10F=%d 10H=%d",
             s_result.partner_100m_fdx, s_result.partner_100m_hdx,
             s_result.partner_10m_fdx, s_result.partner_10m_hdx);

    // Assess cable quality based on negotiation outcome
    if (!s_result.link_up) {
        s_result.quality = CABLE_QUALITY_UNKNOWN;
        s_result.quality_str = "No Link";
    } else if (!s_result.autoneg_complete) {
        s_result.quality = CABLE_QUALITY_UNKNOWN;
        s_result.quality_str = "Negotiating";
    } else if (anlpar == 0) {
        // Link partner didn't respond with capabilities — might be forced mode
        s_result.quality = CABLE_QUALITY_UNKNOWN;
        s_result.quality_str = "No AN data";
    } else {
        // Both sides advertise 100M FDX and we negotiated it — good cable
        bool both_100f = s_result.local_100m_fdx && s_result.partner_100m_fdx;
        bool both_100h = s_result.local_100m_hdx && s_result.partner_100m_hdx;
        bool partner_has_100 = s_result.partner_100m_fdx || s_result.partner_100m_hdx;

        if (both_100f) {
            s_result.quality = CABLE_QUALITY_GOOD;
            s_result.quality_str = "Good";
        } else if (both_100h) {
            // 100M HDX — switch might have duplex forced, or cable marginal
            s_result.quality = CABLE_QUALITY_FAIR;
            s_result.quality_str = "Fair (HDX)";
        } else if (!partner_has_100 && (s_result.partner_10m_fdx || s_result.partner_10m_hdx)) {
            // Partner only offers 10M — likely a cable quality issue or old switch
            s_result.quality = CABLE_QUALITY_POOR;
            s_result.quality_str = "Poor (10M)";
        } else {
            // Some other mismatch
            s_result.quality = CABLE_QUALITY_FAIR;
            s_result.quality_str = "Fair";
        }
    }

    ESP_LOGI(TAG, "Cable quality: %s", s_result.quality_str);
}

const cable_test_result_t *cable_test_get_result(void)
{
    return s_result.valid ? &s_result : NULL;
}

void cable_test_reset(void)
{
    memset(&s_result, 0, sizeof(s_result));
}
