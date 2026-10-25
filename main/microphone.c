#include "microphone.h"

#include <stdint.h>
#include <string.h>

#include "driver/i2s_common.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Conexiones INMP441: SCK=16, WS=17, SD=18 y L/R a GND (canal izquierdo). */
#define INMP441_BCLK_GPIO          GPIO_NUM_16
#define INMP441_WS_GPIO            GPIO_NUM_17
#define INMP441_DATA_GPIO          GPIO_NUM_18
#define INMP441_SAMPLE_RATE_HZ     16000
#define INMP441_MAX_RECORD_SECONDS 30
#define INMP441_CHANNELS           2
#define INMP441_BUFFER_WORDS       256

/* Parametros de deteccion de actividad de voz (VAD) */
#define VAD_CHUNK_SAMPLES          800   /* 50 ms a 16 kHz */
#define VAD_PREROLL_CHUNKS         10    /* 10 chunks x 50 ms = 500 ms de pre-roll */
#define VAD_SILENCE_TIMEOUT_MS     1300  /* 1.3 s de silencio para cerrar la frase */

static const char *TAG = "MIC";
static i2s_chan_handle_t s_rx_channel;
static int16_t *s_recording = NULL;
static int16_t *s_preroll = NULL;
static size_t s_recording_samples = 0;
static bool s_stream_enabled = false;

/* Calcula el nivel medio de energia de la senal (desviacion media absoluta sin DC) */
static uint32_t calculate_chunk_energy(const int16_t *buf, size_t count)
{
    if (count == 0) return 0;
    int32_t sum = 0;
    for (size_t i = 0; i < count; i++) {
        sum += buf[i];
    }
    int32_t mean = sum / (int32_t)count;

    uint32_t abs_diff_sum = 0;
    for (size_t i = 0; i < count; i++) {
        int32_t diff = (int32_t)buf[i] - mean;
        abs_diff_sum += (diff < 0) ? -diff : diff;
    }
    return abs_diff_sum / count;
}

esp_err_t microphone_init(void)
{
    /* 30 segundos de PCM 16-bit mono ocupan 960 KB en PSRAM */
    s_recording = heap_caps_malloc(INMP441_SAMPLE_RATE_HZ * INMP441_MAX_RECORD_SECONDS *
                                   sizeof(*s_recording), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_recording == NULL) return ESP_ERR_NO_MEM;

    s_preroll = heap_caps_malloc(VAD_CHUNK_SAMPLES * VAD_PREROLL_CHUNKS *
                                 sizeof(*s_preroll), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_preroll == NULL) {
        heap_caps_free(s_recording);
        s_recording = NULL;
        return ESP_ERR_NO_MEM;
    }

    const i2s_chan_config_t channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    const i2s_std_config_t i2s_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(INMP441_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = INMP441_BCLK_GPIO,
            .ws = INMP441_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = INMP441_DATA_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel_config, NULL, &s_rx_channel), TAG,
                        "No se pudo crear el canal I2S");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx_channel, &i2s_config), TAG,
                        "No se pudo configurar el INMP441");
    return ESP_OK;
}

esp_err_t microphone_start_stream(void)
{
    if (s_stream_enabled) return ESP_OK;
    esp_err_t err = i2s_channel_enable(s_rx_channel);
    if (err == ESP_OK) s_stream_enabled = true;
    return err;
}

esp_err_t microphone_stop_stream(void)
{
    if (!s_stream_enabled) return ESP_OK;
    esp_err_t err = i2s_channel_disable(s_rx_channel);
    if (err == ESP_OK) s_stream_enabled = false;
    return err;
}

void microphone_flush(void)
{
    if (!s_stream_enabled) return;
    int32_t raw_samples[INMP441_BUFFER_WORDS];
    size_t bytes_read = 0;
    while (i2s_channel_read(s_rx_channel, raw_samples, sizeof(raw_samples), &bytes_read, 0) == ESP_OK && bytes_read > 0) {
        /* vaciando descriptores DMA */
    }
}

void microphone_discard_samples(uint32_t ms)
{
    if (!s_stream_enabled) return;
    size_t total_samples_to_discard = (INMP441_SAMPLE_RATE_HZ * ms) / 1000;
    int16_t dummy[VAD_CHUNK_SAMPLES];
    while (total_samples_to_discard > 0) {
        size_t chunk_size = total_samples_to_discard > VAD_CHUNK_SAMPLES ? VAD_CHUNK_SAMPLES : total_samples_to_discard;
        size_t read = microphone_read_pcm(dummy, chunk_size);
        if (read == 0) break;
        total_samples_to_discard -= read;
    }
}

size_t microphone_read_pcm(int16_t *buf, size_t samples_wanted)
{
    if (!s_stream_enabled) {
        if (microphone_start_stream() != ESP_OK) return 0;
    }

    int32_t raw_samples[INMP441_BUFFER_WORDS];
    size_t samples_collected = 0;

    while (samples_collected < samples_wanted) {
        size_t to_read_frames = samples_wanted - samples_collected;
        size_t max_frames = sizeof(raw_samples) / (sizeof(int32_t) * INMP441_CHANNELS);
        if (to_read_frames > max_frames) {
            to_read_frames = max_frames;
        }

        size_t bytes_to_read = to_read_frames * sizeof(int32_t) * INMP441_CHANNELS;
        size_t bytes_read = 0;
        esp_err_t result = i2s_channel_read(s_rx_channel, raw_samples, bytes_to_read,
                                            &bytes_read, 100);
        if (result != ESP_OK || bytes_read == 0) {
            break;
        }

        const size_t words_read = bytes_read / sizeof(raw_samples[0]);
        for (size_t i = 0; i + 1 < words_read && samples_collected < samples_wanted;
             i += INMP441_CHANNELS) {
            buf[samples_collected++] = (int16_t)(raw_samples[i] >> 16);
        }
    }

    return samples_collected;
}

/*
 * Escucha la respuesta del usuario con deteccion inteligente de voz (VAD):
 * - Espera hasta max_wait_sec (ej. 30 s) a que el usuario empiece a hablar.
 * - Mantiene un buffer circular de pre-roll (500 ms) para no recortar la primera silaba.
 * - Una vez iniciada la voz, graba hasta que detecta 1.3 s de silencio o max_speech_sec.
 * - Si pasan max_wait_sec en silencio absoluto, termina y devuelve *out_heard_speech = false.
 */
esp_err_t microphone_listen_and_record(uint32_t max_wait_sec,
                                       uint32_t max_speech_sec,
                                       bool (*abort_check)(void),
                                       bool *out_heard_speech)
{
    if (out_heard_speech == NULL) return ESP_ERR_INVALID_ARG;
    *out_heard_speech = false;
    s_recording_samples = 0;

    bool newly_started = !s_stream_enabled;
    ESP_RETURN_ON_ERROR(microphone_start_stream(), TAG, "No se pudo iniciar I2S");

    if (newly_started) {
        /* Descartar 350 ms de transitorio de encendido / carga de filtro del INMP441 */
        microphone_discard_samples(350);
    } else {
        /* Si el micro ya estaba activo, descartar muestras viejas acumuladas en DMA */
        microphone_flush();
    }

    int preroll_head = 0;
    int preroll_count = 0;
    bool speech_started = false;
    int consecutive_speech_chunks = 0;
    uint32_t consecutive_silence_ms = 0;
    uint32_t elapsed_wait_ms = 0;
    uint32_t speech_duration_ms = 0;
    uint32_t active_speech_ms = 0;
    const uint32_t max_wait_ms = max_wait_sec * 1000;
    const uint32_t max_speech_ms = max_speech_sec * 1000;
    const uint32_t max_total_samples = INMP441_SAMPLE_RATE_HZ * INMP441_MAX_RECORD_SECONDS;

    int16_t chunk[VAD_CHUNK_SAMPLES];

    /* Calibrar ruido ambiente real de la habitacion con 4 chunks (200 ms) */
    uint32_t noise_sum = 0;
    int noise_chunks = 0;
    for (int i = 0; i < 4; i++) {
        if (abort_check && abort_check()) {
            microphone_stop_stream();
            s_recording_samples = 0;
            *out_heard_speech = false;
            return ESP_OK;
        }
        size_t r = microphone_read_pcm(chunk, VAD_CHUNK_SAMPLES);
        if (r == VAD_CHUNK_SAMPLES) {
            noise_sum += calculate_chunk_energy(chunk, VAD_CHUNK_SAMPLES);
            noise_chunks++;
        }
    }
    uint32_t ambient_noise = (noise_chunks > 0) ? (noise_sum / noise_chunks) : 120;
    if (ambient_noise < 80) ambient_noise = 80;
    if (ambient_noise > 800) ambient_noise = 800;

    uint32_t speech_threshold = ambient_noise * 2 + 250;
    if (speech_threshold < 450) speech_threshold = 450;

    while (1) {
        if (abort_check && abort_check()) {
            ESP_LOGI(TAG, "Escucha abortada por usuario");
            microphone_stop_stream();
            s_recording_samples = 0;
            *out_heard_speech = false;
            return ESP_OK;
        }

        size_t read_samples = microphone_read_pcm(chunk, VAD_CHUNK_SAMPLES);
        if (read_samples < VAD_CHUNK_SAMPLES) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        uint32_t energy = calculate_chunk_energy(chunk, VAD_CHUNK_SAMPLES);

        if (!speech_started) {
            elapsed_wait_ms += 50;

            /* Adaptar nivel de ruido de fondo ante cambios graduales */
            if (energy < ambient_noise) {
                ambient_noise = (ambient_noise * 7 + energy) / 8;
            } else if (energy < speech_threshold) {
                ambient_noise = (ambient_noise * 31 + energy) / 32;
            }

            speech_threshold = ambient_noise * 2 + 250;
            if (speech_threshold < 450) speech_threshold = 450;

            /* Guardar en buffer circular de pre-roll */
            memcpy(s_preroll + (preroll_head * VAD_CHUNK_SAMPLES), chunk, sizeof(chunk));
            preroll_head = (preroll_head + 1) % VAD_PREROLL_CHUNKS;
            if (preroll_count < VAD_PREROLL_CHUNKS) preroll_count++;

            if (energy >= speech_threshold) {
                consecutive_speech_chunks++;
                /* Requiere al menos 3 chunks (150 ms) para descartar chasquidos y transitorios breves */
                if (consecutive_speech_chunks >= 3) {
                    speech_started = true;
                    active_speech_ms = consecutive_speech_chunks * 50;
                    ESP_LOGI(TAG, "Voz detectada! (energia=%lu, umbral=%lu, ruido=%lu)",
                             (unsigned long)energy, (unsigned long)speech_threshold, (unsigned long)ambient_noise);

                    /* Volcar el pre-roll en orden cronologico en s_recording */
                    size_t copied = 0;
                    int start_idx = (preroll_count < VAD_PREROLL_CHUNKS) ? 0 : preroll_head;
                    for (int i = 0; i < preroll_count; i++) {
                        int c_idx = (start_idx + i) % VAD_PREROLL_CHUNKS;
                        memcpy(s_recording + copied, s_preroll + (c_idx * VAD_CHUNK_SAMPLES),
                               VAD_CHUNK_SAMPLES * sizeof(int16_t));
                        copied += VAD_CHUNK_SAMPLES;
                    }
                    s_recording_samples = copied;
                    consecutive_silence_ms = 0;
                    speech_duration_ms = 0;
                }
            } else {
                consecutive_speech_chunks = 0;
            }

            if (!speech_started && elapsed_wait_ms >= max_wait_ms) {
                ESP_LOGI(TAG, "Silencio continuo por %lu s. Finalizando escucha.", (unsigned long)max_wait_sec);
                microphone_stop_stream();
                s_recording_samples = 0;
                *out_heard_speech = false;
                return ESP_OK;
            }
        } else {
            speech_duration_ms += 50;

            if (s_recording_samples + VAD_CHUNK_SAMPLES <= max_total_samples) {
                memcpy(s_recording + s_recording_samples, chunk, sizeof(chunk));
                s_recording_samples += VAD_CHUNK_SAMPLES;
            }

            uint32_t speech_threshold_active = ambient_noise * 2 + 200;
            if (speech_threshold_active < 380) speech_threshold_active = 380;

            if (energy >= speech_threshold_active) {
                active_speech_ms += 50;
                consecutive_silence_ms = 0;
            } else {
                consecutive_silence_ms += 50;
            }

            if (consecutive_silence_ms >= VAD_SILENCE_TIMEOUT_MS) {
                /* Si el audio activo total fue menor a 250 ms, se descarta como ruido / chasquido */
                if (active_speech_ms < 250) {
                    ESP_LOGI(TAG, "Ruido breve descartado (%lu ms de audio activo). Continuando escucha...",
                             (unsigned long)active_speech_ms);
                    speech_started = false;
                    s_recording_samples = 0;
                    preroll_count = 0;
                    preroll_head = 0;
                    consecutive_speech_chunks = 0;
                    consecutive_silence_ms = 0;
                    active_speech_ms = 0;
                    speech_duration_ms = 0;
                    continue;
                }

                /* Recortar el exceso de silencio final para optimizar el envio (dejar ~400 ms de cola) */
                uint32_t excess_silence_ms = consecutive_silence_ms > 400 ? consecutive_silence_ms - 400 : 0;
                size_t excess_samples = (excess_silence_ms * INMP441_SAMPLE_RATE_HZ) / 1000;
                if (s_recording_samples > excess_samples) {
                    s_recording_samples -= excess_samples;
                }

                ESP_LOGI(TAG, "Fin de frase detectado (%lu ms total, %lu ms activa, %u muestras grabadas)",
                         (unsigned long)speech_duration_ms, (unsigned long)active_speech_ms, (unsigned int)s_recording_samples);
                *out_heard_speech = true;
                return ESP_OK;
            }

            if (speech_duration_ms >= max_speech_ms) {
                ESP_LOGI(TAG, "Limite de duracion de frase alcanzado (%lu ms)", (unsigned long)speech_duration_ms);
                *out_heard_speech = true;
                return ESP_OK;
            }
        }
    }
}

esp_err_t microphone_record_five_seconds(void)
{
    const uint32_t target_samples = INMP441_SAMPLE_RATE_HZ * 5;
    uint32_t frame_count = 0;

    ESP_RETURN_ON_ERROR(microphone_start_stream(), TAG, "No se pudo iniciar I2S");

    while (frame_count < target_samples) {
        size_t needed = target_samples - frame_count;
        size_t read = microphone_read_pcm(s_recording + frame_count, needed < 512 ? needed : 512);
        if (read == 0) {
            microphone_stop_stream();
            return ESP_FAIL;
        }
        frame_count += read;
    }

    microphone_stop_stream();
    s_recording_samples = frame_count;
    return ESP_OK;
}

esp_err_t microphone_get_last_recording(const int16_t **samples, size_t *sample_count)
{
    if (samples == NULL || sample_count == NULL || s_recording_samples == 0) return ESP_ERR_INVALID_STATE;
    *samples = s_recording;
    *sample_count = s_recording_samples;
    return ESP_OK;
}
