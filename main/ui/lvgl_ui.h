#pragma once

#include "config.h"

#if FEATURE_DISPLAY

#include "esp_err.h"
#include "ui/screens.h"

/**
 * Initialize TFT hardware, LVGL, touch input, and create UI objects.
 * Starts the LVGL tick/render task internally.
 */
esp_err_t lvgl_ui_init(void);

/** Show boot splash screen. */
void lvgl_ui_show_splash(void);

/** Show "waiting for Ethernet link" screen. */
void lvgl_ui_show_waiting_link(void);

/** Show "scanning for LLDP/CDP" screen with link info. */
void lvgl_ui_show_scanning(const link_info_t *link);

/** Show one of the 4 data screens. */
void lvgl_ui_show_screen(screen_id_t id, const link_info_t *link);

/** Get current screen (may change via touch gesture). */
screen_id_t lvgl_ui_get_current_screen(void);

#endif // FEATURE_DISPLAY
