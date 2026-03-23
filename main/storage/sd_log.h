#pragma once

#include <stdbool.h>
#include "protocols/lldp_parser.h"
#include "protocols/cdp_parser.h"
#include "protocols/dhcp_info.h"
#include "protocols/eapol_detect.h"
#include "tests/connectivity.h"
#include "tests/cable_test.h"

// Initialize the log file on SD card. Call after sd_card_init() succeeds.
// Creates /sdcard/lldpocket.log if it doesn't exist.
void sd_log_init(void);

// Log a boot event (device MAC, board name, uptime)
void sd_log_boot(const char *mac_str);

// Log link up with PHY info
void sd_log_link_up(bool is_100m, bool full_duplex);

// Log link down
void sd_log_link_down(void);

// Log all discovered network data (LLDP/CDP, DHCP, EAPOL, connectivity, cable)
void sd_log_network_data(const lldp_info_t *lldp,
                         const cdp_info_t *cdp,
                         const dhcp_info_t *dhcp,
                         const char *eapol_status,
                         const connectivity_results_t *conn,
                         const cable_test_result_t *cable);
