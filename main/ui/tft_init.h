#pragma once

#include "config.h"

#if FEATURE_DISPLAY

#include "esp_err.h"
#include "esp_lcd_types.h"
#include "esp_lcd_touch.h"

typedef struct {
    esp_lcd_panel_handle_t panel;
    esp_lcd_panel_io_handle_t io;
} tft_handles_t;

/**
 * Initialize SPI bus, ST7796S panel, reset and init display.
 * Returns panel + IO handles for use with LVGL port.
 */
esp_err_t tft_init(tft_handles_t *out);

/**
 * Initialize I2C bus and FT6336U capacitive touch controller.
 * Returns touch handle for use with LVGL port.
 */
esp_err_t tft_touch_init(esp_lcd_touch_handle_t *out);

#endif // FEATURE_DISPLAY
