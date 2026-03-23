#include "storage/sd_log.h"
#include "storage/sd_card.h"
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

static const char *TAG = "sd_log";
static const char *LOG_PATH = "/sdcard/lldpkt.log";

// Shared write buffer — allocated from PSRAM to save internal RAM
#define BUF_SIZE 1024
static char *s_buf = NULL;

static const char *test_status_str(test_status_t s)
{
    switch (s) {
    case TEST_PASS: return "PASS";
    case TEST_FAIL: return "FAIL";
    case TEST_RUNNING: return "RUNNING";
    default: return "N/A";
    }
}

static int uptime_secs(void)
{
    return (int)(esp_timer_get_time() / 1000000);
}

// Write string to log file using POSIX open/write (no heap allocation)
static void log_write(const char *text)
{
    if (!sd_card_is_mounted()) return;
    int fd = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        ESP_LOGW(TAG, "Failed to open %s", LOG_PATH);
        return;
    }
    write(fd, text, strlen(text));
    close(fd);
}

void sd_log_init(void)
{
    if (!sd_card_is_mounted()) return;
    if (!s_buf) {
        s_buf = heap_caps_malloc(BUF_SIZE, MALLOC_CAP_SPIRAM);
        if (!s_buf) {
            ESP_LOGW(TAG, "Failed to allocate log buffer");
            return;
        }
    }
    snprintf(s_buf, BUF_SIZE,
             "\n========================================\n"
             "LLDPocket Session Start\n"
             "Board: %s\n"
             "========================================\n",
             BOARD_NAME);
    log_write(s_buf);
    ESP_LOGI(TAG, "Log file ready: %s", LOG_PATH);
}

void sd_log_boot(const char *mac_str)
{
    if (!s_buf) return;
    snprintf(s_buf, BUF_SIZE, "[%ds] BOOT mac=%s\n",
             uptime_secs(), mac_str ? mac_str : "unknown");
    log_write(s_buf);
}

void sd_log_link_up(bool is_100m, bool full_duplex)
{
    if (!s_buf) return;
    snprintf(s_buf, BUF_SIZE, "[%ds] LINK_UP speed=%s duplex=%s\n",
             uptime_secs(),
             is_100m ? "100M" : "10M",
             full_duplex ? "full" : "half");
    log_write(s_buf);
}

void sd_log_link_down(void)
{
    if (!s_buf) return;
    snprintf(s_buf, BUF_SIZE, "[%ds] LINK_DOWN\n", uptime_secs());
    log_write(s_buf);
}

void sd_log_network_data(const lldp_info_t *lldp,
                         const cdp_info_t *cdp,
                         const dhcp_info_t *dhcp,
                         const char *eapol_status,
                         const connectivity_results_t *conn,
                         const cable_test_result_t *cable)
{
    if (!s_buf) return;
    int off = 0;
    int rem = BUF_SIZE;

#define APPEND(...) do { \
    int n = snprintf(s_buf + off, rem, __VA_ARGS__); \
    if (n > 0 && n < rem) { off += n; rem -= n; } \
} while(0)

    APPEND("[%ds] NETWORK_DATA\n", uptime_secs());

    // LLDP
    if (lldp && lldp->received) {
        APPEND("  LLDP:\n");
        APPEND("    switch=%s\n", lldp->system_name);
        if (lldp->port_desc[0])
            APPEND("    port=%s (%s)\n", lldp->port_id, lldp->port_desc);
        else
            APPEND("    port=%s\n", lldp->port_id);
        if (lldp->has_mgmt_addr && lldp->mgmt_addr_subtype == 1) {
            APPEND("    mgmt=%d.%d.%d.%d\n",
                   lldp->mgmt_addr[0], lldp->mgmt_addr[1],
                   lldp->mgmt_addr[2], lldp->mgmt_addr[3]);
        }
        if (lldp->has_vlan) APPEND("    vlan=%d\n", lldp->port_vlan_id);
        if (lldp->has_poe) APPEND("    poe=0x%02X\n", lldp->poe_mdi_power_support);
        if (lldp->system_desc[0]) APPEND("    platform=%.80s\n", lldp->system_desc);
    }

    // CDP
    if (cdp && cdp->received) {
        APPEND("  CDP:\n");
        APPEND("    switch=%s\n", cdp->device_id);
        APPEND("    port=%s\n", cdp->port_id);
        if (cdp->platform[0]) APPEND("    platform=%s\n", cdp->platform);
        if (cdp->has_native_vlan) APPEND("    vlan=%d\n", cdp->native_vlan);
        if (cdp->has_mgmt_addr && cdp->mgmt_addr_len == 4) {
            APPEND("    mgmt=%d.%d.%d.%d\n",
                   cdp->mgmt_addr[0], cdp->mgmt_addr[1],
                   cdp->mgmt_addr[2], cdp->mgmt_addr[3]);
        }
        if (cdp->has_power) APPEND("    power=%lumW\n", (unsigned long)cdp->power_available_mw);
    }

    // DHCP
    if (dhcp && dhcp->valid) {
        APPEND("  DHCP:\n");
        APPEND("    ip=%s mask=%s gw=%s dns=%s\n",
               dhcp->ip, dhcp->netmask, dhcp->gateway, dhcp->dns);
    }

    // EAPOL
    if (eapol_status) {
        APPEND("  EAPOL: %s\n", eapol_status);
    }

    // Connectivity
    if (conn) {
        APPEND("  Connectivity: gw=%s dns=%s http=%s\n",
               test_status_str(conn->ping_gw),
               test_status_str(conn->dns),
               test_status_str(conn->http));
    }

    // Cable
    if (cable && cable->valid) {
        APPEND("  Cable: %s (autoneg=%s)\n",
               cable->quality_str ? cable->quality_str : "unknown",
               cable->autoneg_complete ? "yes" : "no");
    }

#undef APPEND

    log_write(s_buf);
    ESP_LOGI(TAG, "Network data logged to SD");
}
