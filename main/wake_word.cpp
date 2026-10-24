#include "wake_word.h"

#include <string>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "hola_bombon_model.h"
#include "microphone.h"

#include "esphome/components/microphone/microphone.h"
#include "esphome/components/micro_wake_word/micro_wake_word.h"

static const char *TAG = "WAKE_WORD";

class RobotMicrophone : public esphome::microphone::Microphone {
public:
    void start() override {
        microphone_start_stream();
        this->state_ = esphome::microphone::STATE_RUNNING;
    }

    void stop() override {
        microphone_stop_stream();
        this->state_ = esphome::microphone::STATE_STOPPED;
    }

    size_t read(int16_t *buf, size_t len_bytes) override {
        size_t samples_wanted = len_bytes / sizeof(int16_t);
        size_t samples_read = microphone_read_pcm(buf, samples_wanted);
        return samples_read * sizeof(int16_t);
    }
};

static esphome::micro_wake_word::MicroWakeWord s_wake_word;
static RobotMicrophone s_robot_mic;
static volatile bool s_detected = false;
static volatile bool s_listening = false;
static volatile bool s_muted = false;
static TaskHandle_t s_ww_task_handle = nullptr;

static void on_wake_word_detected(std::string name) {
    ESP_LOGI(TAG, "¡Palabra de activacion detectada: %s!", name.c_str());
    s_detected = true;
}

static void wake_word_task(void *arg) {
    ESP_LOGI(TAG, "Tarea de escucha continua iniciada en Core 1");
    while (1) {
        if (s_muted || !s_listening) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        s_wake_word.loop();

        if (s_detected) {
            s_listening = false;
            s_wake_word.stop();
        }

        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

esp_err_t wake_word_init(void) {
    s_wake_word.set_microphone(&s_robot_mic);

    /*
     * Parametros para MicroWakeWord:
     * - Modelo TFLite en Flash
     * - Umbral de probabilidad: 0.80f (80%)
     * - Ventana deslizante para promedio: 5 pasos
     * - Nombre: "Hola Bombon"
     * - Arena de tensores: 64 KB (asignada en PSRAM)
     */
    s_wake_word.add_wake_word_model(
        hola_bombon_tflite,
        0.80f,
        5,
        "Hola Bombon",
        64 * 1024
    );
    s_wake_word.set_features_step_size(10);
    s_wake_word.add_detection_callback(on_wake_word_detected);

    s_wake_word.setup();

    BaseType_t res = xTaskCreatePinnedToCore(
        wake_word_task,
        "ww_task",
        8192,
        nullptr,
        3,
        &s_ww_task_handle,
        1
    );

    if (res != pdPASS) {
        ESP_LOGE(TAG, "No se pudo crear la tarea wake_word_task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Motor microWakeWord inicializado con modelo 'Hola Bombon'");
    return ESP_OK;
}

void wake_word_start(void) {
    if (s_muted) return;
    s_detected = false;
    s_listening = true;
    s_wake_word.start();
    ESP_LOGI(TAG, "Escucha continua activada para 'Hola Bombon'");
}

void wake_word_stop(void) {
    s_listening = false;
    s_wake_word.stop();
    ESP_LOGI(TAG, "Escucha continua pausada");
}

bool wake_word_detected(void) {
    return s_detected;
}

void wake_word_clear_detected(void) {
    s_detected = false;
}

void wake_word_toggle_mute(void) {
    s_muted = !s_muted;
    if (s_muted) {
        wake_word_stop();
        ESP_LOGW(TAG, "Microfono SILENCIADO (MUTE). El robot no escuchara.");
    } else {
        ESP_LOGI(TAG, "Microfono ACTIVADO (UNMUTE). Escuchando 'Hola Bombon'...");
        wake_word_start();
    }
}

void wake_word_set_mute(bool mute) {
    if (s_muted == mute) return;
    wake_word_toggle_mute();
}

bool wake_word_is_muted(void) {
    return s_muted;
}
