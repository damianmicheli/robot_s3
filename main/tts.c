#include "tts.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "private_config.h"
#include "speaker.h"

#define TTS_REQUEST_SIZE 1400
static const char *TAG = "TTS";

static void json_escape(char *out, size_t capacity, const char *text)
{
    size_t position = 0;
    while (*text && position + 2 < capacity) {
        if (*text == '"' || *text == '\\') out[position++] = '\\';
        else if (*text == '\n' || *text == '\r') { out[position++] = '\\'; out[position++] = 'n'; text++; continue; }
        out[position++] = *text++;
    }
    out[position] = '\0';
}

esp_err_t tts_speak(const char *text)
{
    if (text == NULL || *text == '\0') return ESP_ERR_INVALID_ARG;
    char *escaped = heap_caps_malloc(800, MALLOC_CAP_8BIT);
    char *request = heap_caps_malloc(TTS_REQUEST_SIZE, MALLOC_CAP_8BIT);
    if (escaped == NULL || request == NULL) {
        heap_caps_free(escaped);
        heap_caps_free(request);
        return ESP_ERR_NO_MEM;
    }
    json_escape(escaped, 800, text);
    const int length = snprintf(request, TTS_REQUEST_SIZE,
        "{\"model\":\"" TTS_MODEL "\",\"input\":\"%s\",\"voice\":\"" TTS_VOICE
        "\",\"response_format\":\"wav\"}", escaped);
    heap_caps_free(escaped);
    if (length < 0 || length >= TTS_REQUEST_SIZE) { heap_caps_free(request); return ESP_ERR_INVALID_SIZE; }

    const esp_http_client_config_t config = {
        .url = TTS_URL, .method = HTTP_METHOD_POST, .timeout_ms = 60000,
        .buffer_size = 2048, .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) { heap_caps_free(request); return ESP_ERR_NO_MEM; }
    esp_err_t err = esp_http_client_set_header(client, "Content-Type", "application/json");
    if (err == ESP_OK) err = esp_http_client_open(client, length);
    if (err == ESP_OK && esp_http_client_write(client, request, length) != length) err = ESP_FAIL;
    heap_caps_free(request);
    /* fetch_headers devuelve el largo del audio, no un esp_err_t. */
    if (err == ESP_OK && esp_http_client_fetch_headers(client) < 0) err = ESP_FAIL;
    const int status = esp_http_client_get_status_code(client);
    if (err == ESP_OK && status >= 200 && status < 300) err = speaker_play_wav_http(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (err == ESP_OK && (status < 200 || status >= 300)) err = ESP_FAIL;
    if (err != ESP_OK) ESP_LOGE(TAG, "No se pudo sintetizar/reproducir: HTTP %d, %s", status,
                               esp_err_to_name(err));
    return err;
}
