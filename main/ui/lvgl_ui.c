#include "config.h"

#if FEATURE_DISPLAY

#include "ui/lvgl_ui.h"
#include "ui/tft_init.h"
#include "protocols/lldp_parser.h"
#include "protocols/cdp_parser.h"
#include "protocols/dhcp_info.h"

#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_check.h"
#include "esp_log.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "lvgl_ui";

// ---- Palette ----------------------------------------------------------------

#define COL_BG          lv_color_hex(0x000000)
#define COL_DIM         lv_color_hex(0x555B66)
#define COL_LABEL       lv_color_hex(0xC8D1DA)
#define COL_VALUE       lv_color_hex(0xFFFFFF)
#define COL_ACCENT      lv_color_hex(0x86EFAC)  // light green — PortLens brand
#define COL_PASS        lv_color_hex(0x3FB950)
#define COL_WARN        lv_color_hex(0x7DD3FC)  // light blue — Starting
#define COL_FAIL        lv_color_hex(0xFCA5A5)  // light red — Disconnected

// ---- Layout -----------------------------------------------------------------

#define HEADER_H        12
#define SIDE_PAD         4
#define OVL_LINE_H      22
#define OVL_BRAND_Y      6
#define OVL_L1_Y        30
#define OVL_L2_Y        52
#define OVL_L3_Y        74
#define OVL_SUB_Y      104

// ---- Static UI handles ------------------------------------------------------

// Separate LVGL screens for each UI state.
// lv_obj_create(NULL) creates a screen with no parent — no clipping at all.
// Switch between them with lv_screen_load().
static lv_obj_t *s_scr_splash  = NULL;
static lv_obj_t *s_scr_waiting = NULL;
static lv_obj_t *s_scr_data    = NULL;

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
    if (lldp && lldp->port_id[0]) return lldp->port_id;
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

    // Header
    lv_obj_t *header = lv_obj_create(s_scr_data);
    lv_obj_set_size(header, TFT_H_RES, HEADER_H);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_align(header, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_remove_flag(header, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *header_title = lv_label_create(header);
    lv_obj_set_style_text_font(header_title, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(header_title, COL_ACCENT, 0);
    lv_label_set_text(header_title, "PortLens");
    lv_obj_align(header_title, LV_ALIGN_CENTER, 0, 0);

    // Separator
    lv_obj_t *sep = lv_obj_create(s_scr_data);
    lv_obj_set_size(sep, TFT_H_RES, 1);
    lv_obj_set_style_bg_color(sep, COL_DIM, 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sep, 0, 0);
    lv_obj_set_style_radius(sep, 0, 0);
    lv_obj_set_style_pad_all(sep, 0, 0);
    lv_obj_align(sep, LV_ALIGN_TOP_LEFT, 0, HEADER_H);
    lv_obj_remove_flag(sep, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // Data rows container
    int content_y = HEADER_H + 1;
    int content_h = TFT_V_RES - content_y;

    lv_obj_t *rows = lv_obj_create(s_scr_data);
    lv_obj_set_pos(rows, 0, content_y);
    lv_obj_set_size(rows, TFT_H_RES, content_h);
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

static void create_ui(void)
{
    create_splash_screen();
    create_waiting_screen();
    create_data_screen();
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
    if (s_scr_waiting) lv_screen_load(s_scr_waiting);
    lvgl_port_unlock();
}

void lvgl_ui_show_scanning(const link_info_t *link)
{
    lvgl_ui_show_waiting_link();
}

void lvgl_ui_show_data(const link_info_t *link)
{
    const lldp_info_t *lldp = lldp_get_info();
    const cdp_info_t  *cdp  = cdp_get_info();
    const dhcp_info_t *dhcp = dhcp_info_get();
    char buf[64];

    lvgl_port_lock(0);

    if (s_scr_data) lv_screen_load(s_scr_data);

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

    lvgl_port_unlock();
}

#endif // FEATURE_DISPLAY
