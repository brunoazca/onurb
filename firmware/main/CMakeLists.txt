idf_component_register(SRCS "bringup_main.c" "pmu.cpp" "wifi.c" "ui.c" "turn.c" "wav.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_driver_gpio esp_driver_i2c esp_psram spi_flash
                                esp_wifi esp_netif esp_event esp_timer nvs_flash esp_http_client mbedtls)
