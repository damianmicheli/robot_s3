idf_component_register(
    SRCS
        "main.c"
        "button.c"
        "display.c"
        "font_roboto_24.c"
        "font_merienda_18.c"
        "font_merienda_24.c"
       
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        esp_driver_gpio
        esp_timer
        spi_flash
        esp_psram
        esp_driver_spi
        esp_lcd
)
