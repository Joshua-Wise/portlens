#include "web/web_server.h"

#if FEATURE_WIFI_AP

#include "config.h"
#include "proto_lock.h"
#include "protocols/lldp_parser.h"
#include "protocols/cdp_parser.h"
#include "protocols/dhcp_info.h"
#include "protocols/eapol_detect.h"
#include "tests/connectivity.h"
#include "eth/eth_init.h"

#include "esp_wifi.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "ping/ping_sock.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static const char *TAG = "web_server";

static httpd_handle_t s_httpd = NULL;
static esp_netif_t   *s_ap_netif = NULL;
static TaskHandle_t   s_dns_task = NULL;

// ---- DNS captive portal task ------------------------------------------------
// Responds to ALL DNS queries with the AP's own IP (192.168.4.1)
// This makes phones/laptops detect it as a captive portal.

#define DNS_PORT     53
#define DNS_BUF_SIZE 512
// Bytes appended after the echoed query: name ptr(2) + type(2) + class(2)
// + TTL(4) + RDLENGTH(2) + RDATA(4) = 16
#define DNS_ANSWER_LEN 16

static void dns_server_task(void *pvParam)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket failed");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS captive portal running on port 53");

    // AP IP: 192.168.4.1
    uint8_t ap_ip[4] = {192, 168, 4, 1};

    uint8_t buf[DNS_BUF_SIZE];
    struct sockaddr_in client_addr;
    socklen_t client_len;

    while (1) {
        client_len = sizeof(client_addr);
        int len = recvfrom(sock, buf, DNS_BUF_SIZE, 0,
                           (struct sockaddr *)&client_addr, &client_len);
        if (len < 12) continue;  // Too short for DNS header

        // We append a fixed 16-byte answer record after the echoed query, so
        // the query must leave room for it. Drop anything that would overflow
        // `resp` (a malformed/oversized query could otherwise smash the stack).
        if (len > DNS_BUF_SIZE - DNS_ANSWER_LEN) continue;

        // Build minimal DNS response:
        // Copy the query, set QR=1 (response), ANCOUNT=1, add an answer
        uint8_t resp[DNS_BUF_SIZE];
        memcpy(resp, buf, len);

        // Set QR bit (bit 15 of flags), keep RD, set RA
        resp[2] = 0x81;  // QR=1, Opcode=0, AA=0, TC=0, RD=1
        resp[3] = 0x80;  // RA=1, Z=0, RCODE=0 (no error)

        // Set ANCOUNT = 1
        resp[6] = 0x00;
        resp[7] = 0x01;

        // Append answer: NAME (pointer to query), TYPE A, CLASS IN, TTL=60, RDLENGTH=4, RDATA=IP
        int pos = len;
        resp[pos++] = 0xC0; resp[pos++] = 0x0C;  // Name pointer to offset 12
        resp[pos++] = 0x00; resp[pos++] = 0x01;   // Type A
        resp[pos++] = 0x00; resp[pos++] = 0x01;   // Class IN
        resp[pos++] = 0x00; resp[pos++] = 0x00;   // TTL (high)
        resp[pos++] = 0x00; resp[pos++] = 0x3C;   // TTL = 60
        resp[pos++] = 0x00; resp[pos++] = 0x04;   // RDLENGTH = 4
        memcpy(&resp[pos], ap_ip, 4);
        pos += 4;

        sendto(sock, resp, pos, 0,
               (struct sockaddr *)&client_addr, client_len);
    }

    close(sock);
    vTaskDelete(NULL);
}

// ---- Results snapshot -------------------------------------------------------
// The page is served as a static shell; all live data is delivered as JSON via
// /api/results and rendered client-side. collect_display() takes a consistent
// snapshot of the shared protocol state under proto_lock.

static const char *status_str(test_status_t s)
{
    switch (s) {
    case TEST_PASS:    return "PASS";
    case TEST_FAIL:    return "FAIL";
    case TEST_RUNNING: return "...";
    default:           return "--";
    }
}

// Display-ready snapshot. All strings are pre-formatted for direct rendering.
typedef struct {
    char        sw[80];
    char        port[80];
    char        vlan[80];
    char        mgmt[48];
    char        proto[8];
    int         ttl;
    char        port_desc[128];
    char        platform[96];
    char        ip[16];
    char        netmask[16];
    char        gateway[16];
    char        dns[16];
    char        link[32];
    const char *ping_gw;   // points at a status_str() literal
    const char *dns_test;
    const char *http;
    char        dot1x[64];
    char        poe[32];
} display_data_t;

static void collect_display(display_data_t *d)
{
    memset(d, 0, sizeof(*d));
    strcpy(d->proto, "--");
    strcpy(d->vlan, "--");
    strcpy(d->mgmt, "--");
    strcpy(d->poe, "No");
    d->ping_gw = d->dns_test = d->http = "--";

    proto_lock();

    const lldp_info_t *lldp = lldp_get_info();
    const cdp_info_t  *cdp  = cdp_get_info();
    const dhcp_info_t *dhcp = dhcp_info_get();
    const connectivity_results_t *conn = connectivity_get_results();

    const char *sw = "--", *port = "--";

    if (lldp) {
        if (lldp->system_name[0]) sw = lldp->system_name;
        if (lldp->port_id[0]) port = lldp->port_id;
        if (lldp->system_desc[0]) strlcpy(d->platform, lldp->system_desc, sizeof(d->platform));
        if (lldp->port_desc[0]) strlcpy(d->port_desc, lldp->port_desc, sizeof(d->port_desc));
        strcpy(d->proto, "LLDP");
        d->ttl = lldp->ttl;
        if (lldp->has_vlan) {
            if (lldp->has_vlan_name && lldp->vlan_name[0])
                snprintf(d->vlan, sizeof(d->vlan), "%u (%s)", lldp->port_vlan_id, lldp->vlan_name);
            else
                snprintf(d->vlan, sizeof(d->vlan), "%u", lldp->port_vlan_id);
        }
        if (lldp->has_mgmt_addr && lldp->mgmt_addr_subtype == 1 && lldp->mgmt_addr_len >= 4) {
            snprintf(d->mgmt, sizeof(d->mgmt), "%u.%u.%u.%u",
                     lldp->mgmt_addr[0], lldp->mgmt_addr[1],
                     lldp->mgmt_addr[2], lldp->mgmt_addr[3]);
        }
        if (lldp->has_poe) {
            uint8_t ps = lldp->poe_mdi_power_support;
            if ((ps & 0x02) && (ps & 0x04)) strcpy(d->poe, "Enabled");
            else if (ps & 0x02) strcpy(d->poe, "Supported");
            else strcpy(d->poe, "Yes");
        }
    } else if (cdp) {
        if (cdp->device_id[0]) sw = cdp->device_id;
        if (cdp->port_id[0]) port = cdp->port_id;
        if (cdp->platform[0]) strlcpy(d->platform, cdp->platform, sizeof(d->platform));
        strcpy(d->proto, "CDP");
        d->ttl = cdp->ttl;
        if (cdp->has_native_vlan) snprintf(d->vlan, sizeof(d->vlan), "%u", cdp->native_vlan);
        if (cdp->has_mgmt_addr && cdp->mgmt_addr_len >= 4) {
            snprintf(d->mgmt, sizeof(d->mgmt), "%u.%u.%u.%u",
                     cdp->mgmt_addr[0], cdp->mgmt_addr[1],
                     cdp->mgmt_addr[2], cdp->mgmt_addr[3]);
        }
        if (cdp->has_power && cdp->power_available_mw > 0) {
            snprintf(d->poe, sizeof(d->poe), "%luW", (unsigned long)(cdp->power_available_mw / 1000));
        }
    }
    strlcpy(d->sw, sw, sizeof(d->sw));
    strlcpy(d->port, port, sizeof(d->port));

    if (dhcp) {
        strlcpy(d->ip, dhcp->ip, sizeof(d->ip));
        strlcpy(d->netmask, dhcp->netmask, sizeof(d->netmask));
        strlcpy(d->gateway, dhcp->gateway, sizeof(d->gateway));
        strlcpy(d->dns, dhcp->dns, sizeof(d->dns));
    } else {
        strcpy(d->ip, "DHCP...");
        strcpy(d->netmask, "--");
        strcpy(d->gateway, "--");
        strcpy(d->dns, "--");
    }

    d->ping_gw  = status_str(conn->ping_gw);
    d->dns_test = status_str(conn->dns);
    d->http     = status_str(conn->http);

    if (eapol_detected()) {
        const char *e = eapol_get_status();
        strlcpy(d->dot1x, e ? e : "None", sizeof(d->dot1x));
    } else {
        strcpy(d->dot1x, "None");
    }

    proto_unlock();

    // Link info comes from the PHY (not the protocol structs) — no lock needed.
    esp_eth_handle_t eth = eth_get_handle();
    strcpy(d->link, "Down");
    if (eth) {
        eth_speed_t speed;
        eth_duplex_t duplex;
        esp_eth_ioctl(eth, ETH_CMD_G_SPEED, &speed);
        esp_eth_ioctl(eth, ETH_CMD_G_DUPLEX_MODE, &duplex);
        snprintf(d->link, sizeof(d->link), "%s %s",
                 (speed == ETH_SPEED_100M) ? "100M" : "10M",
                 (duplex == ETH_DUPLEX_FULL) ? "Full-Duplex" : "Half-Duplex");
    }
}

// Copy `src` into `dst` as a JSON string body, escaping the characters that
// would otherwise break the JSON (or inject into it — switch-supplied names
// are untrusted). Always NUL-terminates.
static void json_escape(char *dst, size_t dstsize, const char *src)
{
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 1 < dstsize; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') {
            if (j + 2 >= dstsize) break;
            dst[j++] = '\\';
            dst[j++] = c;
        } else if (c < 0x20) {
            if (j + 6 >= dstsize) break;
            j += snprintf(dst + j, dstsize - j, "\\u%04x", c);
        } else {
            dst[j++] = c;
        }
    }
    dst[j] = '\0';
}

// ---- HTML page (static shell) ----------------------------------------------
// All values are filled in by the client from /api/results, so this is a
// constant served straight from flash — no per-request rendering or buffer.
// Rendering values via textContent (not innerHTML) also means switch-supplied
// strings cannot inject markup into the operator's browser.

static const char PAGE_HTML[] =
    "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>PortLens</title>"
    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{font-family:-apple-system,system-ui,sans-serif;background:#0d1117;color:#e6edf3;padding:16px;max-width:480px;margin:0 auto}"
    "h1{font-size:20px;margin-bottom:4px;color:#58a6ff}"
    ".sub{color:#8b949e;font-size:13px;margin-bottom:16px}"
    ".section{margin-bottom:16px}"
    ".section h2{font-size:14px;color:#8b949e;text-transform:uppercase;letter-spacing:1px;margin-bottom:8px;border-bottom:1px solid #30363d;padding-bottom:4px}"
    ".row{display:flex;justify-content:space-between;padding:6px 0;border-bottom:1px solid #21262d}"
    ".row .label{color:#8b949e;font-size:14px}"
    ".row .value{font-size:14px;font-weight:500;text-align:right;max-width:60%}"
    ".pass{color:#3fb950}"
    ".fail{color:#f85149}"
    ".warn{color:#d29922}"
    ".badge{display:inline-block;padding:2px 8px;border-radius:12px;font-size:12px;font-weight:600}"
    ".badge.pass{background:#0f2d1a}"
    ".badge.fail{background:#3d1014}"
    ".badge.warn{background:#3d2c06}"
    "button{background:#21262d;color:#e6edf3;border:1px solid #30363d;padding:10px 20px;border-radius:6px;font-size:14px;width:100%;margin-top:8px;cursor:pointer}"
    "button:active{background:#30363d}"
    "#ping-modal{position:fixed;inset:0;display:none;align-items:center;justify-content:center;z-index:10}"
    "#ping-modal.open{display:flex}"
    "#ping-modal .backdrop{position:absolute;inset:0;background:rgba(0,0,0,0.7)}"
    "#ping-modal .card{position:relative;background:#161b22;border:1px solid #30363d;border-radius:8px;padding:20px;width:90%;max-width:360px}"
    "#ping-modal h3{margin-bottom:12px;color:#58a6ff;font-size:16px}"
    "#ping-modal input{width:100%;padding:8px;background:#0d1117;border:1px solid #30363d;color:#e6edf3;border-radius:4px;margin-bottom:8px;font-size:14px}"
    "#ping-result{margin-top:8px;padding:8px;font-size:13px;min-height:18px}"
    "</style></head><body>"
    "<h1>PortLens</h1>"

    "<div class='section'><h2>Switch</h2>"
    "<div class='row'><span class='label'>Name</span><span class='value' id='sw'>--</span></div>"
    "<div class='row'><span class='label'>Port</span><span class='value' id='port'>--</span></div>"
    "<div class='row'><span class='label'>VLAN</span><span class='value' id='vlan'>--</span></div>"
    "<div class='row'><span class='label'>Mgmt IP</span><span class='value' id='mgmt'>--</span></div>"
    "<div class='row'><span class='label'>Protocol</span><span class='value'><span id='proto'>--</span> (TTL <span id='ttl'>0</span>)</span></div>"
    "<div class='row' id='pdrow' style='display:none'><span class='label'>Port Desc</span><span class='value' id='pdesc'></span></div>"
    "<div class='row' id='pfrow' style='display:none'><span class='label'>Platform</span><span class='value' id='plat' style='font-size:12px'></span></div>"
    "</div>"

    "<div class='section'><h2>Network</h2>"
    "<div class='row'><span class='label'>IP Address</span><span class='value' id='ip'>--</span></div>"
    "<div class='row'><span class='label'>Netmask</span><span class='value' id='netmask'>--</span></div>"
    "<div class='row'><span class='label'>Gateway</span><span class='value' id='gateway'>--</span></div>"
    "<div class='row'><span class='label'>DNS</span><span class='value' id='dns'>--</span></div>"
    "</div>"

    "<div class='section'><h2>Connectivity</h2>"
    "<div class='row'><span class='label'>Link</span><span class='value' id='link'>--</span></div>"
    "<div class='row'><span class='label'>Gateway Ping</span><span class='value'><span class='badge' id='ping_gw'>--</span></span></div>"
    "<div class='row'><span class='label'>DNS Test</span><span class='value'><span class='badge' id='dns_test'>--</span></span></div>"
    "<div class='row'><span class='label'>HTTP / WAN</span><span class='value'><span class='badge' id='http'>--</span></span></div>"
    "<div class='row'><span class='label'>802.1X</span><span class='value' id='dot1x'>--</span></div>"
    "<div class='row'><span class='label'>PoE</span><span class='value' id='poe'>--</span></div>"
    "</div>"

    "<button onclick='openPing()'>Ping</button>"
    "<button onclick='update()'>Refresh</button>"

    "<div id='ping-modal'>"
    "<div class='backdrop' onclick='closePing()'></div>"
    "<div class='card'>"
    "<h3>Ping</h3>"
    "<input id='ping-ip' type='text' placeholder='IP address' autocomplete='off'>"
    "<button onclick='doPing()'>Ping</button>"
    "<div id='ping-result'></div>"
    "<button onclick='closePing()'>Close</button>"
    "</div></div>"

    "<script>"
    "function cls(s){return s==='PASS'?'pass':s==='FAIL'?'fail':s==='...'?'warn':'';}"
    "function set(id,v){var e=document.getElementById(id);if(e)e.textContent=(v==null?'':v);}"
    "function badge(id,v){var e=document.getElementById(id);if(!e)return;e.textContent=v;e.className='badge '+cls(v);}"
    "function row(rid,vid,v){var r=document.getElementById(rid);set(vid,v);if(r)r.style.display=v?'':'none';}"
    "function update(){"
    "fetch('/api/results').then(function(r){return r.json();}).then(function(d){"
    "set('sw',d.switch);set('port',d.port);set('vlan',d.vlan);set('mgmt',d.mgmt);"
    "set('proto',d.protocol);set('ttl',d.ttl);"
    "row('pdrow','pdesc',d.port_desc);row('pfrow','plat',d.platform);"
    "set('ip',d.ip);set('netmask',d.netmask);set('gateway',d.gateway);set('dns',d.dns);"
    "set('link',d.link);"
    "badge('ping_gw',d.ping_gw);badge('dns_test',d.dns_test);badge('http',d.http);"
    "set('dot1x',d.dot1x);set('poe',d.poe);"
    "}).catch(function(){});"
    "}"
    "function openPing(){"
    "document.getElementById('ping-modal').classList.add('open');"
    "var el=document.getElementById('ping-result');el.textContent='';el.className='';"
    "document.getElementById('ping-ip').focus();"
    "}"
    "function closePing(){"
    "document.getElementById('ping-modal').classList.remove('open');"
    "}"
    "function doPing(){"
    "var ip=document.getElementById('ping-ip').value.trim();"
    "if(!ip)return;"
    "var el=document.getElementById('ping-result');"
    "el.className='warn';"
    "el.textContent='Pinging '+ip+'...';"
    "fetch('/api/ping?ip='+encodeURIComponent(ip))"
    ".then(function(r){return r.json();})"
    ".then(function(d){"
    "if(d.error){el.className='fail';el.textContent='Error: '+d.error;return;}"
    "el.className=d.received>0?'pass':'fail';"
    "var loss=d.sent>0?Math.round((d.sent-d.received)*100/d.sent):100;"
    "el.textContent=d.ip+': '+d.received+'/'+d.sent+' replies, '+loss+'% loss, avg '+d.avg_ms+' ms';"
    "})"
    ".catch(function(){el.className='fail';el.textContent='Request failed';});"
    "}"
    "document.getElementById('ping-ip').addEventListener('keydown',function(e){if(e.key==='Enter')doPing();});"
    "update();setInterval(update,5000);"
    "</script>"
    "<div style='text-align:center;margin-top:12px;color:#8b949e;font-size:11px'>PortLens v1.0</div>"
    "</body></html>";

// Generate JSON results from a consistent snapshot
static int generate_json(char *buf, size_t buflen)
{
    display_data_t d;
    collect_display(&d);

    // Escape the switch-supplied (untrusted) strings.
    char sw[160], port[160], vlan[160], pdesc[256], plat[192], dot1x[128];
    json_escape(sw, sizeof(sw), d.sw);
    json_escape(port, sizeof(port), d.port);
    json_escape(vlan, sizeof(vlan), d.vlan);
    json_escape(pdesc, sizeof(pdesc), d.port_desc);
    json_escape(plat, sizeof(plat), d.platform);
    json_escape(dot1x, sizeof(dot1x), d.dot1x);

    return snprintf(buf, buflen,
        "{"
        "\"switch\":\"%s\",\"port\":\"%s\",\"vlan\":\"%s\","
        "\"mgmt\":\"%s\",\"protocol\":\"%s\",\"ttl\":%d,"
        "\"port_desc\":\"%s\",\"platform\":\"%s\","
        "\"ip\":\"%s\",\"netmask\":\"%s\",\"gateway\":\"%s\",\"dns\":\"%s\","
        "\"link\":\"%s\",\"ping_gw\":\"%s\",\"dns_test\":\"%s\","
        "\"http\":\"%s\",\"dot1x\":\"%s\",\"poe\":\"%s\""
        "}",
        sw, port, vlan,
        d.mgmt, d.proto, d.ttl,
        pdesc, plat,
        d.ip, d.netmask, d.gateway, d.dns,
        d.link, d.ping_gw, d.dns_test,
        d.http, dot1x, d.poe);
}

// ---- HTTP handlers ----------------------------------------------------------

static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, PAGE_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t json_handler(httpd_req_t *req)
{
    char buf[2048];
    generate_json(buf, sizeof(buf));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ---- Ping API ---------------------------------------------------------------
// A user-requested ping takes several seconds. Running it inline would block
// the (single) HTTP worker, freezing the rest of the UI. Instead the handler
// hands the request off to a dedicated task via the async handler API and
// returns immediately, keeping the server responsive. Only one ping runs at a
// time.

typedef struct {
    SemaphoreHandle_t done;
    uint32_t rtt_total_ms;
} ping_ctx_t;

typedef struct {
    httpd_req_t *req;   // async-captured request
    char ip[48];
} ping_job_t;

static volatile bool s_ping_busy = false;

static void ping_api_on_success(esp_ping_handle_t hdl, void *args)
{
    ping_ctx_t *ctx = (ping_ctx_t *)args;
    uint32_t gap = 0;
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &gap, sizeof(gap));
    ctx->rtt_total_ms += gap;
}

static void ping_api_on_timeout(esp_ping_handle_t hdl, void *args) { }

static void ping_api_on_end(esp_ping_handle_t hdl, void *args)
{
    ping_ctx_t *ctx = (ping_ctx_t *)args;
    xSemaphoreGive(ctx->done);
}

static void send_ping_error(httpd_req_t *req, const char *status, const char *msg)
{
    char buf[96];
    int n = snprintf(buf, sizeof(buf), "{\"error\":\"%s\"}", msg);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, buf, n);
}

static void ping_async_task(void *arg)
{
    ping_job_t *job = (ping_job_t *)arg;
    httpd_req_t *req = job->req;

    struct in_addr in;
    inet_aton(job->ip, &in);   // already validated in the handler

    esp_netif_t *eth_netif = eth_get_netif();
    ping_ctx_t ctx = { .done = xSemaphoreCreateBinary(), .rtt_total_ms = 0 };

    if (!ctx.done || !eth_netif) {
        send_ping_error(req, "500 Internal Server Error", "out of memory");
        goto cleanup;
    }

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    ip_addr_t target = {0};
#if LWIP_IPV6
    target.type = IPADDR_TYPE_V4;
    target.u_addr.ip4.addr = in.s_addr;
#else
    target.addr = in.s_addr;
#endif
    cfg.target_addr = target;
    cfg.count = 4;
    cfg.timeout_ms = 1500;
    cfg.interval_ms = 500;
    cfg.interface = esp_netif_get_netif_impl_index(eth_netif);

    esp_ping_callbacks_t cbs = {
        .on_ping_success = ping_api_on_success,
        .on_ping_timeout = ping_api_on_timeout,
        .on_ping_end = ping_api_on_end,
        .cb_args = &ctx,
    };

    esp_ping_handle_t hdl = NULL;
    if (esp_ping_new_session(&cfg, &cbs, &hdl) != ESP_OK) {
        send_ping_error(req, "500 Internal Server Error", "ping session failed");
        goto cleanup;
    }

    esp_ping_start(hdl);
    // 4 pings × (1500 ms timeout + 500 ms interval) = 8 s; add margin
    xSemaphoreTake(ctx.done, pdMS_TO_TICKS(12000));

    uint32_t sent = 0, received = 0;
    esp_ping_get_profile(hdl, ESP_PING_PROF_REQUEST, &sent, sizeof(sent));
    esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY, &received, sizeof(received));
    esp_ping_delete_session(hdl);

    uint32_t avg_ms = received > 0 ? ctx.rtt_total_ms / received : 0;

    char resp[160];
    int n = snprintf(resp, sizeof(resp),
        "{\"ip\":\"%s\",\"sent\":%lu,\"received\":%lu,\"avg_ms\":%lu}",
        job->ip, (unsigned long)sent, (unsigned long)received, (unsigned long)avg_ms);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, n);

cleanup:
    if (ctx.done) vSemaphoreDelete(ctx.done);
    httpd_req_async_handler_complete(req);
    free(job);
    s_ping_busy = false;
    vTaskDelete(NULL);
}

static esp_err_t ping_handler(httpd_req_t *req)
{
    char query[64];
    char ip[48];

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "ip", ip, sizeof(ip)) != ESP_OK) {
        send_ping_error(req, "400 Bad Request", "missing ip param");
        return ESP_OK;
    }

    struct in_addr in;
    if (inet_aton(ip, &in) == 0) {
        send_ping_error(req, "400 Bad Request", "invalid ip");
        return ESP_OK;
    }

    if (!eth_get_netif()) {
        send_ping_error(req, "503 Service Unavailable", "no ethernet");
        return ESP_OK;
    }

    if (s_ping_busy) {
        send_ping_error(req, "429 Too Many Requests", "ping in progress");
        return ESP_OK;
    }

    ping_job_t *job = malloc(sizeof(*job));
    if (!job) {
        send_ping_error(req, "500 Internal Server Error", "out of memory");
        return ESP_OK;
    }
    strlcpy(job->ip, ip, sizeof(job->ip));

    // Detach the request so this worker is freed while the ping runs.
    if (httpd_req_async_handler_begin(req, &job->req) != ESP_OK) {
        free(job);
        send_ping_error(req, "500 Internal Server Error", "async begin failed");
        return ESP_OK;
    }

    s_ping_busy = true;
    if (xTaskCreate(ping_async_task, "ping_job", 4096, job, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create ping task");
        httpd_req_async_handler_complete(job->req);
        free(job);
        s_ping_busy = false;
    }
    return ESP_OK;
}

// Captive portal detection endpoints — redirect to root
static esp_err_t captive_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// ---- Public API -------------------------------------------------------------

esp_err_t web_server_start(void)
{
    ESP_LOGI(TAG, "Starting web server + WiFi AP");

    // Start HTTP server FIRST — serves on all interfaces (Ethernet + WiFi AP)
    httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();
    http_config.max_uri_handlers = 10;
    http_config.lru_purge_enable = true;
    http_config.stack_size = 8192;

    esp_err_t ret = httpd_start(&s_httpd, &http_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Register handlers
    httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_handler };
    httpd_uri_t api  = { .uri = "/api/results", .method = HTTP_GET, .handler = json_handler };
    httpd_uri_t ping = { .uri = "/api/ping", .method = HTTP_GET, .handler = ping_handler };

    // Captive portal detection URIs used by Android/iOS/Windows
    httpd_uri_t cp1 = { .uri = "/generate_204", .method = HTTP_GET, .handler = captive_redirect };
    httpd_uri_t cp2 = { .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = captive_redirect };
    httpd_uri_t cp3 = { .uri = "/connecttest.txt", .method = HTTP_GET, .handler = captive_redirect };
    httpd_uri_t cp4 = { .uri = "/redirect", .method = HTTP_GET, .handler = captive_redirect };
    httpd_uri_t cp5 = { .uri = "/ncsi.txt", .method = HTTP_GET, .handler = captive_redirect };

    httpd_register_uri_handler(s_httpd, &root);
    httpd_register_uri_handler(s_httpd, &api);
    httpd_register_uri_handler(s_httpd, &ping);
    httpd_register_uri_handler(s_httpd, &cp1);
    httpd_register_uri_handler(s_httpd, &cp2);
    httpd_register_uri_handler(s_httpd, &cp3);
    httpd_register_uri_handler(s_httpd, &cp4);
    httpd_register_uri_handler(s_httpd, &cp5);

    ESP_LOGI(TAG, "HTTP server started (listening on all interfaces)");

    // Now attempt WiFi AP — failure here won't affect HTTP on Ethernet
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (!s_ap_netif) {
        ESP_LOGW(TAG, "Failed to create WiFi AP netif — HTTP still available on Ethernet");
        return ESP_OK;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "WiFi init failed: %s — HTTP still available on Ethernet", esp_err_to_name(ret));
        return ESP_OK;
    }

    // Generate SSID with last 4 hex of MAC
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ssid[32];
    snprintf(ssid, sizeof(ssid), "PortLens-%02X%02X", mac[4], mac[5]);

    wifi_config_t wifi_config = {
        .ap = {
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
            .channel = 1,
        },
    };
    strncpy((char *)wifi_config.ap.ssid, ssid, sizeof(wifi_config.ap.ssid));
    wifi_config.ap.ssid_len = strlen(ssid);

    ret = esp_wifi_set_mode(WIFI_MODE_AP);
    if (ret != ESP_OK) { ESP_LOGW(TAG, "WiFi set mode failed: %s", esp_err_to_name(ret)); return ESP_OK; }
    ret = esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    if (ret != ESP_OK) { ESP_LOGW(TAG, "WiFi set config failed: %s", esp_err_to_name(ret)); return ESP_OK; }
    ret = esp_wifi_start();
    if (ret != ESP_OK) { ESP_LOGW(TAG, "WiFi start failed: %s", esp_err_to_name(ret)); return ESP_OK; }

    ESP_LOGI(TAG, "WiFi AP started: SSID=%s (open, no password)", ssid);

    // Start DNS captive portal
    xTaskCreate(dns_server_task, "dns_server", 4096, NULL, 3, &s_dns_task);

    return ESP_OK;
}

#endif // FEATURE_WIFI_AP
