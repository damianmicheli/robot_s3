#include "llm.h"
#include <stdio.h>
#include <string.h>
#include "esp_http_client.h"
#include "esp_log.h"
#include "private_config.h"

static const char *TAG = "OLLAMA";
static size_t json_escape(char *out, size_t capacity, const char *text)
{
    size_t pos = 0;
    for (; *text && pos + 2 < capacity; text++) {
        if (*text == '"' || *text == '\\') out[pos++] = '\\';
        else if (*text == '\n' || *text == '\r') { out[pos++] = '\\'; out[pos++] = 'n'; continue; }
        out[pos++] = *text;
    }
    out[pos] = '\0';
    return pos;
}

esp_err_t llm_answer(const char *user_text)
{
    char escaped[768];
    char request[1200];
    char response[2048] = {0};
    json_escape(escaped, sizeof(escaped), user_text);
    const int length = snprintf(request, sizeof(request),
        "{\"model\":\"" OLLAMA_MODEL "\",\"messages\":[{\"role\":\"system\",\"content\":\"Sos un robot de escritorio. Responde en espanol argentino, natural, breve y conversacional. Tus respuestas seran reproducidas mediante voz.\"},{\"role\":\"user\",\"content\":\"%s\"}],\"think\":false,\"stream\":false,\"options\":{\"num_ctx\":4096,\"temperature\":0.7}}", escaped);
    if (length < 0 || length >= (int)sizeof(request)) return ESP_ERR_INVALID_SIZE;
    const esp_http_client_config_t config = {.url = OLLAMA_CHAT_URL, .method = HTTP_METHOD_POST, .timeout_ms = 60000, .buffer_size = 2048, .buffer_size_tx = 2048};
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) return ESP_ERR_NO_MEM;
    esp_err_t err = esp_http_client_set_header(client, "Content-Type", "application/json");
    if (err == ESP_OK) err = esp_http_client_open(client, length);
    if (err == ESP_OK && esp_http_client_write(client, request, length) != length) err = ESP_FAIL;
    if (err == ESP_OK) esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);
    int read = 0;
    if (err == ESP_OK && status >= 200 && status < 300) read = esp_http_client_read(client, response, sizeof(response) - 1);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) return err;
    if (status < 200 || status >= 300) { ESP_LOGE(TAG, "HTTP %d", status); return ESP_FAIL; }
    if (read < 0) return ESP_FAIL;
    ESP_LOGI(TAG, "Respuesta: %s", response);
    return ESP_OK;
}
