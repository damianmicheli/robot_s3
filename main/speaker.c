#include "speaker.h"

#include <stdint.h>
#include <string.h>

#include "driver/i2s_common.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* MAX98357A: BCLK=5, LRC/WS=6 y DIN=7. No necesita MCLK. */
#define SPEAKER_BCLK_GPIO GPIO_NUM_5
#define SPEAKER_WS_GPIO GPIO_NUM_6
#define SPEAKER_DATA_GPIO GPIO_NUM_7
/* Margen suficiente para un parlante de 3 W sin llevar la señal digital al máximo. */
#define SPEAKER_VOLUME_PERCENT 50

static const char *TAG = "SPEAKER";
static i2s_chan_handle_t s_tx_channel;

typedef struct __attribute__((packed)) {
    char riff[4]; uint32_t size; char wave[4]; char fmt[4]; uint32_t fmt_size;
    uint16_t format; uint16_t channels; uint32_t sample_rate; uint32_t byte_rate;
    uint16_t block_align; uint16_t bits; char data[4]; uint32_t data_size;
} wav_header_t;

static esp_err_t i2s_write_all(const int16_t *samples, size_t byte_count)
{
    const uint8_t *data = (const uint8_t *)samples;
    while (byte_count > 0) {
        size_t written = 0;
        esp_err_t err = i2s_channel_write(s_tx_channel, data, byte_count, &written, 1000);
        if (err != ESP_OK || written == 0) return err == ESP_OK ? ESP_FAIL : err;
        data += written;
        byte_count -= written;
    }
    return ESP_OK;
}

static esp_err_t speaker_start(uint32_t sample_rate)
{
    const i2s_std_clk_config_t clock_config = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    ESP_RETURN_ON_ERROR(i2s_channel_reconfig_std_clock(s_tx_channel, &clock_config), TAG,
                        "No se pudo cambiar la frecuencia I2S");
    return i2s_channel_enable(s_tx_channel);
}

esp_err_t speaker_init(void)
{
    const i2s_chan_config_t channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    const i2s_std_config_t i2s_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(24000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = SPEAKER_BCLK_GPIO,
            .ws = SPEAKER_WS_GPIO,
            .dout = SPEAKER_DATA_GPIO,
            .din = I2S_GPIO_UNUSED,
        },
    };

    ESP_RETURN_ON_ERROR(i2s_new_channel(&channel_config, &s_tx_channel, NULL), TAG,
                        "No se pudo crear la salida I2S");
    return i2s_channel_init_std_mode(s_tx_channel, &i2s_config);
}

esp_err_t speaker_play_startup_tone(void)
{
    enum { sample_rate = 24000, tone_frames = 4800, frames_per_block = 240 };
    int16_t *stereo = heap_caps_malloc(frames_per_block * 2 * sizeof(*stereo), MALLOC_CAP_8BIT);
    if (stereo == NULL) return ESP_ERR_NO_MEM;

    esp_err_t err = speaker_start(sample_rate);
    for (int frame = 0; err == ESP_OK && frame < tone_frames; frame += frames_per_block) {
        const int block_frames = (tone_frames - frame) < frames_per_block ?
                                 (tone_frames - frame) : frames_per_block;
        for (int i = 0; i < block_frames; i++) {
            /* Onda a 440 Hz solo para comprobar el hardware al encender. */
            const int16_t value = (((frame + i) * 440 / sample_rate) & 1) ? -2800 : 2800;
            stereo[i * 2] = value;
            stereo[i * 2 + 1] = value;
        }
        err = i2s_write_all(stereo, block_frames * 2 * sizeof(*stereo));
    }
    i2s_channel_disable(s_tx_channel);
    heap_caps_free(stereo);
    return err;
}

/*
 * Con Wi-Fi, esp_http_client_read puede devolver cero mientras espera el próximo
 * paquete TCP. No es fin de archivo si todavía falta cuerpo: esperamos hasta 5 s.
 */
static int http_read_wait(esp_http_client_handle_t client, void *buffer, size_t size)
{
    for (int waited = 0; waited < 5000; waited += 10) {
        const int read = esp_http_client_read(client, buffer, size);
        if (read != 0) return read;
        if (esp_http_client_is_complete_data_received(client)) return 0;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGE(TAG, "Timeout esperando datos WAV");
    return -1;
}

static esp_err_t http_read_all(esp_http_client_handle_t client, void *buffer, size_t size)
{
    uint8_t *data = buffer;
    while (size > 0) {
        const int read = http_read_wait(client, data, size);
        if (read <= 0) return ESP_FAIL;
        data += read;
        size -= read;
    }
    return ESP_OK;
}

/* Kokoro, al pedir WAV, entrega PCM 16-bit mono. Se reproduce mientras descarga. */
esp_err_t speaker_play_wav_http(esp_http_client_handle_t client)
{
    wav_header_t header;
    ESP_RETURN_ON_ERROR(http_read_all(client, &header, sizeof(header)), TAG,
                        "No se pudo leer el encabezado WAV");
    if (memcmp(header.riff, "RIFF", 4) != 0 || memcmp(header.wave, "WAVE", 4) != 0 ||
        memcmp(header.fmt, "fmt ", 4) != 0 || memcmp(header.data, "data", 4) != 0 ||
        header.format != 1 || header.channels != 1 || header.bits != 16 || header.sample_rate == 0) {
        ESP_LOGE(TAG, "WAV no compatible: formato=%u canales=%u bits=%u", header.format,
                 header.channels, header.bits);
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint8_t *mono_bytes = heap_caps_malloc(1024, MALLOC_CAP_8BIT);
    int16_t *stereo = heap_caps_malloc(1024 * sizeof(*stereo), MALLOC_CAP_8BIT);
    if (mono_bytes == NULL || stereo == NULL) {
        heap_caps_free(mono_bytes);
        heap_caps_free(stereo);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Reproduciendo WAV: %lu Hz, %lu bytes", (unsigned long)header.sample_rate,
             (unsigned long)header.data_size);
    esp_err_t err = speaker_start(header.sample_rate);
    uint32_t remaining = header.data_size;
    while (err == ESP_OK && remaining > 0) {
        const size_t wanted = remaining < 1024 ? remaining : 1024;
        const int read = http_read_wait(client, mono_bytes, wanted);
        if (read <= 0) { err = ESP_FAIL; break; }
        const size_t samples = (size_t)read / sizeof(int16_t);
        const int16_t *mono = (const int16_t *)mono_bytes;
        for (size_t i = 0; i < samples; i++) {
            const int16_t value = (int16_t)((int32_t)mono[i] * SPEAKER_VOLUME_PERCENT / 100);
            stereo[i * 2] = value;
            stereo[i * 2 + 1] = value;
        }
        err = i2s_write_all(stereo, samples * 2 * sizeof(*stereo));
        remaining -= read;
    }
    i2s_channel_disable(s_tx_channel);
    heap_caps_free(mono_bytes);
    heap_caps_free(stereo);
    return err;
}
