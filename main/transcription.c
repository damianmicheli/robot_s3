#include "transcription.h"
#include <stdio.h>
#include <string.h>
#include "esp_http_client.h"
#include "esp_log.h"
#include "microphone.h"
#include "private_config.h"

#define BOUNDARY "----robotS3AudioBoundary"
static const char *TAG = "SPEACHES";
static char s_transcribed_text[512];

/* Encabezado WAV PCM mínimo; packed garantiza los 44 bytes exigidos por el formato. */
typedef struct __attribute__((packed)) {
    char riff[4]; uint32_t size; char wave[4]; char fmt[4]; uint32_t fmt_size;
    uint16_t format; uint16_t channels; uint32_t sample_rate; uint32_t byte_rate;
    uint16_t block_align; uint16_t bits; char data[4]; uint32_t data_size;
} wav_header_t;
/* esp_http_client_write puede aceptar solo una parte del bloque; completa el envío. */
static esp_err_t write_all(esp_http_client_handle_t c, const void *p, size_t n)
{
    const char *s = p;
    while (n > 0) {
        const int w = esp_http_client_write(c, s, (int)n);
        if (w <= 0) return ESP_FAIL;
        s += w;
        n -= w;
    }
    return ESP_OK;
}
esp_err_t transcription_send_last_recording(void)
{
    const int16_t *samples;
    size_t count;
    esp_err_t err = microphone_get_last_recording(&samples, &count);
    if (err != ESP_OK) return err;
    const size_t data_size = count * sizeof(*samples);
    const wav_header_t wav = {
        .riff = {'R', 'I', 'F', 'F'}, .size = (uint32_t)(data_size + 36),
        .wave = {'W', 'A', 'V', 'E'}, .fmt = {'f', 'm', 't', ' '}, .fmt_size = 16,
        .format = 1, .channels = 1, .sample_rate = 16000, .byte_rate = 32000,
        .block_align = 2, .bits = 16, .data = {'d', 'a', 't', 'a'},
        .data_size = (uint32_t)data_size,
    };
    const char preamble[] = "--" BOUNDARY "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"recording.wav\"\r\nContent-Type: audio/wav\r\n\r\n";
    const char ending[] = "\r\n--" BOUNDARY "\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n" TRANSCRIPTION_MODEL "\r\n--" BOUNDARY "--\r\n";
    char content_type[80];
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", BOUNDARY);
    const esp_http_client_config_t cfg = {
        .url = TRANSCRIPTION_URL, .method = HTTP_METHOD_POST, .timeout_ms = 30000,
        .buffer_size = 2048, .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_ERR_NO_MEM;
    err = esp_http_client_set_header(client, "Content-Type", content_type);
    size_t total = sizeof(preamble)-1 + sizeof(wav) + data_size + sizeof(ending)-1;
    if (err == ESP_OK) err = esp_http_client_open(client, (int)total);
    if (err == ESP_OK) err = write_all(client, preamble, sizeof(preamble)-1);
    if (err == ESP_OK) err = write_all(client, &wav, sizeof(wav));
    if (err == ESP_OK) err = write_all(client, samples, data_size);
    if (err == ESP_OK) err = write_all(client, ending, sizeof(ending)-1);
    if (err == ESP_OK) esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);
    char response[512] = {0};
    if (err == ESP_OK && esp_http_client_read(client, response, sizeof(response)-1) < 0) err = ESP_FAIL;
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) return err;
    if (status < 200 || status >= 300) { ESP_LOGE(TAG, "HTTP %d: %s", status, response); return ESP_FAIL; }
    /* Speaches responde {"text":"..."}; extraemos solo el contenido para Ollama. */
    const char *start = strstr(response, "\"text\":");
    if (start == NULL) return ESP_FAIL;
    start = strchr(start, ':');
    if (start == NULL) return ESP_FAIL;
    while (*++start == ' ') {}
    if (*start != '"') return ESP_FAIL;
    start++;
    size_t text_length = 0;
    while (*start && *start != '"' && text_length + 1 < sizeof(s_transcribed_text)) {
        /* La transcripción puede escapar comillas o saltos de línea en el JSON. */
        if (*start == '\\' && start[1] != '\0') start++;
        s_transcribed_text[text_length++] = *start++;
    }
    s_transcribed_text[text_length] = '\0';
    ESP_LOGI(TAG, "Transcripcion: %s", s_transcribed_text);
    return ESP_OK;
}

const char *transcription_get_last_text(void)
{
    return s_transcribed_text;
}
