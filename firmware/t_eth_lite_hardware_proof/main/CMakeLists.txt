idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        esp_eth
        esp_event
        esp_netif
        esp_psram
        esp_wifi
        lwip
        nvs_flash
        spi_flash
)
