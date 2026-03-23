#pragma once

// ============================================================================
// LLDPocket Board Configuration
// Uncomment ONE board definition below.
// ============================================================================

#define BOARD_OLIMEX_POE_ISO
// #define BOARD_LILYGO_T_INTERNET_POE

// ============================================================================
// Feature flags — enable/disable based on connected hardware
// ============================================================================
#define FEATURE_DISPLAY     1   // TFT display via LVGL
#define FEATURE_TOUCH_NAV   1   // Capacitive touch navigation (FT6336U)
#define FEATURE_BUTTON_NAV  0   // Set to 0 if no external button wired

// ============================================================================
// Olimex ESP32-PoE-ISO (Rev.M1, WROOM variant)
// PHY: LAN8710A (LAN8720-compatible), galvanic isolation, onboard PoE + LiPo
// Verified against official KiCad schematic and Olimex sdkconfig.
// NOTE: WROVER variant requires I2C_SCL on GPIO 33, ETH_CLK on GPIO 0.
// ============================================================================
#ifdef BOARD_OLIMEX_POE_ISO
  #define BOARD_NAME            "Olimex ESP32-PoE-ISO"
  #define ETH_PHY_ADDR          0
  #define ETH_PHY_RST_GPIO      -1  // No hardware reset pin
  #define ETH_PHY_PWR_GPIO      12  // PHY power enable (HIGH=ON: NPN Q2/Q3 invert before P-ch FET)
  #define ETH_MDC_GPIO          23
  #define ETH_MDIO_GPIO         18
  #define ETH_CLK_OUT_GPIO_NUM  17
  #define BUTTON_GPIO           34  // Onboard BUT1, input-only, 10k external pull-up

  // TFT display (ST7796S) — SPI2_HOST via GPIO matrix
  #define TFT_MOSI_GPIO        14  // UEXT header
  #define TFT_SCK_GPIO         33  // EXT header
  #define TFT_CS_GPIO           5  // 10k pull-up (CS HIGH at boot)
  #define TFT_DC_GPIO           2  // Strapping pin (LOW at boot) — OK, display DC is high-Z
  #define TFT_RST_GPIO         15  // 10k pull-up (not-reset at boot)
  #define TFT_H_RES           320
  #define TFT_V_RES           480
  #define TFT_SPI_HOST     SPI2_HOST

  // Capacitive touch (FT6336U) — I2C
  #define TOUCH_SDA_GPIO       13  // EXT2/UEXT, onboard 2.2k pull-up
  #define TOUCH_SCL_GPIO        4  // EXT/UEXT, free
  #define TOUCH_INT_GPIO       36  // Input-only, 2.2k pull-up on display board

  // SD card (on display panel, shares SPI bus with TFT)
  #define SD_CS_GPIO           32
  #define SD_MISO_GPIO         39  // Input-only (SVN), fine for MISO
#endif

// ============================================================================
// LILYGO TTGO T-Internet-POE
// PHY: LAN8720A, prototype/dev board
// ============================================================================
#ifdef BOARD_LILYGO_T_INTERNET_POE
  #define BOARD_NAME            "LILYGO T-Internet-POE"
  #define ETH_PHY_ADDR          0
  #define ETH_PHY_RST_GPIO      5
  #define ETH_MDC_GPIO          23
  #define ETH_MDIO_GPIO         18
  #define ETH_CLK_OUT_GPIO_NUM  17
  #define I2C_SDA_GPIO          4   // GPIO 21/22 unavailable (used by ETH PHY)
  #define I2C_SCL_GPIO          32  // Confirmed working — do NOT use GPIO 12
  #define BUTTON_GPIO           0   // BOOT button (change to 33 for external tactile)
#endif

// Verify a board was selected
#if !defined(BOARD_OLIMEX_POE_ISO) && !defined(BOARD_LILYGO_T_INTERNET_POE)
  #error "No board defined! Uncomment one board in config.h"
#endif
