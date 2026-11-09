idf_component_register(SRCS "bringup_main.c" "pmu.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES esp_driver_gpio esp_driver_i2c esp_psram spi_flash)
