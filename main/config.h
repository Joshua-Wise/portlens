#pragma once

// ============================================================================
// PortLens Board Configuration — Olimex ESP32-PoE-ISO (Rev.M1, WROOM variant)
// PHY: LAN8710A (LAN8720-compatible), galvanic isolation, onboard PoE + LiPo
// ============================================================================

// ============================================================================
// Feature flags
// ============================================================================
#define FEATURE_DISPLAY     1   // 1.44" ST7735 128x128 TFT via LVGL
#define FEATURE_DEMO_MODE   0   // Skip eth_init, inject fake LLDP/DHCP for UI iteration

#define BOARD_NAME            "Olimex ESP32-PoE-ISO"
#define ETH_PHY_ADDR          0
#define ETH_PHY_RST_GPIO      -1  // No hardware reset pin
#define ETH_PHY_PWR_GPIO      12  // PHY power enable (HIGH=ON: NPN Q2/Q3 invert before P-ch FET)
#define ETH_MDC_GPIO          23
#define ETH_MDIO_GPIO         18
#define ETH_CLK_OUT_GPIO_NUM  17

// 1.44" ST7735 128x128 SPI LCD (no touch, backlight tied to 3.3V)
#define TFT_MOSI_GPIO        14
#define TFT_SCK_GPIO         33
#define TFT_CS_GPIO           5  // 10k pull-up (CS HIGH at boot)
#define TFT_DC_GPIO           2  // Strapping pin (LOW at boot) — OK, display DC is high-Z
#define TFT_RST_GPIO         15  // 10k pull-up (not-reset at boot)
#define TFT_H_RES           128
#define TFT_V_RES           128
// ST7735 controller is 132x162; 128x128 panel is offset by (2,1).
// If the image appears shifted up or down, try TFT_Y_OFFSET=3.
#define TFT_X_OFFSET          2
#define TFT_Y_OFFSET          1
#define TFT_SPI_HOST     SPI2_HOST
