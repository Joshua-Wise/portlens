#pragma once

#include "esp_eth.h"

typedef enum {
    CABLE_QUALITY_UNKNOWN = 0,
    CABLE_QUALITY_GOOD,     // 100M FDX negotiated, both sides advertise 100M
    CABLE_QUALITY_FAIR,     // 100M negotiated but HDX, or some capability mismatch
    CABLE_QUALITY_POOR,     // Only 10M negotiated despite higher capability
} cable_quality_t;

typedef struct {
    bool valid;

    // Our advertised capabilities (PHY register 4, ANAR)
    bool local_100m_fdx;
    bool local_100m_hdx;
    bool local_10m_fdx;
    bool local_10m_hdx;

    // Link partner capabilities (PHY register 5, ANLPAR)
    bool partner_100m_fdx;
    bool partner_100m_hdx;
    bool partner_10m_fdx;
    bool partner_10m_hdx;

    // Negotiation result
    bool autoneg_complete;
    bool link_up;

    // Quality assessment
    cable_quality_t quality;
    const char *quality_str;
} cable_test_result_t;

/**
 * Run a cable quality assessment by reading PHY auto-negotiation registers.
 * Call after link is up and autoneg is complete.
 */
void cable_test_run(esp_eth_handle_t eth);

/**
 * Get the most recent cable test result.
 * Returns NULL if no test has been run.
 */
const cable_test_result_t *cable_test_get_result(void);

/**
 * Reset cable test state (call on link down).
 */
void cable_test_reset(void);
