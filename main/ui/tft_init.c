#include "config.h"

#if FEATURE_DISPLAY

#include "ui/tft_init.h"

#include "esp_check.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7796.h"
#include "esp_lcd_touch_ft5x06.h"
#include "esp_log.h"

static const char *TAG = "tft_init";

esp_err_t tft_init(tft_handles_t *out)
{
    ESP_LOGI(TAG, "Initializing ST7796S TFT (MOSI=%d, SCK=%d, CS=%d, DC=%d, RST=%d)",
             TFT_MOSI_GPIO, TFT_SCK_GPIO, TFT_CS_GPIO, TFT_DC_GPIO, TFT_RST_GPIO);

    // Initialize SPI bus
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = TFT_MOSI_GPIO,
#ifdef SD_MISO_GPIO
        .miso_io_num = SD_MISO_GPIO,   // Shared bus: display is write-only, SD card needs MISO
#else
        .miso_io_num = -1,
#endif
        .sclk_io_num = TFT_SCK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = TFT_H_RES * 80 * sizeof(uint16_t), // 80 lines at a time
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(TFT_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO),
                        TAG, "SPI bus init failed");

    // Create SPI panel IO
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = TFT_CS_GPIO,
        .dc_gpio_num = TFT_DC_GPIO,
        .spi_mode = 0,
        .pclk_hz = 26 * 1000 * 1000,   // 26 MHz (40 MHz causes SPI queue overflows under load)
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(TFT_SPI_HOST, &io_cfg, &io),
                        TAG, "SPI panel IO init failed");

    // Create ST7796 panel
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = TFT_RST_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
    };
    esp_lcd_panel_handle_t panel = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7796(io, &panel_cfg, &panel),
                        TAG, "ST7796 panel init failed");

    // Reset and initialize the panel
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "Panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "Panel init failed");

    // Turn on display (orientation is set via LVGL port rotation config)
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), TAG, "Display on failed");

    out->panel = panel;
    out->io = io;

    ESP_LOGI(TAG, "ST7796S TFT initialized (%dx%d)", TFT_H_RES, TFT_V_RES);
    return ESP_OK;
}

esp_err_t tft_touch_init(esp_lcd_touch_handle_t *out)
{
    ESP_LOGI(TAG, "Initializing FT6336U touch (SDA=%d, SCL=%d)",
             TOUCH_SDA_GPIO, TOUCH_SCL_GPIO);

    // Initialize I2C bus for touch
    i2c_master_bus_config_t i2c_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = TOUCH_SDA_GPIO,
        .scl_io_num = TOUCH_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t i2c_bus = NULL;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&i2c_cfg, &i2c_bus),
                        TAG, "I2C bus init failed");

    // Scan I2C bus to find connected devices
    ESP_LOGI(TAG, "Scanning I2C bus (SDA=%d, SCL=%d)...", TOUCH_SDA_GPIO, TOUCH_SCL_GPIO);
    bool found_0x38 = false;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(i2c_bus, addr, 100) == ESP_OK) {
            ESP_LOGI(TAG, "  I2C device found at 0x%02X%s", addr,
                     (addr == 0x38) ? " (FT6336U)" : "");
            if (addr == 0x38) found_0x38 = true;
        }
    }
    if (!found_0x38) {
        ESP_LOGE(TAG, "FT6336U NOT found at 0x38! Check SDA/SCL wiring.");
    }

    // Create FT5x06 touch controller (FT6336U is register-compatible)
    esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_FT5x06_CONFIG();
    tp_io_cfg.scl_speed_hz = 400000;  // FT6336U supports up to 400kHz
    esp_lcd_panel_io_handle_t tp_io = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c_v2(i2c_bus, &tp_io_cfg, &tp_io),
                        TAG, "Touch panel IO init failed");

    esp_lcd_touch_config_t tp_cfg = {
        .x_max = TFT_H_RES,
        .y_max = TFT_V_RES,
        .rst_gpio_num = -1,         // CTP_RST tied to 3.3V
        .int_gpio_num = -1,         // Use polling mode (GPIO 36 interrupt unreliable on ESP32)
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    esp_lcd_touch_handle_t touch = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_ft5x06(tp_io, &tp_cfg, &touch),
                        TAG, "FT5x06 touch init failed");

    *out = touch;
    ESP_LOGI(TAG, "FT6336U touch initialized");
    return ESP_OK;
}

#endif // FEATURE_DISPLAY
