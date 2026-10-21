#include "speaker.h"

#include <stdint.h>
#include <string.h>

#include "driver/i2s_common.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* MAX98357A: BCLK=5, LRC/WS=6 y DIN=7. No necesita MCLK. */
#define SPEAKER_BCLK_GPIO GPIO_NUM_5
#define SPEAKER_WS_GPIO GPIO_NUM_6
#define SPEAKER_DATA_GPIO GPIO_NUM_7

/*
 * Volumen digital con amplificacion y saturacion (clamping).
 * 130% da una ganancia de +2.3 dB sobre el audio original normalizado de Kokoro
 * sin distorsionar la voz. Si se desea mas volumen fisico, conectar el pin GAIN
 * del MAX98357A a GND (o con resistencia segun hoja de datos) y alimentarlo de 5 V.
 */
#define SPEAKER_VOLUME_PERCENT 130

static const char *TAG = "SPEAKER";
static i2s_chan_handle_t s_tx_channel;

typedef struct __attribute__((packed)) {
    char riff[4];
    uint32_t file_size;
    char wave[4];
} riff_header_t;

typedef struct __attribute__((packed)) {
    char chunk_id[4];
    uint32_t chunk_size;
} chunk_header_t;

typedef struct __attribute__((packed)) {
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
} wav_fmt_t;

static inline int16_t scale_and_clamp_sample(int16_t sample, int volume_percent)
{
    int32_t val = ((int32_t)sample * volume_percent) / 100;
    if (val > 32767) return 32767;
    if (val < -32768) return -32768;
    return (int16_t)val;
}

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
            /* Tono claro a 520 Hz audible al encender */
            const int16_t value = (((frame + i) * 520 / sample_rate) & 1) ? -8000 : 8000;
            stereo[i * 2] = value;
            stereo[i * 2 + 1] = value;
        }
        err = i2s_write_all(stereo, block_frames * 2 * sizeof(*stereo));
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    i2s_channel_disable(s_tx_channel);
    heap_caps_free(stereo);
    return err;
}

/*
 * Lee datos desde esp_http_client con reintentos y tiempo de espera amplio
 * para tolerar la generacion por lotes de Kokoro en el servidor local.
 */
static int http_read_wait(esp_http_client_handle_t client, void *buffer, size_t size, int timeout_ms)
{
    const int step_ms = 10;
    for (int waited = 0; waited < timeout_ms; waited += step_ms) {
        const int read = esp_http_client_read(client, buffer, size);
        if (read != 0) return read;
        if (esp_http_client_is_complete_data_received(client)) return 0;
        vTaskDelay(pdMS_TO_TICKS(step_ms));
    }
    ESP_LOGE(TAG, "Timeout esperando datos WAV (%d ms)", timeout_ms);
    return -1;
}

static esp_err_t http_read_exact(esp_http_client_handle_t client, void *buffer, size_t size, int timeout_ms)
{
    uint8_t *data = (uint8_t *)buffer;
    while (size > 0) {
        const int read = http_read_wait(client, data, size, timeout_ms);
        if (read <= 0) return ESP_FAIL;
        data += read;
        size -= read;
    }
    return ESP_OK;
}

static esp_err_t http_skip_bytes(esp_http_client_handle_t client, size_t bytes_to_skip, int timeout_ms)
{
    uint8_t dummy[256];
    while (bytes_to_skip > 0) {
        size_t wanted = bytes_to_skip < sizeof(dummy) ? bytes_to_skip : sizeof(dummy);
        if (http_read_exact(client, dummy, wanted, timeout_ms) != ESP_OK) {
            return ESP_FAIL;
        }
        bytes_to_skip -= wanted;
    }
    return ESP_OK;
}

/*
 * Reproduce audio WAV streaming por HTTP de forma robusta:
 * - Parsea correctamente chunks WAV estandar y extendidos (ignora LIST, JUNK, etc.).
 * - Admite mono y estereo a cualquier sample rate (16 kHz, 22.05 kHz, 24 kHz, etc.).
 * - Funciona con tamano fijo o chunked streaming (tamano indefinido).
 * - Mantiene alineacion estricta de 16 bits para evitar desfasaje de bytes y ruido blanco.
 * - Aplica boost de volumen digital configurable.
 */
esp_err_t speaker_play_wav_http(esp_http_client_handle_t client)
{
    riff_header_t riff;
    ESP_RETURN_ON_ERROR(http_read_exact(client, &riff, sizeof(riff), 15000), TAG,
                        "No se pudo leer el encabezado RIFF");
    if (memcmp(riff.riff, "RIFF", 4) != 0 || memcmp(riff.wave, "WAVE", 4) != 0) {
        ESP_LOGE(TAG, "Encabezado no es RIFF/WAVE");
        return ESP_ERR_NOT_SUPPORTED;
    }

    wav_fmt_t fmt = {0};
    bool has_fmt = false;
    uint32_t data_size = 0;
    bool found_data = false;

    while (!found_data) {
        chunk_header_t chunk;
        if (http_read_exact(client, &chunk, sizeof(chunk), 15000) != ESP_OK) {
            ESP_LOGE(TAG, "Error buscando chunks en archivo WAV");
            return ESP_FAIL;
        }

        if (memcmp(chunk.chunk_id, "fmt ", 4) == 0) {
            if (chunk.chunk_size < sizeof(fmt)) {
                ESP_LOGE(TAG, "Chunk fmt invalido (%lu bytes)", (unsigned long)chunk.chunk_size);
                return ESP_ERR_NOT_SUPPORTED;
            }
            if (http_read_exact(client, &fmt, sizeof(fmt), 15000) != ESP_OK) {
                return ESP_FAIL;
            }
            has_fmt = true;
            if (chunk.chunk_size > sizeof(fmt)) {
                if (http_skip_bytes(client, chunk.chunk_size - sizeof(fmt), 15000) != ESP_OK) {
                    return ESP_FAIL;
                }
            }
        } else if (memcmp(chunk.chunk_id, "data", 4) == 0) {
            data_size = chunk.chunk_size;
            found_data = true;
            break;
        } else {
            ESP_LOGD(TAG, "Saltando chunk %.4s (%lu bytes)", chunk.chunk_id, (unsigned long)chunk.chunk_size);
            if (http_skip_bytes(client, chunk.chunk_size, 15000) != ESP_OK) {
                return ESP_FAIL;
            }
        }
    }

    if (!has_fmt || fmt.audio_format != 1 || fmt.bits_per_sample != 16 || fmt.sample_rate == 0 ||
        (fmt.num_channels != 1 && fmt.num_channels != 2)) {
        ESP_LOGE(TAG, "Formato WAV incompatible: fmt=%u ch=%u bits=%u rate=%lu",
                 fmt.audio_format, fmt.num_channels, fmt.bits_per_sample, (unsigned long)fmt.sample_rate);
        return ESP_ERR_NOT_SUPPORTED;
    }

    bool is_stream = (data_size == 0 || data_size == 0xFFFFFFFF || data_size > 50000000);
    ESP_LOGI(TAG, "Reproduciendo WAV: %lu Hz, %u ch, %s (%lu bytes)",
             (unsigned long)fmt.sample_rate, fmt.num_channels,
             is_stream ? "streaming" : "fijo", (unsigned long)data_size);

    const size_t mono_buf_size = 4096;
    uint8_t *mono_bytes = heap_caps_malloc(mono_buf_size, MALLOC_CAP_8BIT);
    int16_t *stereo = heap_caps_malloc(mono_buf_size * 2, MALLOC_CAP_8BIT);
    if (mono_bytes == NULL || stereo == NULL) {
        heap_caps_free(mono_bytes);
        heap_caps_free(stereo);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = speaker_start(fmt.sample_rate);
    uint32_t remaining = data_size;
    size_t leftover_bytes = 0;

    while (err == ESP_OK) {
        if (!is_stream && remaining == 0) {
            break;
        }

        size_t wanted = mono_buf_size - leftover_bytes;
        if (!is_stream && wanted > remaining) {
            wanted = remaining;
        }

        int read = http_read_wait(client, mono_bytes + leftover_bytes, wanted, 15000);
        if (read < 0) {
            ESP_LOGE(TAG, "Error o timeout leyendo audio HTTP");
            err = ESP_FAIL;
            break;
        }
        if (read == 0) {
            /* Fin del stream o conexion cerrada limpiamente por el servidor */
            break;
        }

        size_t total_bytes = leftover_bytes + read;
        size_t sample_bytes = total_bytes & ~1; /* Multiplo estricto de 2 bytes */
        leftover_bytes = total_bytes - sample_bytes;

        size_t samples = sample_bytes / sizeof(int16_t);
        const int16_t *pcm = (const int16_t *)mono_bytes;

        if (fmt.num_channels == 1) {
            for (size_t i = 0; i < samples; i++) {
                int16_t val = scale_and_clamp_sample(pcm[i], SPEAKER_VOLUME_PERCENT);
                stereo[i * 2] = val;
                stereo[i * 2 + 1] = val;
            }
            err = i2s_write_all(stereo, samples * 2 * sizeof(int16_t));
        } else {
            size_t frames = samples / 2;
            for (size_t i = 0; i < frames * 2; i++) {
                stereo[i] = scale_and_clamp_sample(pcm[i], SPEAKER_VOLUME_PERCENT);
            }
            err = i2s_write_all(stereo, frames * 2 * sizeof(int16_t));
        }

        if (!is_stream) {
            if (remaining > (uint32_t)read) remaining -= (uint32_t)read;
            else remaining = 0;
        }

        if (leftover_bytes > 0) {
            mono_bytes[0] = mono_bytes[total_bytes - 1];
        }
    }

    /* Permite que el DMA termine de enviar las ultimas muestras antes de apagar I2S */
    vTaskDelay(pdMS_TO_TICKS(150));
    i2s_channel_disable(s_tx_channel);
    heap_caps_free(mono_bytes);
    heap_caps_free(stereo);
    return err;
}
