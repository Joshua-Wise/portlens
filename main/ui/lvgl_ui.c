#include "config.h"

#if FEATURE_DISPLAY

#include "ui/lvgl_ui.h"
#include "ui/tft_init.h"
#include "ui/screens.h"
#include "protocols/lldp_parser.h"
#include "protocols/cdp_parser.h"
#include "protocols/dhcp_info.h"
#include "protocols/eapol_detect.h"
#include "tests/connectivity.h"
#include "tests/cable_test.h"
#include "web/web_server.h"

#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_check.h"
#include "esp_log.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "lvgl_ui";

// ---- Dark mode color palette ------------------------------------------------

#define COL_BG          lv_color_hex(0x0D1117)
#define COL_CARD        lv_color_hex(0x161B22)
#define COL_BORDER      lv_color_hex(0x30363D)
#define COL_LABEL       lv_color_hex(0x8B949E)
#define COL_VALUE       lv_color_hex(0xE6EDF3)
#define COL_PASS        lv_color_hex(0x3FB950)
#define COL_FAIL        lv_color_hex(0xF85149)
#define COL_WARN        lv_color_hex(0xD29922)
#define COL_ACCENT      lv_color_hex(0x58A6FF)

// ---- Layout constants -------------------------------------------------------

#define HEADER_H        40
#define CARD_W          152
#define CARD_H          68
#define CARD_GAP        5
#define CARD_PAD        8
#define CARD_RADIUS     4
#define PAGE_PAD_X      4

// ---- Styles (initialized once) ----------------------------------------------

static lv_style_t style_card;
static lv_style_t style_card_label;
static lv_style_t style_card_value;
static bool s_styles_inited = false;

static void init_styles(void)
{
    if (s_styles_inited) return;
    s_styles_inited = true;

    lv_style_init(&style_card);
    lv_style_set_bg_color(&style_card, COL_CARD);
    lv_style_set_bg_opa(&style_card, LV_OPA_COVER);
    lv_style_set_border_color(&style_card, COL_BORDER);
    lv_style_set_border_width(&style_card, 1);
    lv_style_set_radius(&style_card, CARD_RADIUS);
    lv_style_set_pad_all(&style_card, CARD_PAD);
    lv_style_set_width(&style_card, CARD_W);
    lv_style_set_height(&style_card, CARD_H);

    lv_style_init(&style_card_label);
    lv_style_set_text_font(&style_card_label, &lv_font_montserrat_12);
    lv_style_set_text_color(&style_card_label, COL_LABEL);

    lv_style_init(&style_card_value);
    lv_style_set_text_font(&style_card_value, &lv_font_montserrat_20);
    lv_style_set_text_color(&style_card_value, COL_VALUE);
}

// ---- LVGL object pointers ---------------------------------------------------

// Header
static lv_obj_t *s_header_title  = NULL;
static lv_obj_t *s_status_dot    = NULL;
static lv_obj_t *s_status_text   = NULL;

// Card value labels
static lv_obj_t *s_val_switch    = NULL;
static lv_obj_t *s_val_port      = NULL;
static lv_obj_t *s_val_vlan      = NULL;
static lv_obj_t *s_val_mgmt      = NULL;
static lv_obj_t *s_val_ip        = NULL;
static lv_obj_t *s_val_gw        = NULL;
static lv_obj_t *s_val_dns       = NULL;
static lv_obj_t *s_val_link      = NULL;
static lv_obj_t *s_val_poe       = NULL;
static lv_obj_t *s_val_proto     = NULL;
static lv_obj_t *s_badge_active  = NULL;
static lv_obj_t *s_val_dhcp      = NULL;
static lv_obj_t *s_val_ping_gw   = NULL;
static lv_obj_t *s_val_dns_test  = NULL;
static lv_obj_t *s_val_http      = NULL;
static lv_obj_t *s_val_netmask   = NULL;
static lv_obj_t *s_val_dot1x     = NULL;
static lv_obj_t *s_val_wifi      = NULL;
static lv_obj_t *s_wifi_card     = NULL;

// Single scrollable card container
static lv_obj_t *s_card_cont     = NULL;

// Overlay (splash / waiting / scanning)
static lv_obj_t *s_overlay       = NULL;
static lv_obj_t *s_overlay_text  = NULL;

// State
static bool s_on_data_screen = false;
static screen_id_t s_stored_screen = SCREEN_SUMMARY;
static link_info_t s_last_link = {0};

// ---- Helper: data accessors -------------------------------------------------

static const char *get_switch_name(const lldp_info_t *lldp, const cdp_info_t *cdp)
{
    if (lldp && lldp->system_name[0]) return lldp->system_name;
    if (cdp && cdp->device_id[0])     return cdp->device_id;
    return "--";
}

static const char *get_port_name(const lldp_info_t *lldp, const cdp_info_t *cdp)
{
    if (lldp && lldp->port_id[0]) return lldp->port_id;
    if (cdp && cdp->port_id[0])   return cdp->port_id;
    return "--";
}

static void format_mgmt_addr(char *buf, size_t buflen,
                              const lldp_info_t *lldp, const cdp_info_t *cdp)
{
    if (lldp && lldp->has_mgmt_addr &&
        lldp->mgmt_addr_subtype == 1 && lldp->mgmt_addr_len >= 4) {
        snprintf(buf, buflen, "%u.%u.%u.%u",
                 lldp->mgmt_addr[0], lldp->mgmt_addr[1],
                 lldp->mgmt_addr[2], lldp->mgmt_addr[3]);
    } else if (cdp && cdp->has_mgmt_addr && cdp->mgmt_addr_len >= 4) {
        snprintf(buf, buflen, "%u.%u.%u.%u",
                 cdp->mgmt_addr[0], cdp->mgmt_addr[1],
                 cdp->mgmt_addr[2], cdp->mgmt_addr[3]);
    } else {
        snprintf(buf, buflen, "--");
    }
}

static void set_status_style(lv_obj_t *label, test_status_t status)
{
    lv_color_t color;
    switch (status) {
    case TEST_PASS:    color = COL_PASS; break;
    case TEST_FAIL:    color = COL_FAIL; break;
    case TEST_RUNNING: color = COL_WARN; break;
    default:           color = COL_VALUE; break;
    }
    lv_obj_set_style_text_color(label, color, 0);
}

static const char *status_text(test_status_t s)
{
    switch (s) {
    case TEST_PASS:    return "PASS";
    case TEST_FAIL:    return "FAIL";
    case TEST_RUNNING: return "....";
    default:           return "--";
    }
}

// ---- Helper: create a card, return value label, store card obj ---------------

static lv_obj_t *create_card(lv_obj_t *parent, const char *label_text, lv_obj_t **card_out)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_add_style(card, &style_card, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_grow(card, 0);

    lv_obj_t *lbl = lv_label_create(card);
    lv_obj_add_style(lbl, &style_card_label, 0);
    lv_label_set_text(lbl, label_text);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *val = lv_label_create(card);
    lv_obj_add_style(val, &style_card_value, 0);
    lv_label_set_text(val, "--");
    lv_label_set_long_mode(val, LV_LABEL_LONG_DOT);
    lv_obj_set_width(val, CARD_W - 2 * CARD_PAD);
    lv_obj_align(val, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    if (card_out) *card_out = card;
    return val;
}

// Forward declarations
static void populate_page0(const link_info_t *link);
static void populate_page1(void);

static void wifi_toggle_cb(lv_event_t *e)
{
    (void)e;
#if FEATURE_WIFI_AP
    bool broadcasting = web_server_toggle_ssid_broadcast();
    if (s_val_wifi) {
        if (broadcasting) {
            lv_label_set_text(s_val_wifi, web_server_get_ssid());
            lv_obj_set_style_text_color(s_val_wifi, COL_PASS, 0);
        } else {
            lv_label_set_text(s_val_wifi, "Hidden");
            lv_obj_set_style_text_color(s_val_wifi, COL_LABEL, 0);
        }
    }
#endif
}

// ---- Set header status ------------------------------------------------------

static void set_header_status(const char *text, lv_color_t color)
{
    if (s_status_dot)  lv_obj_set_style_bg_color(s_status_dot, color, 0);
    if (s_status_text) {
        lv_obj_set_style_text_color(s_status_text, color, 0);
        lv_label_set_text(s_status_text, text);
    }
}

// ---- Populate page data (update existing labels) ----------------------------

static void populate_page0(const link_info_t *link)
{
    const lldp_info_t *lldp = lldp_get_info();
    const cdp_info_t  *cdp  = cdp_get_info();
    const dhcp_info_t *dhcp = dhcp_info_get();
    char buf[64];

    lv_label_set_text(s_val_switch, get_switch_name(lldp, cdp));
    lv_label_set_text(s_badge_active, (lldp || cdp) ? LV_SYMBOL_OK : "");

    const char *port_id = get_port_name(lldp, cdp);
    const char *port_desc_str = (lldp && lldp->port_desc[0]) ? lldp->port_desc : NULL;
    if (port_desc_str) {
        snprintf(buf, sizeof(buf), "%.20s (%.20s)", port_id, port_desc_str);
        lv_label_set_text(s_val_port, buf);
    } else {
        lv_label_set_text(s_val_port, port_id);
    }

    if (lldp && lldp->has_vlan) {
        if (lldp->has_vlan_name && lldp->vlan_name[0])
            snprintf(buf, sizeof(buf), "%u %.10s", lldp->port_vlan_id, lldp->vlan_name);
        else
            snprintf(buf, sizeof(buf), "%u", lldp->port_vlan_id);
        lv_label_set_text(s_val_vlan, buf);
    } else if (cdp && cdp->has_native_vlan) {
        snprintf(buf, sizeof(buf), "%u", cdp->native_vlan);
        lv_label_set_text(s_val_vlan, buf);
    } else {
        lv_label_set_text(s_val_vlan, "--");
    }

    format_mgmt_addr(buf, sizeof(buf), lldp, cdp);
    lv_label_set_text(s_val_mgmt, buf);

    lv_label_set_text(s_val_ip, dhcp ? dhcp->ip : "DHCP...");
    lv_label_set_text(s_val_gw, dhcp ? dhcp->gateway : "--");
    lv_label_set_text(s_val_dns, dhcp ? dhcp->dns : "--");

    if (link->link_up) {
        const cable_test_result_t *cable = cable_test_get_result();
        if (cable && cable->quality_str)
            snprintf(buf, sizeof(buf), "%s %s %s",
                     link->is_100m ? "100M" : "10M",
                     link->full_duplex ? "FDX" : "HDX",
                     cable->quality_str);
        else
            snprintf(buf, sizeof(buf), "%s %s",
                     link->is_100m ? "100M" : "10M",
                     link->full_duplex ? "FDX" : "HDX");
        lv_label_set_text(s_val_link, buf);
    } else {
        lv_label_set_text(s_val_link, "Down");
    }

    if (cdp && cdp->has_power && cdp->power_available_mw > 0) {
        uint32_t watts = cdp->power_available_mw / 1000;
        uint32_t frac  = (cdp->power_available_mw % 1000) / 100;
        if (frac > 0)
            snprintf(buf, sizeof(buf), "%lu.%luW", (unsigned long)watts, (unsigned long)frac);
        else
            snprintf(buf, sizeof(buf), "%luW", (unsigned long)watts);
        lv_label_set_text(s_val_poe, buf);
        set_status_style(s_val_poe, TEST_PASS);
    } else if (lldp && lldp->has_poe) {
        uint8_t ps = lldp->poe_mdi_power_support;
        const char *poe_text = ((ps & 0x02) && (ps & 0x04)) ? "Enabled" :
                               (ps & 0x02) ? "Supported" : "Yes";
        lv_label_set_text(s_val_poe, poe_text);
        set_status_style(s_val_poe, TEST_PASS);
    } else {
        lv_label_set_text(s_val_poe, "No");
        lv_obj_set_style_text_color(s_val_poe, COL_VALUE, 0);
    }

    if (lldp) {
        snprintf(buf, sizeof(buf), "LLDP T:%u", lldp->ttl);
        lv_label_set_text(s_val_proto, buf);
    } else if (cdp) {
        snprintf(buf, sizeof(buf), "CDP T:%u", cdp->ttl);
        lv_label_set_text(s_val_proto, buf);
    } else {
        lv_label_set_text(s_val_proto, "waiting...");
    }
}

static void populate_page1(void)
{
    const dhcp_info_t *dhcp = dhcp_info_get();
    const connectivity_results_t *conn = connectivity_get_results();

    lv_label_set_text(s_val_dhcp, dhcp ? dhcp->ip : "waiting...");
    set_status_style(s_val_dhcp, dhcp ? TEST_PASS : TEST_RUNNING);

    lv_label_set_text(s_val_ping_gw, status_text(conn->ping_gw));
    set_status_style(s_val_ping_gw, conn->ping_gw);

    lv_label_set_text(s_val_dns_test, status_text(conn->dns));
    set_status_style(s_val_dns_test, conn->dns);

    lv_label_set_text(s_val_http, status_text(conn->http));
    set_status_style(s_val_http, conn->http);

    lv_label_set_text(s_val_netmask, dhcp ? dhcp->netmask : "--");

    if (eapol_detected()) {
        const char *eapol_status = eapol_get_status();
        lv_label_set_text(s_val_dot1x, eapol_status ? eapol_status : "Detected");
        set_status_style(s_val_dot1x, TEST_FAIL);
    } else {
        lv_label_set_text(s_val_dot1x, "None");
        set_status_style(s_val_dot1x, TEST_PASS);
    }

#if FEATURE_WIFI_AP
    if (s_val_wifi) {
        bool broadcasting = web_server_is_ssid_broadcasting();
        if (broadcasting) {
            lv_label_set_text(s_val_wifi, web_server_get_ssid());
            lv_obj_set_style_text_color(s_val_wifi, COL_PASS, 0);
        } else {
            lv_label_set_text(s_val_wifi, "Hidden");
            lv_obj_set_style_text_color(s_val_wifi, COL_LABEL, 0);
        }
    }
#else
    if (s_val_wifi) {
        lv_label_set_text(s_val_wifi, "Disabled");
        lv_obj_set_style_text_color(s_val_wifi, COL_LABEL, 0);
    }
#endif
}

static void populate_all_cards(const link_info_t *link)
{
    populate_page0(link);
    populate_page1();
}

// ---- UI creation ------------------------------------------------------------

static void create_ui(void)
{
    init_styles();

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // ---- Header bar ----
    lv_obj_t *header = lv_obj_create(scr);
    lv_obj_set_size(header, TFT_H_RES, HEADER_H);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_hor(header, 8, 0);
    lv_obj_set_style_pad_ver(header, 0, 0);
    lv_obj_align(header, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_header_title = lv_label_create(header);
    lv_obj_set_style_text_font(s_header_title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_header_title, COL_VALUE, 0);
    lv_label_set_text(s_header_title, "LLDPocket");

    lv_obj_t *status_area = lv_obj_create(header);
    lv_obj_set_size(status_area, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(status_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(status_area, 0, 0);
    lv_obj_set_style_pad_all(status_area, 0, 0);
    lv_obj_remove_flag(status_area, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(status_area, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_area, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(status_area, 6, 0);

    s_status_dot = lv_obj_create(status_area);
    lv_obj_set_size(s_status_dot, 8, 8);
    lv_obj_set_style_radius(s_status_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_status_dot, COL_WARN, 0);
    lv_obj_set_style_bg_opa(s_status_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_status_dot, 0, 0);
    lv_obj_remove_flag(s_status_dot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_status_text = lv_label_create(status_area);
    lv_obj_set_style_text_font(s_status_text, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_status_text, COL_WARN, 0);
    lv_label_set_text(s_status_text, "Starting");

    // ---- Separator ----
    lv_obj_t *sep = lv_obj_create(scr);
    lv_obj_set_size(sep, TFT_H_RES, 1);
    lv_obj_set_style_bg_color(sep, COL_BORDER, 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sep, 0, 0);
    lv_obj_set_style_radius(sep, 0, 0);
    lv_obj_set_style_pad_all(sep, 0, 0);
    lv_obj_align(sep, LV_ALIGN_TOP_LEFT, 0, HEADER_H);
    lv_obj_remove_flag(sep, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // ---- Single scrollable card container (all cards, vertical scroll) ----
    int content_y = HEADER_H + 1;
    int content_h = TFT_V_RES - content_y;

    s_card_cont = lv_obj_create(scr);
    lv_obj_set_pos(s_card_cont, 0, content_y);
    lv_obj_set_size(s_card_cont, TFT_H_RES, content_h);
    lv_obj_set_style_bg_color(s_card_cont, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_card_cont, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_card_cont, 0, 0);
    lv_obj_set_style_radius(s_card_cont, 0, 0);
    lv_obj_set_style_pad_all(s_card_cont, PAGE_PAD_X, 0);
    lv_obj_set_style_pad_row(s_card_cont, CARD_GAP, 0);
    lv_obj_set_style_pad_column(s_card_cont, CARD_GAP, 0);
    lv_obj_set_flex_flow(s_card_cont, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(s_card_cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    // Enable vertical scrolling with scrollbar (CLICKABLE needed for touch scroll)
    lv_obj_add_flag(s_card_cont, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(s_card_cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_card_cont, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_snap_y(s_card_cont, LV_SCROLL_SNAP_NONE);

    // ---- All cards in one container ----
    s_val_switch = create_card(s_card_cont, "Switch", NULL);
    s_badge_active = lv_label_create(lv_obj_get_parent(s_val_switch));
    lv_obj_set_style_text_font(s_badge_active, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_badge_active, COL_PASS, 0);
    lv_label_set_text(s_badge_active, "");
    lv_obj_align(s_badge_active, LV_ALIGN_TOP_RIGHT, 0, 0);

    s_val_port     = create_card(s_card_cont, "Port",         NULL);
    s_val_vlan     = create_card(s_card_cont, "VLAN",         NULL);
    s_val_mgmt     = create_card(s_card_cont, "Mgmt IP",      NULL);
    s_val_ip       = create_card(s_card_cont, "IP Address",    NULL);
    s_val_gw       = create_card(s_card_cont, "Gateway",      NULL);
    s_val_dns      = create_card(s_card_cont, "DNS",           NULL);
    s_val_link     = create_card(s_card_cont, "Link Speed",    NULL);
    s_val_poe      = create_card(s_card_cont, "PoE",           NULL);
    s_val_proto    = create_card(s_card_cont, "Protocol",      NULL);
    s_val_dhcp     = create_card(s_card_cont, "DHCP",          NULL);
    s_val_ping_gw  = create_card(s_card_cont, "Gateway Ping",  NULL);
    s_val_dns_test = create_card(s_card_cont, "DNS Test",      NULL);
    s_val_http     = create_card(s_card_cont, "HTTP / WAN",    NULL);
    s_val_netmask  = create_card(s_card_cont, "Netmask",       NULL);
    s_val_dot1x    = create_card(s_card_cont, "802.1X",        NULL);
    s_val_wifi     = create_card(s_card_cont, "WiFi AP",       &s_wifi_card);
    lv_obj_add_flag(s_wifi_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_wifi_card, wifi_toggle_cb, LV_EVENT_CLICKED, NULL);
}

// ---- Public API -------------------------------------------------------------

esp_err_t lvgl_ui_init(void)
{
    ESP_LOGI(TAG, "Initializing LVGL UI");

    tft_handles_t tft = {0};
    ESP_RETURN_ON_ERROR(tft_init(&tft), TAG, "TFT init failed");

    esp_lcd_touch_handle_t touch = NULL;
    esp_err_t touch_ret = tft_touch_init(&touch);
    if (touch_ret != ESP_OK) {
        ESP_LOGE(TAG, "Touch init FAILED: %s", esp_err_to_name(touch_ret));
        touch = NULL;
    } else {
        ESP_LOGI(TAG, "Touch init OK");
    }

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
        .buffer_size = TFT_H_RES * 20 * sizeof(lv_color16_t),
        .double_buffer = false,
        .hres = TFT_H_RES,
        .vres = TFT_V_RES,
        .monochrome = false,
        .rotation = {
            .swap_xy = false,
            .mirror_x = true,
            .mirror_y = false,
        },
        .flags.swap_bytes = true,
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    if (!disp) {
        ESP_LOGE(TAG, "Failed to add display to LVGL");
        return ESP_FAIL;
    }

    if (touch) {
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = disp,
            .handle = touch,
        };
        lv_indev_t *indev = lvgl_port_add_touch(&touch_cfg);
        if (!indev) {
            ESP_LOGW(TAG, "Failed to add touch to LVGL");
        }
    }

    lvgl_port_lock(0);
    create_ui();
    lvgl_port_unlock();

    ESP_LOGI(TAG, "LVGL UI initialized");
    return ESP_OK;
}

static void destroy_overlay(void)
{
    if (s_overlay) {
        lv_obj_delete(s_overlay);
        s_overlay = NULL;
        s_overlay_text = NULL;
        lv_obj_remove_flag(lv_layer_top(), LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(lv_layer_top(), LV_OBJ_FLAG_SCROLLABLE);
    }
}

static void create_overlay(const char *text)
{
    destroy_overlay();

    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_overlay, TFT_H_RES, TFT_V_RES - HEADER_H - 1);
    lv_obj_set_pos(s_overlay, 0, HEADER_H + 1);
    lv_obj_set_style_bg_color(s_overlay, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_radius(s_overlay, 0, 0);
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_overlay_text = lv_label_create(s_overlay);
    lv_obj_set_style_text_font(s_overlay_text, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_overlay_text, COL_VALUE, 0);
    lv_obj_set_style_text_align(s_overlay_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_overlay_text, TFT_H_RES - 32);
    lv_obj_align(s_overlay_text, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(s_overlay_text, text);
}

void lvgl_ui_show_splash(void)
{
    s_on_data_screen = false;
    lvgl_port_lock(0);
    set_header_status("Starting", COL_WARN);
    create_overlay("LLDPocket\n\nPocket LLDP/CDP\nNetwork Tester\n\nInitializing...");
    lvgl_port_unlock();
}

void lvgl_ui_show_waiting_link(void)
{
    s_on_data_screen = false;
    lvgl_port_lock(0);
    set_header_status("No Link", COL_FAIL);
    create_overlay("Waiting for link...\n\nPlug in Ethernet cable");
    lvgl_port_unlock();
}

void lvgl_ui_show_scanning(const link_info_t *link)
{
    s_on_data_screen = false;
    char buf[128];
    snprintf(buf, sizeof(buf),
        "Link: %s %s\n\nScanning...\n\nListening for\nLLDP/CDP frames",
        link->is_100m ? "100M" : "10M",
        link->full_duplex ? "Full-Duplex" : "Half-Duplex");
    lvgl_port_lock(0);
    set_header_status("Scanning", COL_WARN);
    create_overlay(buf);
    lvgl_port_unlock();
}

void lvgl_ui_show_screen(screen_id_t id, const link_info_t *link)
{
    s_on_data_screen = true;
    s_stored_screen = id;
    s_last_link = *link;

    lvgl_port_lock(0);
    set_header_status("Connected", COL_PASS);
    destroy_overlay();
    populate_all_cards(link);
    lvgl_port_unlock();
}

screen_id_t lvgl_ui_get_current_screen(void)
{
    return s_stored_screen;
}

#endif // FEATURE_DISPLAY
