#pragma once

#include <stdbool.h>
#include "esp_netif.h"

typedef enum {
    TEST_NOT_RUN = 0,
    TEST_RUNNING,
    TEST_PASS,
    TEST_FAIL,
} test_status_t;

typedef struct {
    test_status_t ping_gw;
    test_status_t dns;
    test_status_t http;
} connectivity_results_t;

/**
 * Start connectivity tests on a background FreeRTOS task.
 * Tests run sequentially: ping gateway → DNS resolve → HTTP GET.
 * Safe to call multiple times — restarts tests if already complete.
 *
 * @param gateway  Gateway IP address to ping (from DHCP lease)
 * @param netif    Network interface (used for ping bind)
 */
void connectivity_start_tests(const esp_ip4_addr_t *gateway, esp_netif_t *netif);

/**
 * Get current test results. Always returns a valid pointer
 * (results may show TEST_NOT_RUN if tests haven't started).
 */
const connectivity_results_t *connectivity_get_results(void);

/**
 * Reset all test results to TEST_NOT_RUN (call on link down).
 */
void connectivity_reset(void);
