#include "config.h"

#if FEATURE_DISPLAY

#include "ui/tft_init.h"

#include "esp_check.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "tft_init";

// ST7735 register addresses
#define ST7735_SWRESET  0x01
#define ST7735_SLPOUT   0x11
#define ST7735_NORON    0x13
#define ST7735_INVOFF   0x20
#define ST7735_INVON    0x21
#define ST7735_DISPON   0x29
#define ST7735_CASET    0x2A
#define ST7735_RASET    0x2B
#define ST7735_MADCTL   0x36
#define ST7735_COLMOD   0x3A
#define ST7735_FRMCTR1  0xB1
#define ST7735_FRMCTR2  0xB2
#define ST7735_FRMCTR3  0xB3
#define ST7735_INVCTR   0xB4
#define ST7735_PWCTR1   0xC0
#define ST7735_PWCTR2   0xC1
#define ST7735_PWCTR3   0xC2
#define ST7735_PWCTR4   0xC3
#define ST7735_PWCTR5   0xC4
#define ST7735_VMCTR1   0xC5
#define ST7735_GMCTRP1  0xE0
#define ST7735_GMCTRN1  0xE1

// MADCTL bits
#define MADCTL_MY   0x80
#define MADCTL_MX   0x40
#define MADCTL_MV   0x20
#define MADCTL_BGR  0x08

static esp_err_t st7735_send_cmd(esp_lcd_panel_io_handle_t io, int cmd,
                                  const void *data, size_t data_len)
{
    return esp_lcd_panel_io_tx_param(io, cmd, data, data_len);
}

static esp_err_t st7735_init_panel(esp_lcd_panel_io_handle_t io)
{
    // Reset
    ESP_RETURN_ON_ERROR(st7735_send_cmd(io, ST7735_SWRESET, NULL, 0), TAG, "SWRESET");
    vTaskDelay(pdMS_TO_TICKS(150));

    // Sleep out
    ESP_RETURN_ON_ERROR(st7735_send_cmd(io, ST7735_SLPOUT, NULL, 0), TAG, "SLPOUT");
    vTaskDelay(pdMS_TO_TICKS(500));

    // Frame rate control (normal / idle / partial)
    ESP_RETURN_ON_ERROR(st7735_send_cmd(io, ST7735_FRMCTR1,
        (uint8_t[]){0x01, 0x2C, 0x2D}, 3), TAG, "FRMCTR1");
    ESP_RETURN_ON_ERROR(st7735_send_cmd(io, ST7735_FRMCTR2,
        (uint8_t[]){0x01, 0x2C, 0x2D}, 3), TAG, "FRMCTR2");
    ESP_RETURN_ON_ERROR(st7735_send_cmd(io, ST7735_FRMCTR3,
        (uint8_t[]){0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D}, 6), TAG, "FRMCTR3");

    // Display inversion (column inversion)
    ESP_RETURN_ON_ERROR(st7735_send_cmd(io, ST7735_INVCTR,
        (uint8_t[]){0x07}, 1), TAG, "INVCTR");

    // Power control
    ESP_RETURN_ON_ERROR(st7735_send_cmd(io, ST7735_PWCTR1,
        (uint8_t[]){0xA2, 0x02, 0x84}, 3), TAG, "PWCTR1");
    ESP_RETURN_ON_ERROR(st7735_send_cmd(io, ST7735_PWCTR2,
        (uint8_t[]){0xC5}, 1), TAG, "PWCTR2");
    ESP_RETURN_ON_ERROR(st7735_send_cmd(io, ST7735_PWCTR3,
        (uint8_t[]){0x0A, 0x00}, 2), TAG, "PWCTR3");
    ESP_RETURN_ON_ERROR(st7735_send_cmd(io, ST7735_PWCTR4,
        (uint8_t[]){0x8A, 0x2A}, 2), TAG, "PWCTR4");
    ESP_RETURN_ON_ERROR(st7735_send_cmd(io, ST7735_PWCTR5,
        (uint8_t[]){0x8A, 0xEE}, 2), TAG, "PWCTR5");

    // VCOM
    ESP_RETURN_ON_ERROR(st7735_send_cmd(io, ST7735_VMCTR1,
        (uint8_t[]){0x0E}, 1), TAG, "VMCTR1");

    // Memory access: MX+MY for 180° rotation, no BGR (this module uses RGB order)
    ESP_RETURN_ON_ERROR(st7735_send_cmd(io, ST7735_MADCTL,
        (uint8_t[]){MADCTL_MX | MADCTL_MY}, 1), TAG, "MADCTL");

    // 16-bit color
    ESP_RETURN_ON_ERROR(st7735_send_cmd(io, ST7735_COLMOD,
        (uint8_t[]){0x05}, 1), TAG, "COLMOD");

    // Column address window: 2..129 (128 pixels starting at offset 2)
    ESP_RETURN_ON_ERROR(st7735_send_cmd(io, ST7735_CASET,
        (uint8_t[]){0x00, TFT_X_OFFSET, 0x00, TFT_X_OFFSET + TFT_H_RES - 1},
        4), TAG, "CASET");
    // Row address window: 1..128 (128 pixels starting at offset 1)
    ESP_RETURN_ON_ERROR(st7735_send_cmd(io, ST7735_RASET,
        (uint8_t[]){0x00, TFT_Y_OFFSET, 0x00, TFT_Y_OFFSET + TFT_V_RES - 1},
        4), TAG, "RASET");

    // Gamma
    ESP_RETURN_ON_ERROR(st7735_send_cmd(io, ST7735_GMCTRP1,
        (uint8_t[]){0x02, 0x1C, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2D,
                    0x29, 0x25, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10}, 16),
        TAG, "GMCTRP1");
    ESP_RETURN_ON_ERROR(st7735_send_cmd(io, ST7735_GMCTRN1,
        (uint8_t[]){0x03, 0x1D, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D,
                    0x2E, 0x2E, 0x37, 0x3F, 0x00, 0x00, 0x02, 0x10}, 16),
        TAG, "GMCTRN1");

    // Normal display mode on
    ESP_RETURN_ON_ERROR(st7735_send_cmd(io, ST7735_NORON, NULL, 0), TAG, "NORON");
    vTaskDelay(pdMS_TO_TICKS(10));

    // Display on
    ESP_RETURN_ON_ERROR(st7735_send_cmd(io, ST7735_DISPON, NULL, 0), TAG, "DISPON");
    vTaskDelay(pdMS_TO_TICKS(100));

    return ESP_OK;
}

esp_err_t tft_init(tft_handles_t *out)
{
    ESP_LOGI(TAG, "Initializing ST7735 TFT (MOSI=%d, SCK=%d, CS=%d, DC=%d, RST=%d)",
             TFT_MOSI_GPIO, TFT_SCK_GPIO, TFT_CS_GPIO, TFT_DC_GPIO, TFT_RST_GPIO);

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = TFT_MOSI_GPIO,
        .miso_io_num = -1,
        .sclk_io_num = TFT_SCK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = TFT_H_RES * TFT_V_RES * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(TFT_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO),
                        TAG, "SPI bus init failed");

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = TFT_CS_GPIO,
        .dc_gpio_num = TFT_DC_GPIO,
        .spi_mode = 0,
        .pclk_hz = 20 * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(TFT_SPI_HOST, &io_cfg, &io),
                        TAG, "SPI panel IO init failed");

    // Assert hardware reset via GPIO before sending init commands
    gpio_set_direction(TFT_RST_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(TFT_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(TFT_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    ESP_RETURN_ON_ERROR(st7735_init_panel(io), TAG, "ST7735 init sequence failed");

    // Use the ST7789 panel driver for its LVGL-compatible draw/flush callbacks.
    // CASET/RASET/RAMWR commands are identical between ST7735 and ST7789.
    // We pass skip_init_device=true so the driver doesn't overwrite our init.
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = -1,   // reset already done above
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = 16,
        .flags.reset_active_high = 0,
    };
    esp_lcd_panel_handle_t panel = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io, &panel_cfg, &panel),
                        TAG, "Panel driver init failed");

    // Skip esp_lcd_panel_reset / esp_lcd_panel_init — the ST7735 is already
    // initialized above. Calling panel_init() would overwrite our settings.

    // Set the same address window the ST7735 init established.
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(panel, TFT_X_OFFSET, TFT_Y_OFFSET),
                        TAG, "Panel set_gap failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(panel, true), TAG, "Display on failed");

    out->panel = panel;
    out->io = io;

    ESP_LOGI(TAG, "ST7735 TFT initialized (%dx%d, gap=%d,%d)",
             TFT_H_RES, TFT_V_RES, TFT_X_OFFSET, TFT_Y_OFFSET);
    return ESP_OK;
}

#endif // FEATURE_DISPLAY
