idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES bme68x esp_driver_i2c bsec2_src esp_timer fatfs sdmmc esp_driver_sdspi esp_driver_spi
)