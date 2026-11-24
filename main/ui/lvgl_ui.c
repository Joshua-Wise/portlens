#include "config.h"

#if FEATURE_DISPLAY

#include "ui/lvgl_ui.h"
#include "ui/tft_init.h"
#include "proto_lock.h"
#include "protocols/lldp_parser.h"
#include "protocols/cdp_parser.h"
#include "protocols/dhcp_info.h"
#include "protocols/eapol_detect.h"
#include "tests/connectivity.h"

#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "lvgl_ui";

// ---- Palette ----------------------------------------------------------------

#define COL_BG          lv_color_hex(0x000000)
#define COL_DIM         lv_color_hex(0x555B66)
#define COL_LABEL       lv_color_hex(0xC8D1DA)
#define COL_VALUE       lv_color_hex(0xFFFFFF)
#define COL_ACCENT      lv_color_hex(0x86EFAC)  // light green — PortLens brand
#define COL_FAIL        lv_color_hex(0xFCA5A5)  // light red — Disconnected

// ---- Layout -----------------------------------------------------------------

#define HEADER_H        12
#define SIDE_PAD         4
#define OVL_LINE_H      22
#define OVL_BRAND_Y      6
#define OVL_SUB_Y      104

// Time each data page stays on screen before auto-cycling to the next
#define PAGE_CYCLE_MS   10000

// ---- Static UI handles ------------------------------------------------------

// Separate LVGL screens for each UI state.
// lv_obj_create(NULL) creates a screen with no parent — no clipping at all.
// Switch between them with lv_screen_load().
static lv_obj_t *s_scr_splash  = NULL;
static lv_obj_t *s_scr_waiting = NULL;
static lv_obj_t *s_scr_data    = NULL;
static lv_obj_t *s_scr_data2   = NULL;

// Data row value labels (children of s_scr_data)
static lv_obj_t *s_val_switch = NULL;
static lv_obj_t *s_val_port   = NULL;
static lv_obj_t *s_val_vlan   = NULL;
static lv_obj_t *s_val_link   = NULL;
static lv_obj_t *s_val_poe    = NULL;
static lv_obj_t *s_val_ip     = NULL;
static lv_obj_t *s_val_gw     = NULL;
static lv_obj_t *s_val_dns    = NULL;
static lv_obj_t *s_val_dhcp   = NULL;

// Page 2 — connectivity test results
static lv_obj_t *s_val_ping   = NULL;
static lv_obj_t *s_val_dns2   = NULL;
static lv_obj_t *s_val_wan    = NULL;
static lv_obj_t *s_val_8021x  = NULL;
static lv_obj_t *s_val_desc   = NULL;
static lv_obj_t *s_val_mac    = NULL;

// Auto-cycle timer (NULL = not running)
static lv_timer_t *s_page_timer = NULL;

// ---- Helpers ----------------------------------------------------------------

static void init_screen(lv_obj_t *scr)
{
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_set_style_border_width(scr, 0, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
}

// Add a centered label directly on a screen at absolute y coordinate.
static lv_obj_t *scr_add_label(lv_obj_t *scr, const char *text, int y,
                                const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(scr);
    lv_obj_set_pos(label, 0, y);
    lv_obj_set_size(label, TFT_H_RES, OVL_LINE_H);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_all(label, 0, 0);
    lv_obj_set_style_border_width(label, 0, 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(label, text);
    return label;
}

static lv_obj_t *add_row(lv_obj_t *parent, const char *label_text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_height(label, lv_font_montserrat_10.line_height);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(label, COL_VALUE, 0);
    lv_obj_set_style_pad_all(label, 0, 0);
    char buf[64];
    snprintf(buf, sizeof(buf), "%s: --", label_text);
    lv_label_set_text(label, buf);
    return label;
}

// Build the common header bar used by both data screens.
// page is 1-based; passing 0 omits the page indicator.
static lv_obj_t *make_data_header(lv_obj_t *scr, int page)
{
    lv_obj_t *header = lv_obj_create(scr);
    lv_obj_set_size(header, TFT_H_RES, HEADER_H);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_align(header, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *title = lv_label_create(header);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(title, COL_ACCENT, 0);
    lv_label_set_text(title, "PortLens");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

    if (page > 0) {
        lv_obj_t *pg = lv_label_create(header);
        lv_obj_set_style_text_font(pg, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(pg, COL_DIM, 0);
        char buf[4];
        snprintf(buf, sizeof(buf), "%d/2", page);
        lv_label_set_text(pg, buf);
        lv_obj_align(pg, LV_ALIGN_RIGHT_MID, -SIDE_PAD, 0);
    }

    // Separator
    lv_obj_t *sep = lv_obj_create(scr);
    lv_obj_set_size(sep, TFT_H_RES, 1);
    lv_obj_set_style_bg_color(sep, COL_DIM, 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sep, 0, 0);
    lv_obj_set_style_radius(sep, 0, 0);
    lv_obj_set_style_pad_all(sep, 0, 0);
    lv_obj_align(sep, LV_ALIGN_TOP_LEFT, 0, HEADER_H);
    lv_obj_remove_flag(sep, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    return header;
}

static lv_obj_t *make_rows_container(lv_obj_t *scr)
{
    int content_y = HEADER_H + 1;
    lv_obj_t *rows = lv_obj_create(scr);
    lv_obj_set_pos(rows, 0, content_y);
    lv_obj_set_size(rows, TFT_H_RES, TFT_V_RES - content_y);
    lv_obj_set_style_bg_opa(rows, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(rows, 0, 0);
    lv_obj_set_style_radius(rows, 0, 0);
    lv_obj_set_style_pad_hor(rows, SIDE_PAD, 0);
    lv_obj_set_style_pad_top(rows, 0, 0);
    lv_obj_set_style_pad_bottom(rows, 0, 0);
    lv_obj_set_style_pad_row(rows, 0, 0);
    lv_obj_remove_flag(rows, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(rows, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(rows, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    return rows;
}

static void page_cycle_cb(lv_timer_t *timer)
{
    static bool on_page2 = false;
    on_page2 = !on_page2;
    if (on_page2 && s_scr_data2) {
        lv_screen_load(s_scr_data2);
    } else if (s_scr_data) {
        lv_screen_load(s_scr_data);
    }
}

static void set_row(lv_obj_t *obj, const char *label, const char *value)
{
    if (!obj) return;
    char buf[80];
    snprintf(buf, sizeof(buf), "%s: %s", label, value ? value : "--");
    lv_label_set_text(obj, buf);
}

// ---- Data accessors ---------------------------------------------------------

static const char *switch_name(const lldp_info_t *lldp, const cdp_info_t *cdp)
{
    if (lldp && lldp->system_name[0]) return lldp->system_name;
    if (cdp  && cdp->device_id[0])    return cdp->device_id;
    return "--";
}

static const char *port_name(const lldp_info_t *lldp, const cdp_info_t *cdp)
{
    if (lldp && lldp_port_label(lldp)[0]) return lldp_port_label(lldp);
    if (cdp  && cdp->port_id[0])  return cdp->port_id;
    return "--";
}

// ---- UI construction --------------------------------------------------------

static void create_splash_screen(void)
{
    s_scr_splash = lv_obj_create(NULL);
    init_screen(s_scr_splash);
    scr_add_label(s_scr_splash, "PortLens",
                  (TFT_V_RES - OVL_LINE_H) / 2, &lv_font_montserrat_16, COL_ACCENT);
}

static void create_waiting_screen(void)
{
    s_scr_waiting = lv_obj_create(NULL);
    init_screen(s_scr_waiting);
    scr_add_label(s_scr_waiting, "PortLens",
                  OVL_BRAND_Y, &lv_font_montserrat_16, COL_ACCENT);
    lv_obj_t *wl;
    wl = scr_add_label(s_scr_waiting, "Waiting", 36, &lv_font_montserrat_12, COL_VALUE);
    lv_obj_set_height(wl, LV_SIZE_CONTENT);
    wl = scr_add_label(s_scr_waiting, "for",     52, &lv_font_montserrat_12, COL_VALUE);
    lv_obj_set_height(wl, LV_SIZE_CONTENT);
    wl = scr_add_label(s_scr_waiting, "Link",    68, &lv_font_montserrat_12, COL_VALUE);
    lv_obj_set_height(wl, LV_SIZE_CONTENT);
    scr_add_label(s_scr_waiting, "Disconnected",
                  OVL_SUB_Y,   &lv_font_montserrat_12, COL_FAIL);
}

static void create_data_screen(void)
{
    s_scr_data = lv_obj_create(NULL);
    init_screen(s_scr_data);
    make_data_header(s_scr_data, 1);
    lv_obj_t *rows = make_rows_container(s_scr_data);
    s_val_switch = add_row(rows, "Switch");
    s_val_port   = add_row(rows, "Port");
    s_val_vlan   = add_row(rows, "VLAN");
    s_val_link   = add_row(rows, "Link");
    s_val_poe    = add_row(rows, "PoE");
    s_val_ip     = add_row(rows, "IP");
    s_val_gw     = add_row(rows, "GW");
    s_val_dns    = add_row(rows, "DNS");
    s_val_dhcp   = add_row(rows, "DHCP");
}

static void create_data2_screen(void)
{
    s_scr_data2 = lv_obj_create(NULL);
    init_screen(s_scr_data2);
    make_data_header(s_scr_data2, 2);
    lv_obj_t *rows = make_rows_container(s_scr_data2);
    s_val_ping  = add_row(rows, "GW Ping");
    s_val_dns2  = add_row(rows, "DNS");
    s_val_wan   = add_row(rows, "WAN");
    s_val_8021x = add_row(rows, "802.1X");
    s_val_desc  = add_row(rows, "Desc");
    s_val_mac   = add_row(rows, "MAC");

    // Our own Ethernet MAC never changes — fill it in once.
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_ETH) == ESP_OK) {
        char buf[20];
        snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        set_row(s_val_mac, "MAC", buf);
    }
}

static void create_ui(void)
{
    create_splash_screen();
    create_waiting_screen();
    create_data_screen();
    create_data2_screen();
}

// ---- Public API -------------------------------------------------------------

esp_err_t lvgl_ui_init(void)
{
    ESP_LOGI(TAG, "Initializing LVGL UI");

    tft_handles_t tft = {0};
    ESP_RETURN_ON_ERROR(tft_init(&tft), TAG, "TFT init failed");

    const lvgl_port_cfg_t port_cfg = {
        .task_priority = 4,
        .task_stack = 8192,
        .task_affinity = 0,
        .task_max_sleep_ms = 500,
        .timer_period_ms = 30,
    };
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "LVGL port init failed");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = tft.io,
        .panel_handle = tft.panel,
        .buffer_size = TFT_H_RES * TFT_V_RES,  // pixels (port multiplies by color_bytes)
        .double_buffer = false,
        .hres = TFT_H_RES,
        .vres = TFT_V_RES,
        .monochrome = false,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags.swap_bytes = true,
        .flags.full_refresh = true,
        .flags.buff_dma = true,
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    if (!disp) {
        ESP_LOGE(TAG, "Failed to add display to LVGL");
        return ESP_FAIL;
    }

    lvgl_port_lock(0);
    create_ui();
    lvgl_port_unlock();

    ESP_LOGI(TAG, "LVGL UI initialized");
    return ESP_OK;
}

void lvgl_ui_show_splash(void)
{
    lvgl_port_lock(0);
    if (s_scr_splash) lv_screen_load(s_scr_splash);
    lvgl_port_unlock();
}

void lvgl_ui_show_waiting_link(void)
{
    lvgl_port_lock(0);
    if (s_page_timer) {
        lv_timer_delete(s_page_timer);
        s_page_timer = NULL;
    }
    if (s_scr_waiting) lv_screen_load(s_scr_waiting);
    lvgl_port_unlock();
}

static const char *test_status_str(test_status_t s)
{
    switch (s) {
    case TEST_NOT_RUN: return "--";
    case TEST_RUNNING: return "...";
    case TEST_PASS:    return "OK";
    case TEST_FAIL:    return "Fail";
    default:           return "--";
    }
}

void lvgl_ui_show_data(const link_info_t *link)
{
    char buf[64];

    lvgl_port_lock(0);

    // Hold the protocol lock while reading so a frame arriving mid-refresh
    // can't hand us a half-updated record.
    proto_lock();
    const lldp_info_t *lldp = lldp_get_info();
    const cdp_info_t  *cdp  = cdp_get_info();
    const dhcp_info_t *dhcp = dhcp_info_get();

    // On the first call, load page 1 and start the auto-cycle timer.
    // Subsequent calls just update values without disrupting the current page.
    if (!s_page_timer) {
        if (s_scr_data) lv_screen_load(s_scr_data);
        s_page_timer = lv_timer_create(page_cycle_cb, PAGE_CYCLE_MS, NULL);
    }

    set_row(s_val_switch, "Switch", switch_name(lldp, cdp));
    set_row(s_val_port,   "Port",   port_name(lldp, cdp));

    if (lldp && lldp->has_vlan) {
        snprintf(buf, sizeof(buf), "%u", lldp->port_vlan_id);
        set_row(s_val_vlan, "VLAN", buf);
    } else if (cdp && cdp->has_native_vlan) {
        snprintf(buf, sizeof(buf), "%u", cdp->native_vlan);
        set_row(s_val_vlan, "VLAN", buf);
    } else {
        set_row(s_val_vlan, "VLAN", "--");
    }

    if (link->link_up) {
        snprintf(buf, sizeof(buf), "%s %s",
                 link->is_100m ? "100M" : "10M",
                 link->full_duplex ? "Full" : "Half");
        set_row(s_val_link, "Link", buf);
    } else {
        set_row(s_val_link, "Link", "Down");
    }

    if (cdp && cdp->has_power && cdp->power_available_mw > 0) {
        uint32_t watts = cdp->power_available_mw / 1000;
        uint32_t frac  = (cdp->power_available_mw % 1000) / 100;
        if (frac > 0)
            snprintf(buf, sizeof(buf), "%lu.%luW", (unsigned long)watts, (unsigned long)frac);
        else
            snprintf(buf, sizeof(buf), "%luW", (unsigned long)watts);
        set_row(s_val_poe, "PoE", buf);
    } else if (lldp && lldp->has_poe) {
        uint8_t ps = lldp->poe_mdi_power_support;
        set_row(s_val_poe, "PoE",
            ((ps & 0x02) && (ps & 0x04)) ? "Enabled" :
            (ps & 0x02) ? "Supported" : "Yes");
    } else {
        set_row(s_val_poe, "PoE", "--");
    }

    set_row(s_val_ip,   "IP",   dhcp ? dhcp->ip      : "--");
    set_row(s_val_gw,   "GW",   dhcp ? dhcp->gateway : "--");
    set_row(s_val_dns,  "DNS",  dhcp ? dhcp->dns     : "--");
    set_row(s_val_dhcp, "DHCP", dhcp ? "OK"          : "waiting");

    // Page 2 — connectivity / protocol details
    const connectivity_results_t *conn = connectivity_get_results();
    set_row(s_val_ping,  "GW Ping", conn ? test_status_str(conn->ping_gw) : "--");
    set_row(s_val_dns2,  "DNS",     conn ? test_status_str(conn->dns)     : "--");
    set_row(s_val_wan,   "WAN",     conn ? test_status_str(conn->http)    : "--");

    set_row(s_val_8021x, "802.1X", eapol_get_status());

    const char *desc = NULL;
    if (lldp && lldp->system_desc[0]) desc = lldp->system_desc;
    set_row(s_val_desc, "Desc", desc ? desc : "--");

    proto_unlock();
    lvgl_port_unlock();
}

#endif // FEATURE_DISPLAY
