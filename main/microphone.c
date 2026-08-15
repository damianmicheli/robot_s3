#include "microphone.h"

#include <stdint.h>

#include "driver/i2s_common.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

/* Conexiones INMP441: SCK=16, WS=17, SD=18 y L/R conectado a GND (canal izquierdo). */
#define INMP441_BCLK_GPIO      GPIO_NUM_16
#define INMP441_WS_GPIO        GPIO_NUM_17
#define INMP441_DATA_GPIO      GPIO_NUM_18
#define INMP441_SAMPLE_RATE_HZ 16000
#define INMP441_RECORD_SECONDS 5
#define INMP441_CHANNELS       2
#define INMP441_BUFFER_WORDS   256

static const char *TAG = "MIC";
static i2s_chan_handle_t s_rx_channel;
static int16_t *s_recording;
static size_t s_recording_samples;

typedef struct {
    uint32_t samples;
    uint32_t peak;
    uint64_t sum_absolute;
} audio_level_t;

static void add_sample_to_level(audio_level_t *level, int32_t raw_sample)
{
    /* INMP441: 24 bits útiles alineados arriba; se reduce a PCM firmado de 16 bits. */
    const int32_t pcm16 = raw_sample >> 16;
    const uint32_t magnitude = pcm16 < 0 ? (uint32_t)(-(int64_t)pcm16) : (uint32_t)pcm16;

    if (magnitude > level->peak) level->peak = magnitude;
    level->sum_absolute += magnitude;
    level->samples++;
}

static uint32_t mean_level(const audio_level_t *level)
{
    return level->samples == 0 ? 0 : (uint32_t)(level->sum_absolute / level->samples);
}

esp_err_t microphone_init(void)
{
    s_recording = heap_caps_malloc(INMP441_SAMPLE_RATE_HZ * INMP441_RECORD_SECONDS * sizeof(*s_recording), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_recording == NULL) return ESP_ERR_NO_MEM;
    const i2s_chan_config_t channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    const i2s_std_config_t i2s_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(INMP441_SAMPLE_RATE_HZ),
        /* El INMP441 entrega audio I2S Philips de 24 bits, en slots de 32 bits. */
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

esp_err_t microphone_record_five_seconds(void)
{
    int32_t raw_samples[INMP441_BUFFER_WORDS];
    const uint32_t target_samples = INMP441_SAMPLE_RATE_HZ * INMP441_RECORD_SECONDS;
    audio_level_t left_level = {0};
    audio_level_t right_level = {0};
    audio_level_t second_left_level = {0};
    audio_level_t second_right_level = {0};
    uint32_t frame_count = 0;
    uint32_t seconds_reported = 0;

    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx_channel), TAG, "No se pudo iniciar I2S");

    while (frame_count < target_samples) {
        size_t bytes_read = 0;
        esp_err_t result = i2s_channel_read(s_rx_channel, raw_samples, sizeof(raw_samples),
                                            &bytes_read, 1000);
        if (result != ESP_OK) {
            i2s_channel_disable(s_rx_channel);
            return result;
        }

        /*
         * Cada frame contiene los slots izquierdo y derecho. Medimos ambos durante
         * esta primera prueba para confirmar en cuál está transmitiendo el módulo.
         */
        const size_t words_read = bytes_read / sizeof(raw_samples[0]);
        for (size_t i = 0; i + 1 < words_read && frame_count < target_samples;
             i += INMP441_CHANNELS) {
            add_sample_to_level(&left_level, raw_samples[i]);
            add_sample_to_level(&right_level, raw_samples[i + 1]);
            add_sample_to_level(&second_left_level, raw_samples[i]);
            add_sample_to_level(&second_right_level, raw_samples[i + 1]);
            s_recording[frame_count++] = (int16_t)(raw_samples[i] >> 16);

            if (second_left_level.samples == INMP441_SAMPLE_RATE_HZ) {
                seconds_reported++;
                ESP_LOGI(TAG, "%lus: izq medio=%lu pico=%lu | der medio=%lu pico=%lu",
                         (unsigned long)seconds_reported,
                         (unsigned long)mean_level(&second_left_level),
                         (unsigned long)second_left_level.peak,
                         (unsigned long)mean_level(&second_right_level),
                         (unsigned long)second_right_level.peak);
                second_left_level = (audio_level_t){0};
                second_right_level = (audio_level_t){0};
            }
        }
    }

    ESP_RETURN_ON_ERROR(i2s_channel_disable(s_rx_channel), TAG, "No se pudo detener I2S");
    s_recording_samples = frame_count;
    ESP_LOGI(TAG, "Grabacion terminada: %lu frames | izq medio=%lu pico=%lu | der medio=%lu pico=%lu",
             (unsigned long)frame_count, (unsigned long)mean_level(&left_level),
             (unsigned long)left_level.peak, (unsigned long)mean_level(&right_level),
             (unsigned long)right_level.peak);
    return ESP_OK;
}

esp_err_t microphone_get_last_recording(const int16_t **samples, size_t *sample_count)
{
    if (samples == NULL || sample_count == NULL || s_recording_samples == 0) return ESP_ERR_INVALID_STATE;
    *samples = s_recording;
    *sample_count = s_recording_samples;
    return ESP_OK;
}
