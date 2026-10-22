#include <stdint.h>
#include <string.h>

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

/* Comprueba si el texto devuelto por Whisper es silencio o ruido vacio */
static bool is_silent_or_empty(const char *text)
{
    if (text == NULL) return true;

    /* Filtrar alucinaciones frecuentes de Whisper en audio silencioso */
    if (strstr(text, "Subtítulos") || strstr(text, "subtítulos") ||
        strstr(text, "Subtitulado") || strstr(text, "subtitulado") ||
        strstr(text, "Amara.org")   || strstr(text, "amara.org")) {
        return true;
    }

    while (*text) {
        unsigned char c = (unsigned char)*text;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c >= 0x80) {
            return false;
        }
        text++;
    }
    return true;
}

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

    /* Inicia la escucha continua de la wake word "Hola Bombón" */
    wake_word_start();

    ESP_LOGI(TAG, "Robot listo. Escuchando 'Hola Bombón' (Boton = Silenciar/Activar microfono).");

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

        /* Deteccion de la palabra de activacion "Hola Bombón" */
        if (!wake_word_is_muted() && wake_word_detected())
        {
            wake_word_clear_detected();
            wake_word_stop(); /* Pausa escucha continua durante la sesion conversacional */
            ESP_LOGI(TAG, "Activacion por voz: 'Hola Bombón'");

            llm_reset_conversation();

            bool in_conversation = true;
            int consecutive_silence = 0;
            int turn_number = 0;

            while (in_conversation)
            {
                /* Salida manual si se presiona el botón fisico */
                if (button_was_pressed()) {
                    ESP_LOGI(TAG, "Boton presionado: finalizando conversacion");
                    in_conversation = false;
                    break;
                }

                /* 1. Carita atenta y LED rojo de grabacion */
                display_show_recording();
                rgb_led_set_color(255, 0, 0);

                /* 2. Graba la intervencion del usuario (5 segundos) */
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_err_t result = microphone_record_five_seconds();
                if (result != ESP_OK) {
                    ESP_LOGE(TAG, "Error durante la grabacion: %s", esp_err_to_name(result));
                    break;
                }

                /* 3. Carita pensando y LED ambar mientras procesa STT y Ollama */
                display_show_thinking();
                rgb_led_set_color(255, 140, 0);
                vTaskDelay(pdMS_TO_TICKS(50));

                /* 4. Transcribir audio */
                result = network_connect();
                if (result == ESP_OK) {
                    result = transcription_send_last_recording();
                }

                if (result != ESP_OK) {
                    ESP_LOGW(TAG, "Fallo al transcribir audio");
                    consecutive_silence++;
                    if (consecutive_silence >= 2) {
                        in_conversation = false;
                    }
                    continue;
                }

                const char *user_text = transcription_get_last_text();

                /* 5. Comprobar si hubo silencio */
                if (is_silent_or_empty(user_text)) {
                    if (turn_number == 0) {
                        /* Si el usuario dijo 'Hola Bombón' y espero confirmacion antes de preguntar */
                        display_show_speaking();
                        rgb_led_set_color(0, 180, 255);
                        tts_speak("¡Hola! Te escucho.");
                        consecutive_silence = 0; /* Reset para que no cuente como silencio fallido */
                        turn_number++;
                        continue;
                    }

                    consecutive_silence++;
                    ESP_LOGI(TAG, "Silencio detectado (consecutivos: %d)", consecutive_silence);

                    if (consecutive_silence >= 2) {
                        ESP_LOGI(TAG, "Fin de conversacion por inactividad");
                        in_conversation = false;
                    }
                    continue;
                }

                /* El usuario hablo: reseteamos contador de silencios */
                consecutive_silence = 0;
                turn_number++;

                /* 6. Consulta al LLM con memoria conversacional multi-turno */
                result = llm_answer(user_text);
                if (result != ESP_OK) {
                    ESP_LOGE(TAG, "No se pudo obtener respuesta del LLM: %s", esp_err_to_name(result));
                    continue;
                }

                /* 7. Síntesis y reproduccion de voz (Kokoro) */
                display_show_speaking();
                rgb_led_set_color(0, 180, 255);
                result = tts_speak(llm_get_last_answer());
                if (result != ESP_OK) {
                    ESP_LOGE(TAG, "Error durante TTS: %s", esp_err_to_name(result));
                }

                /* 8. ¿Fue una frase o intencion de despedida? */
                if (llm_is_farewell()) {
                    ESP_LOGI(TAG, "Despedida confirmada. Cerrando sesion conversacional.");
                    in_conversation = false;
                    break;
                }

                /* Si no fue despedida, el bucle repite inmediatamente a escuchar sin necesitar wake word */
            }

            /* Regresa a reposo y reanuda la escucha de la wake word */
            llm_reset_conversation();
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
