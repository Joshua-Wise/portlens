idf_component_register(
    SRC_DIRS "." "eth" "protocols" "ui" "tests" "web" "storage"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_eth esp_netif esp_http_client esp_http_server lwip nvs_flash esp_driver_gpio esp_driver_i2c esp_driver_spi esp_lcd esp_psram esp_wifi fatfs sdmmc esp_timer
)
