idf_component_register(
    SRCS
        "main.c"
        "button.c"
        "display.c"
        "llm.c"
        "microphone.c"
        "network.c"
        "rgb_led.c"
        "transcription.c"
        "font_merienda_24.c"
       
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        esp_driver_gpio
        esp_driver_i2s
        esp_event
        esp_http_client
        esp_netif
        esp_wifi
        nvs_flash
        esp_timer
        spi_flash
        esp_psram
        esp_driver_spi
        esp_lcd
)
