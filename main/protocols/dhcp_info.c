#include "dhcp_info.h"
#include "esp_netif.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "dhcp_info";

static dhcp_info_t s_dhcp_info;

void dhcp_info_save(const esp_netif_ip_info_t *ip_info, esp_netif_t *netif)
{
    memset(&s_dhcp_info, 0, sizeof(s_dhcp_info));

    snprintf(s_dhcp_info.ip, sizeof(s_dhcp_info.ip), IPSTR, IP2STR(&ip_info->ip));
    snprintf(s_dhcp_info.netmask, sizeof(s_dhcp_info.netmask), IPSTR, IP2STR(&ip_info->netmask));
    snprintf(s_dhcp_info.gateway, sizeof(s_dhcp_info.gateway), IPSTR, IP2STR(&ip_info->gw));

    // Query primary DNS from the netif
    esp_netif_dns_info_t dns_info;
    if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns_info) == ESP_OK) {
        snprintf(s_dhcp_info.dns, sizeof(s_dhcp_info.dns), IPSTR,
                 IP2STR(&dns_info.ip.u_addr.ip4));
    } else {
        snprintf(s_dhcp_info.dns, sizeof(s_dhcp_info.dns), "--");
    }

    s_dhcp_info.valid = true;

    ESP_LOGI(TAG, "DHCP lease: IP=%s Mask=%s GW=%s DNS=%s",
             s_dhcp_info.ip, s_dhcp_info.netmask,
             s_dhcp_info.gateway, s_dhcp_info.dns);
}

const dhcp_info_t *dhcp_info_get(void)
{
    return s_dhcp_info.valid ? &s_dhcp_info : NULL;
}

void dhcp_info_reset(void)
{
    memset(&s_dhcp_info, 0, sizeof(s_dhcp_info));
    ESP_LOGI(TAG, "DHCP info reset");
}
