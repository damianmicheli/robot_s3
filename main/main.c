#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_timer.h"

#include "button.h"
#include "display.h"
#include "rgb_led.h"

static const char *TAG = "ROBOT";
static const char *BUTTON_TAG = "BUTTON";
#define BUTTON_HOLD_TO_TOGGLE_OUTPUTS_MS 4000

void app_main(void)
{
    uint32_t flash_size = 0;
    ESP_ERROR_CHECK(esp_flash_get_physical_size(NULL, &flash_size));

    size_t psram_size = esp_psram_get_size();

    ESP_LOGI(
        TAG,
        "Memoria detectada: Flash %lu MB | PSRAM %u MB",
        (unsigned long)(flash_size / (1024 * 1024)),
        (unsigned int)(psram_size / (1024 * 1024))
    );

    button_init();
    ESP_ERROR_CHECK(rgb_led_init());
    ESP_ERROR_CHECK(display_init());

    ESP_LOGI(TAG, "Boton inicializado en GPIO4");

    int64_t pressed_since_ms = -1;
    bool outputs_are_off = false;
    bool long_press_handled = false;

    while (1)
    {
        if (button_was_pressed())
        {
            pressed_since_ms = esp_timer_get_time() / 1000;
            long_press_handled = false;
            if (!outputs_are_off) {
                ESP_LOGI(BUTTON_TAG, "Boton presionado");
                display_show_button_pressed();
            }
        }

        if (!button_is_pressed())
        {
            pressed_since_ms = -1;
            /* La proxima pulsacion puede volver a ejecutar una accion larga. */
            long_press_handled = false;
        }
        else if (!long_press_handled && pressed_since_ms >= 0 &&
                 esp_timer_get_time() / 1000 - pressed_since_ms >= BUTTON_HOLD_TO_TOGGLE_OUTPUTS_MS)
        {
            /*
             * Se ejecuta una vez por pulsacion: al cambiar el estado se evita que
             * siga alternando mientras el boton continua presionado.
             */
            if (outputs_are_off) {
                display_set_power(true);
                outputs_are_off = false;
                display_show_button_pressed();
                ESP_LOGI(BUTTON_TAG, "Pantalla y LED encendidos por pulsacion larga");
            } else {
                display_set_power(false);
                rgb_led_off();
                outputs_are_off = true;
                ESP_LOGI(BUTTON_TAG, "Pantalla y LED apagados por pulsacion larga");
            }
            long_press_handled = true;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
