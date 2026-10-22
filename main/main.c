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
#include "speaker.h"
#include "tts.h"
#include "wake_word.h"

static const char *TAG = "ROBOT";

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
    ESP_ERROR_CHECK(speaker_init());
    ESP_ERROR_CHECK(wake_word_init());

    display_show_standby();
    /* Microfono encendido al inicio: LED en verde */
    rgb_led_set_color(0, 255, 0);
    ESP_ERROR_CHECK(speaker_play_startup_tone());

    if (network_connect() != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi no conectado; se reintentara al enviar");
    }

    /* Inicia la escucha continua de la wake word "Hola Loki" */
    wake_word_start();

    ESP_LOGI(TAG, "Robot listo. Escuchando 'Hola Loki' (Boton = Silenciar/Activar microfono).");

    while (1)
    {
        /* Gestion del boton fisico: Silenciar / Activar microfono */
        if (button_was_pressed())
        {
            wake_word_toggle_mute();
            if (wake_word_is_muted()) {
                /* Microfono apagado: LED apagado */
                rgb_led_off();
                ESP_LOGW(TAG, "Modo silencio activado (Microfono OFF)");
            } else {
                /* Microfono encendido: LED verde */
                rgb_led_set_color(0, 255, 0);
                display_show_standby();
                ESP_LOGI(TAG, "Modo escucha activado (Microfono ON)");
            }
        }

        /* Deteccion de la palabra de activacion "Hola Loki" */
        if (!wake_word_is_muted() && wake_word_detected())
        {
            wake_word_clear_detected();
            ESP_LOGI(TAG, "Activacion por voz: 'Hola Loki'");

            /* Carita atenta y LED rojo de grabacion */
            display_show_recording();
            rgb_led_set_color(255, 0, 0);

            /* Graba 5 segundos la pregunta del usuario */
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_err_t result = microphone_record_five_seconds();
            if (result != ESP_OK) {
                ESP_LOGE(TAG, "Error durante la grabacion: %s", esp_err_to_name(result));
            }

            /* Carita pensando y LED ambar mientras procesa STT y Ollama */
            display_show_thinking();
            rgb_led_set_color(255, 140, 0);
            vTaskDelay(pdMS_TO_TICKS(100));

            if (result == ESP_OK) {
                result = network_connect();
                if (result == ESP_OK) {
                    result = transcription_send_last_recording();
                }
                if (result == ESP_OK) {
                    result = llm_answer(transcription_get_last_text());
                }
                if (result == ESP_OK) {
                    /* Carita hablando con boca animada y LED cian */
                    display_show_speaking();
                    rgb_led_set_color(0, 180, 255);
                    result = tts_speak(llm_get_last_answer());
                }
                if (result != ESP_OK) {
                    ESP_LOGE(TAG, "No se pudo completar la consulta: %s", esp_err_to_name(result));
                }
            }

            /* Regresa a reposo y reanuda la escucha continua */
            if (wake_word_is_muted()) {
                rgb_led_off();
            } else {
                rgb_led_set_color(0, 255, 0);
            }
            display_show_standby();
            wake_word_start();
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
