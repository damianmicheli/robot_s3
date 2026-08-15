#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_psram.h"

#include "button.h"
#include "display.h"
#include "llm.h"
#include "microphone.h"
#include "network.h"
#include "rgb_led.h"
#include "transcription.h"

static const char *TAG = "ROBOT";
static const char *BUTTON_TAG = "BUTTON";

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
    ESP_ERROR_CHECK(microphone_init());

    display_show_standby();
    rgb_led_off();

    if (network_connect() != ESP_OK) ESP_LOGW(TAG, "Wi-Fi no conectado; se reintentara al enviar");

    ESP_LOGI(TAG, "Listo: STAND BY. Pulsa el boton para grabar 5 segundos.");

    while (1)
    {
        if (button_was_pressed())
        {
            ESP_LOGI(BUTTON_TAG, "Grabacion iniciada");
            display_show_recording();
            rgb_led_set_color(255, 0, 0);

            esp_err_t result = microphone_record_five_seconds();
            if (result != ESP_OK) {
                ESP_LOGE(TAG, "Error durante la grabacion: %s", esp_err_to_name(result));
            }

            rgb_led_off();
            display_show_standby();

            /* Deja que la tarea de LVGL envíe STAND BY antes de las llamadas HTTP. */
            vTaskDelay(pdMS_TO_TICKS(100));

            if (result == ESP_OK) {
                result = network_connect();
                if (result == ESP_OK) result = transcription_send_last_recording();
                if (result == ESP_OK) result = llm_answer(transcription_get_last_text());
                if (result != ESP_OK) ESP_LOGE(TAG, "No se pudo transcribir: %s", esp_err_to_name(result));
            }

            /* También recupera la interfaz al terminar o si hubo un error de red. */
            display_show_standby();
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
