#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_psram.h"

#include "button.h"
#include "display.h"

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
    ESP_ERROR_CHECK(display_init());

    ESP_LOGI(TAG, "Boton inicializado en GPIO4");

    while (1)
    {
        if (button_was_pressed())
        {
            ESP_LOGI(BUTTON_TAG, "Boton presionado");
            display_show_button_pressed();
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
