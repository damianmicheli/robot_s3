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

static bool check_button_abort(void)
{
    if (button_was_pressed()) {
        wake_word_toggle_mute();
        return true;
    }
    return false;
}

static bool is_empty_or_silence(const char *text)
{
    if (text == NULL) return true;
    while (*text && (unsigned char)*text <= 32) text++;
    if (*text == '\0') return true;

    /* Ignora anotaciones entre corchetes o parentesis como [musica] */
    if (*text == '[' || *text == '(') {
        const char *end = text + strlen(text) - 1;
        while (end > text && (unsigned char)*end <= 32) end--;
        if (*end == ']' || *end == ')') return true;
    }

    bool has_alnum = false;
    for (const char *p = text; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c > 32 && c != '.' && c != ',' && c != '!' && c != '?' && c != '-' && c != ';' && c != ':') {
            has_alnum = true;
            break;
        }
    }
    return !has_alnum;
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

    /* Inicia la escucha continua de la wake word "Hola Bombon" */
    wake_word_start();

    ESP_LOGI(TAG, "Robot listo. Escuchando 'Hola Bombon' (Boton = Silenciar/Activar microfono).");

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

        /* Deteccion de la palabra de activacion "Hola Bombon" */
        if (!wake_word_is_muted() && wake_word_detected())
        {
            wake_word_clear_detected();
            wake_word_stop();
            ESP_LOGI(TAG, "Activacion por voz: 'Hola Bombon'. Iniciando conversacion fluida.");

            /* 1. Paso 3: Generar y responder el saludo en el momento (no pre-seteado) */
            display_show_thinking();
            rgb_led_set_color(255, 140, 0);

            esp_err_t res = network_connect();
            if (res == ESP_OK) {
                res = llm_generate_greeting();
            }
            if (res == ESP_OK) {
                display_show_speaking();
                rgb_led_set_color(0, 180, 255);
                res = tts_speak(llm_get_last_answer());
            } else {
                ESP_LOGE(TAG, "No se pudo generar el saludo inicial: %s", esp_err_to_name(res));
            }

            /* 2. Ciclo de conversacion fluida:
             *    4. Escucha la respuesta del usuario por 30 segundos
             *    5. El usuario responde
             *    6. El robot vuelve a responder
             *    7. Vuelve al punto 4 hasta frase de despedida o 30 segundos de silencio.
             */
            bool conversation_active = (res == ESP_OK);

            while (conversation_active && !wake_word_is_muted())
            {
                /* 4. Se queda escuchando respuesta por 30 segundos (con deteccion VAD) */
                display_show_recording();
                rgb_led_set_color(255, 0, 0);

                bool heard_speech = false;
                res = microphone_listen_and_record(30, 20, check_button_abort, &heard_speech);

                if (wake_word_is_muted()) {
                    conversation_active = false;
                    break;
                }

                if (res != ESP_OK || !heard_speech) {
                    /* Condicion de salida: silencio por 30 segundos */
                    ESP_LOGI(TAG, "Conversacion finalizada por inactividad (30s en silencio).");
                    conversation_active = false;
                    break;
                }

                /* 5. El usuario respondio: Transcribir el audio grabado */
                display_show_thinking();
                rgb_led_set_color(255, 140, 0);

                res = network_connect();
                if (res == ESP_OK) {
                    res = transcription_send_last_recording();
                }

                if (res != ESP_OK) {
                    ESP_LOGW(TAG, "Error durante la transcripcion: %s", esp_err_to_name(res));
                    continue;
                }

                const char *user_text = transcription_get_last_text();
                if (user_text == NULL || is_empty_or_silence(user_text)) {
                    ESP_LOGI(TAG, "Audio sin palabras reconocibles. Continuando escucha...");
                    continue;
                }

                ESP_LOGI(TAG, "Usuario dijo: \"%s\"", user_text);

                /* 6. Me vuelve a responder con Ollama y TTS */
                bool is_farewell = false;
                res = llm_chat(user_text, &is_farewell);
                if (res == ESP_OK) {
                    display_show_speaking();
                    rgb_led_set_color(0, 180, 255);
                    tts_speak(llm_get_last_answer());
                } else {
                    ESP_LOGE(TAG, "Error en LLM: %s", esp_err_to_name(res));
                }

                /* 7. Condicion de salida: frase de despedida */
                if (is_farewell) {
                    ESP_LOGI(TAG, "Frase de despedida detectada. Finalizando conversacion.");
                    conversation_active = false;
                    break;
                }

                /* Vuelve al punto 4 */
            }

            /* Regresa a reposo y reanuda la escucha continua */
            if (wake_word_is_muted()) {
                rgb_led_off();
            } else {
                rgb_led_set_color(0, 255, 0);
                display_show_standby();
            }
            wake_word_start();
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
