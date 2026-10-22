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
 * Volumen digital limpio para evitar distorsion por recorte (clipping) o
 * sobre-excursion en parlantes pequenos tipo capsula.
 */
#define SPEAKER_VOLUME_PERCENT 65

/*
 * Ajuste de tono (+28% pitch): reproduce el audio nativo en espanol a 30.7 kHz
 * en vez de 24 kHz. Al combinarse con generacion a velocidad 0.78 en Kokoro,
 * la velocidad resultante es 1.0 (natural), pero la voz femenina suena notablemente
 * mas aguda, juvenil y tierna (tono autentico de Bombon).
 */
#define SPEAKER_PITCH_PERCENT 128

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
    i2s_chan_config_t channel_config =
        I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    /* Descriptores DMA ampliados para amortiguar cualquier fluctuacion (256 ms de buffer) */
    channel_config.dma_desc_num = 12;
    channel_config.dma_frame_num = 512;

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
            /* Tono suave a 520 Hz audible al encender sin sobrecargar el parlante */
            const int16_t value = (((frame + i) * 520 / sample_rate) & 1) ? -3500 : 3500;
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
 * Reproduce audio WAV descargandolo primero a la memoria PSRAM del ESP32-S3:
 * - Evita por completo el audio entrecortado por latencia/micro-cortes de red Wi-Fi.
 * - Elimina los chasquidos y la distorsion de underrun del DMA I2S.
 * - Escala el volumen al 65% para garantizar un rango dinámico limpio en el MAX98357A.
 */
esp_err_t speaker_play_wav_http(esp_http_client_handle_t client)
{
    int content_length = esp_http_client_get_content_length(client);
    size_t alloc_size = (content_length > 0 && content_length < 4 * 1024 * 1024) ?
                        (size_t)content_length : (1024 * 1024);

    /* Buffer en PSRAM para precargar el audio */
    uint8_t *wav_buf = heap_caps_malloc(alloc_size, MALLOC_CAP_SPIRAM);
    if (wav_buf == NULL) {
        ESP_LOGE(TAG, "No hay memoria PSRAM para buffer de audio (%u bytes)", (unsigned int)alloc_size);
        return ESP_ERR_NO_MEM;
    }

    /* Descarga el audio a maxima velocidad por Wi-Fi hacia la PSRAM */
    size_t total_read = 0;
    while (total_read < alloc_size) {
        size_t wanted = alloc_size - total_read;
        if (wanted > 16384) wanted = 16384;
        int r = esp_http_client_read(client, (char *)wav_buf + total_read, wanted);
        if (r < 0) {
            ESP_LOGE(TAG, "Error descargando stream WAV");
            heap_caps_free(wav_buf);
            return ESP_FAIL;
        }
        if (r == 0) {
            break; /* Fin de descarga */
        }
        total_read += r;
        if (content_length > 0 && total_read >= (size_t)content_length) {
            break;
        }
    }

    ESP_LOGI(TAG, "Audio WAV precargado en PSRAM (%u bytes)", (unsigned int)total_read);

    /* Parsea el encabezado WAV en memoria */
    if (total_read < sizeof(riff_header_t) + sizeof(chunk_header_t) + sizeof(wav_fmt_t)) {
        ESP_LOGE(TAG, "Archivo WAV demasiado pequeno");
        heap_caps_free(wav_buf);
        return ESP_ERR_INVALID_SIZE;
    }

    riff_header_t *riff = (riff_header_t *)wav_buf;
    if (memcmp(riff->riff, "RIFF", 4) != 0 || memcmp(riff->wave, "WAVE", 4) != 0) {
        ESP_LOGE(TAG, "Formato no es RIFF/WAVE");
        heap_caps_free(wav_buf);
        return ESP_ERR_NOT_SUPPORTED;
    }

    wav_fmt_t fmt = {0};
    bool has_fmt = false;
    const uint8_t *pcm_data = NULL;
    uint32_t pcm_bytes = 0;

    size_t offset = sizeof(riff_header_t);
    while (offset + sizeof(chunk_header_t) <= total_read) {
        chunk_header_t *chunk = (chunk_header_t *)(wav_buf + offset);
        offset += sizeof(chunk_header_t);

        if (memcmp(chunk->chunk_id, "fmt ", 4) == 0) {
            if (chunk->chunk_size >= sizeof(fmt)) {
                memcpy(&fmt, wav_buf + offset, sizeof(fmt));
                has_fmt = true;
            }
            offset += chunk->chunk_size;
        } else if (memcmp(chunk->chunk_id, "data", 4) == 0) {
            pcm_data = wav_buf + offset;
            pcm_bytes = chunk->chunk_size;
            if (offset + pcm_bytes > total_read) {
                pcm_bytes = total_read - offset;
            }
            break;
        } else {
            offset += chunk->chunk_size;
        }
    }

    if (!has_fmt || pcm_data == NULL || pcm_bytes == 0 ||
        fmt.audio_format != 1 || fmt.bits_per_sample != 16 || fmt.sample_rate == 0 ||
        (fmt.num_channels != 1 && fmt.num_channels != 2)) {
        ESP_LOGE(TAG, "WAV incompatible o incompleto: fmt=%u ch=%u bits=%u rate=%lu pcm=%lu",
                 fmt.audio_format, fmt.num_channels, fmt.bits_per_sample,
                 (unsigned long)fmt.sample_rate, (unsigned long)pcm_bytes);
        heap_caps_free(wav_buf);
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint32_t playback_rate = (fmt.sample_rate * SPEAKER_PITCH_PERCENT) / 100;
    ESP_LOGI(TAG, "Reproduciendo desde PSRAM: %lu Hz (pitch agudo %lu Hz), %u ch, %lu bytes",
             (unsigned long)fmt.sample_rate, (unsigned long)playback_rate,
             fmt.num_channels, (unsigned long)pcm_bytes);

    /* Buffer temporal para conversion a estereo y volumen digital */
    const size_t chunk_samples = 1024;
    int16_t *stereo = heap_caps_malloc(chunk_samples * 2 * sizeof(int16_t), MALLOC_CAP_8BIT);
    if (stereo == NULL) {
        heap_caps_free(wav_buf);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = speaker_start(playback_rate);
    const int16_t *pcm16 = (const int16_t *)pcm_data;
    size_t total_samples = (pcm_bytes / sizeof(int16_t));

    if (fmt.num_channels == 1) {
        size_t sample_idx = 0;
        while (err == ESP_OK && sample_idx < total_samples) {
            size_t block = total_samples - sample_idx;
            if (block > chunk_samples) block = chunk_samples;

            for (size_t i = 0; i < block; i++) {
                int16_t val = scale_and_clamp_sample(pcm16[sample_idx + i], SPEAKER_VOLUME_PERCENT);
                stereo[i * 2] = val;
                stereo[i * 2 + 1] = val;
            }
            err = i2s_write_all(stereo, block * 2 * sizeof(int16_t));
            sample_idx += block;
        }
    } else {
        size_t frame_idx = 0;
        size_t total_frames = total_samples / 2;
        while (err == ESP_OK && frame_idx < total_frames) {
            size_t block = total_frames - frame_idx;
            if (block > chunk_samples) block = chunk_samples;

            for (size_t i = 0; i < block * 2; i++) {
                stereo[i] = scale_and_clamp_sample(pcm16[frame_idx * 2 + i], SPEAKER_VOLUME_PERCENT);
            }
            err = i2s_write_all(stereo, block * 2 * sizeof(int16_t));
            frame_idx += block;
        }
    }

    /* Permite que el DMA termine de enviar las ultimas muestras antes de apagar I2S */
    vTaskDelay(pdMS_TO_TICKS(150));
    i2s_channel_disable(s_tx_channel);
    heap_caps_free(stereo);
    heap_caps_free(wav_buf);
    return err;
}
