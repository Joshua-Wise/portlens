#include "connectivity.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "ping/ping_sock.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"

#include "esp_heap_caps.h"
#include <string.h>

static const char *TAG = "connectivity";

#define CONN_TASK_STACK  4096
#define CONN_TASK_PRIO   5

static connectivity_results_t s_results;
static TaskHandle_t s_task_handle = NULL;

// Ping synchronization
static SemaphoreHandle_t s_ping_done;
static bool s_ping_success;

// ---- Ping callbacks --------------------------------------------------------

static void ping_on_success(esp_ping_handle_t hdl, void *args)
{
    s_ping_success = true;
}

static void ping_on_timeout(esp_ping_handle_t hdl, void *args)
{
    // individual ping timeout — don't mark success
}

static void ping_on_end(esp_ping_handle_t hdl, void *args)
{
    xSemaphoreGive(s_ping_done);
}

// ---- Test: Ping Gateway ----------------------------------------------------

static test_status_t test_ping_gw(const esp_ip4_addr_t *gateway, esp_netif_t *netif)
{
    ESP_LOGI(TAG, "Ping gateway: " IPSTR, IP2STR(gateway));

    if (!s_ping_done) {
        s_ping_done = xSemaphoreCreateBinary();
    }
    s_ping_success = false;

    esp_ping_config_t ping_config = ESP_PING_DEFAULT_CONFIG();
    ip_addr_t target_addr;
    memset(&target_addr, 0, sizeof(target_addr));
#if LWIP_IPV6
    target_addr.type = IPADDR_TYPE_V4;
    target_addr.u_addr.ip4.addr = gateway->addr;
#else
    target_addr.addr = gateway->addr;
#endif
    ping_config.target_addr = target_addr;
    ping_config.count = 3;
    ping_config.timeout_ms = 2000;
    ping_config.interval_ms = 1000;
    ping_config.interface = esp_netif_get_netif_impl_index(netif);

    esp_ping_callbacks_t cbs = {
        .on_ping_success = ping_on_success,
        .on_ping_timeout = ping_on_timeout,
        .on_ping_end = ping_on_end,
        .cb_args = NULL,
    };

    esp_ping_handle_t ping_handle = NULL;
    esp_err_t ret = esp_ping_new_session(&ping_config, &cbs, &ping_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Ping session create failed: %s", esp_err_to_name(ret));
        return TEST_FAIL;
    }

    esp_ping_start(ping_handle);

    // Wait for ping session to complete (3 pings × 2s timeout + margin)
    if (xSemaphoreTake(s_ping_done, pdMS_TO_TICKS(10000)) != pdTRUE) {
        ESP_LOGW(TAG, "Ping timed out waiting for completion");
        esp_ping_stop(ping_handle);
    }

    esp_ping_delete_session(ping_handle);

    if (s_ping_success) {
        ESP_LOGI(TAG, "Ping gateway: PASS");
        return TEST_PASS;
    }
    ESP_LOGW(TAG, "Ping gateway: FAIL (no replies)");
    return TEST_FAIL;
}

// ---- Test: DNS Resolution --------------------------------------------------

static test_status_t test_dns(void)
{
    ESP_LOGI(TAG, "DNS resolve: dns.google");

    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *result = NULL;

    int err = getaddrinfo("dns.google", "443", &hints, &result);
    if (err != 0 || result == NULL) {
        ESP_LOGW(TAG, "DNS resolve: FAIL (err=%d)", err);
        if (result) freeaddrinfo(result);
        return TEST_FAIL;
    }

    struct sockaddr_in *addr = (struct sockaddr_in *)result->ai_addr;
    ESP_LOGI(TAG, "DNS resolve: PASS -> %s", inet_ntoa(addr->sin_addr));
    freeaddrinfo(result);
    return TEST_PASS;
}

// ---- Test: HTTP connectivity -----------------------------------------------

static test_status_t test_http(void)
{
    ESP_LOGI(TAG, "HTTP GET: clients3.google.com/generate_204");

    esp_http_client_config_t config = {
        .url = "http://clients3.google.com/generate_204",
        .timeout_ms = 5000,
        .disable_auto_redirect = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "HTTP client init failed");
        return TEST_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    test_status_t status = TEST_FAIL;

    if (err == ESP_OK) {
        int code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "HTTP response: %d", code);
        // Accept 204 (expected) or 200 (some captive portals)
        if (code == 204 || code == 200) {
            status = TEST_PASS;
        }
    } else {
        ESP_LOGW(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return status;
}

// ---- Background task -------------------------------------------------------

typedef struct {
    esp_ip4_addr_t gateway;
    esp_netif_t   *netif;
} conn_task_params_t;

static conn_task_params_t s_task_params;
static bool s_task_uses_caps = false;

static void connectivity_task(void *arg)
{
    conn_task_params_t *params = (conn_task_params_t *)arg;

    // Brief settling delay: ARP for the gateway hasn't been resolved yet when
    // IP_EVENT_ETH_GOT_IP fires, and a 2s ping timeout can lose the race.
    vTaskDelay(pdMS_TO_TICKS(1500));

    // Test 1: Ping gateway
    s_results.ping_gw = TEST_RUNNING;
    s_results.ping_gw = test_ping_gw(&params->gateway, params->netif);

    // Test 2: DNS resolution
    s_results.dns = TEST_RUNNING;
    s_results.dns = test_dns();

    // Test 3: HTTP connectivity
    s_results.http = TEST_RUNNING;
    s_results.http = test_http();

    ESP_LOGI(TAG, "All connectivity tests complete: GW=%s DNS=%s HTTP=%s",
             s_results.ping_gw == TEST_PASS ? "PASS" : "FAIL",
             s_results.dns == TEST_PASS ? "PASS" : "FAIL",
             s_results.http == TEST_PASS ? "PASS" : "FAIL");

    TaskHandle_t self = s_task_handle;
    bool uses_caps = s_task_uses_caps;
    s_task_handle = NULL;
    if (uses_caps) {
        vTaskDeleteWithCaps(self);
    } else {
        vTaskDelete(NULL);
    }
}

// ---- Public API ------------------------------------------------------------

void connectivity_start_tests(const esp_ip4_addr_t *gateway, esp_netif_t *netif)
{
    // Don't start if already running
    if (s_task_handle != NULL) {
        ESP_LOGW(TAG, "Tests already running, skipping");
        return;
    }

    // Reset results
    memset(&s_results, 0, sizeof(s_results));

    // Copy params for the task
    s_task_params.gateway = *gateway;
    s_task_params.netif = netif;

    s_task_uses_caps = false;
    BaseType_t ret = xTaskCreateWithCaps(connectivity_task, "conn_test", CONN_TASK_STACK,
                                         &s_task_params, CONN_TASK_PRIO,
                                         &s_task_handle, MALLOC_CAP_SPIRAM);
    if (ret == pdPASS) {
        s_task_uses_caps = true;
    } else {
        // Fallback to internal RAM with smaller stack
        ret = xTaskCreate(connectivity_task, "conn_test", 3072,
                          &s_task_params, CONN_TASK_PRIO, &s_task_handle);
    }
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create connectivity task (internal=%lu)",
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        s_task_handle = NULL;
    }
}

const connectivity_results_t *connectivity_get_results(void)
{
    return &s_results;
}

void connectivity_reset(void)
{
    memset(&s_results, 0, sizeof(s_results));
    ESP_LOGI(TAG, "Connectivity results reset");
}
