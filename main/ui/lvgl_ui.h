#pragma once

#include "config.h"

#if FEATURE_DISPLAY

#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    bool link_up;
    bool is_100m;
    bool full_duplex;
} link_info_t;

/**
 * Initialize TFT hardware, LVGL port, and create the static UI tree.
 * Starts the LVGL tick/render task internally.
 */
esp_err_t lvgl_ui_init(void);

/** Show the boot splash ("PortLens" + portlens.dev). */
void lvgl_ui_show_splash(void);

/** Show "Waiting for link..." — no Ethernet cable connected. */
void lvgl_ui_show_waiting_link(void);

/** Show "Parsing..." — link is up but no LLDP/CDP/DHCP yet. */
void lvgl_ui_show_scanning(const link_info_t *link);

/** Show the full data list using whatever protocol/DHCP data is available. */
void lvgl_ui_show_data(const link_info_t *link);

#endif // FEATURE_DISPLAY
