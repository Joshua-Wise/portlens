#pragma once

#include "config.h"

#if FEATURE_DISPLAY

#include "esp_err.h"
#include "esp_lcd_types.h"

typedef struct {
    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_io_handle_t io;
} tft_handles_t;

/**
 * Initialize SPI bus and ST7735 panel (1.44" 128x128 LCD).
 * Returns panel + IO handles for use with LVGL port.
 */
esp_err_t tft_init(tft_handles_t *out);

#endif // FEATURE_DISPLAY
