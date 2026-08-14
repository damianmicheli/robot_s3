idf_component_register(
    SRCS
        "main.c"
        "button.c"
       
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        esp_driver_gpio
        esp_timer
        spi_flash
        esp_psram
)