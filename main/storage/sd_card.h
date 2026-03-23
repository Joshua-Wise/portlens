#pragma once

#include <stdbool.h>
#include "esp_err.h"

// Mount the SD card (SPI mode on the shared TFT SPI bus).
// Returns ESP_OK if mounted, or an error code.
// After success, files are accessible under /sdcard/
esp_err_t sd_card_init(void);

// Returns true if the SD card is mounted and usable
bool sd_card_is_mounted(void);

// Unmount the SD card
void sd_card_deinit(void);
