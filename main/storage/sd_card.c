#include "storage/sd_card.h"
#include "config.h"

#ifdef SD_CS_GPIO

#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "esp_log.h"

static const char *TAG = "sd_card";
static sdmmc_card_t *s_card = NULL;
static const char *MOUNT_POINT = "/sdcard";

esp_err_t sd_card_init(void)
{
    ESP_LOGI(TAG, "Mounting SD card (CS=%d, MISO=%d, SPI host=%d)",
             SD_CS_GPIO, SD_MISO_GPIO, TFT_SPI_HOST);

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    // SD card device on the existing TFT SPI bus (already initialized)
    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = SD_CS_GPIO;
    slot_cfg.host_id = TFT_SPI_HOST;

    esp_err_t ret = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &(sdmmc_host_t)SDSPI_HOST_DEFAULT(),
                                             &slot_cfg, &mount_cfg, &s_card);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGW(TAG, "Failed to mount FAT filesystem (no card or not formatted)");
        } else {
            ESP_LOGW(TAG, "SD card init failed: %s", esp_err_to_name(ret));
        }
        s_card = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "SD card mounted at %s", MOUNT_POINT);
    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
}

bool sd_card_is_mounted(void)
{
    return s_card != NULL;
}

void sd_card_deinit(void)
{
    if (s_card) {
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_card);
        s_card = NULL;
        ESP_LOGI(TAG, "SD card unmounted");
    }
}

#else // SD_CS_GPIO not defined

esp_err_t sd_card_init(void) { return ESP_ERR_NOT_SUPPORTED; }
bool sd_card_is_mounted(void) { return false; }
void sd_card_deinit(void) {}

#endif
