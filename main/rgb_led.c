#include "rgb_led.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "led_strip.h"

/*
 * PL9823-F5: LED RGB direccionable de un solo dato.
 * DIN va a GPIO15; cambia este valor si lo cableas a otro GPIO libre.
 */
#define RGB_LED_GPIO GPIO_NUM_15
#define RGB_LED_COUNT 1
#define RGB_LED_BRIGHTNESS_PERCENT 20

static led_strip_handle_t s_led;

esp_err_t rgb_led_init(void)
{
    const led_strip_config_t strip_config = {
        .strip_gpio_num = RGB_LED_GPIO,
        .max_leds = RGB_LED_COUNT,
        /* PL9823 usa la temporización WS2812, pero transmite los canales RGB. */
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_RGB,
    };
    const led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };

    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_led),
                        "RGB_LED", "No se pudo iniciar el LED RGB");
    return led_strip_clear(s_led);
}

void rgb_led_set_color(uint8_t red, uint8_t green, uint8_t blue)
{
    /* Atenúa el LED sin modificar el tono que representa en la pantalla. */
    const uint8_t scaled_red = red * RGB_LED_BRIGHTNESS_PERCENT / 100;
    const uint8_t scaled_green = green * RGB_LED_BRIGHTNESS_PERCENT / 100;
    const uint8_t scaled_blue = blue * RGB_LED_BRIGHTNESS_PERCENT / 100;

    ESP_ERROR_CHECK(led_strip_set_pixel(s_led, 0, scaled_red, scaled_green, scaled_blue));
    /* Hace que el color preparado en memoria llegue físicamente al LED. */
    ESP_ERROR_CHECK(led_strip_refresh(s_led));
}

void rgb_led_off(void)
{
    ESP_ERROR_CHECK(led_strip_clear(s_led));
}
