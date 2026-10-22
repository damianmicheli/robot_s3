#include "microphone.h"

#include <stdint.h>

#include "driver/i2s_common.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_heap_caps.h"

/* Conexiones INMP441: SCK=16, WS=17, SD=18 y L/R a GND (canal izquierdo). */
#define INMP441_BCLK_GPIO      GPIO_NUM_16
#define INMP441_WS_GPIO        GPIO_NUM_17
#define INMP441_DATA_GPIO      GPIO_NUM_18
#define INMP441_SAMPLE_RATE_HZ 16000
#define INMP441_RECORD_SECONDS 5
#define INMP441_CHANNELS       2
#define INMP441_BUFFER_WORDS   256

static i2s_chan_handle_t s_rx_channel;
static int16_t *s_recording;
static size_t s_recording_samples;
static bool s_stream_enabled = false;

esp_err_t microphone_init(void)
{
    /* Cinco segundos de PCM 16-bit mono ocupan 160 KB en PSRAM */
    s_recording = heap_caps_malloc(INMP441_SAMPLE_RATE_HZ * INMP441_RECORD_SECONDS *
                                   sizeof(*s_recording), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_recording == NULL) return ESP_ERR_NO_MEM;

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

    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel_config, NULL, &s_rx_channel), "MIC",
                        "No se pudo crear el canal I2S");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx_channel, &i2s_config), "MIC",
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

esp_err_t microphone_record_five_seconds(void)
{
    const uint32_t target_samples = INMP441_SAMPLE_RATE_HZ * INMP441_RECORD_SECONDS;
    uint32_t frame_count = 0;

    ESP_RETURN_ON_ERROR(microphone_start_stream(), "MIC", "No se pudo iniciar I2S");

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
