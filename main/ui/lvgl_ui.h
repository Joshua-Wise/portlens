#pragma once

#include "config.h"

#include <stdbool.h>

// Used by main.c regardless of FEATURE_DISPLAY.
typedef struct {
    bool link_up;
    bool is_100m;
    bool full_duplex;
} link_info_t;

#if FEATURE_DISPLAY

#include "esp_err.h"

/**
 * Initialize TFT hardware, LVGL port, and create the static UI tree.
 * Starts the LVGL tick/render task internally.
 */
esp_err_t lvgl_ui_init(void);

/** Show the boot splash. */
void lvgl_ui_show_splash(void);

/** Show "Waiting for link..." — no Ethernet cable connected. */
void lvgl_ui_show_waiting_link(void);

/** Show the full data list using whatever protocol/DHCP data is available. */
void lvgl_ui_show_data(const link_info_t *link);

#endif // FEATURE_DISPLAY
