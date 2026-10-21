#include "llm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "private_config.h"

static const char *TAG = "OLLAMA";
#define ESCAPED_TEXT_SIZE 768
#define REQUEST_SIZE      1200
#define RESPONSE_SIZE     2048
static char s_last_answer[768];

/* Escapa la transcripción para incluirla sin romper el JSON de la petición. */
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

/* Ollama responde {"message":{"content":"..."}}; extraemos el texto de voz. */
static bool extract_answer(const char *json)
{
    const char *text = strstr(json, "\"content\":");
    if (text == NULL || (text = strchr(text, ':')) == NULL) return false;
    while (*++text == ' ') {}
    if (*text++ != '"') return false;
    size_t position = 0;
    while (*text && *text != '"' && position + 1 < sizeof(s_last_answer)) {
        if (*text == '\\' && text[1]) text++;
        s_last_answer[position++] = *text++;
    }
    s_last_answer[position] = '\0';
    return position > 0;
}

esp_err_t llm_answer(const char *user_text)
{
    /*
     * La tarea principal tiene una pila pequeña. Los buffers HTTP suman más de
     * 4 KB, por eso van al heap: evita corromper tareas internas de FreeRTOS.
     */
    char *escaped = heap_caps_malloc(ESCAPED_TEXT_SIZE, MALLOC_CAP_8BIT);
    char *request = heap_caps_malloc(REQUEST_SIZE, MALLOC_CAP_8BIT);
    char *response = heap_caps_calloc(1, RESPONSE_SIZE, MALLOC_CAP_8BIT);
    if (escaped == NULL || request == NULL || response == NULL) {
        free(escaped);
        free(request);
        free(response);
        return ESP_ERR_NO_MEM;
    }

    json_escape(escaped, ESCAPED_TEXT_SIZE, user_text);
    /* stream=false entrega un único JSON, ideal hasta que se agregue TTS. */
    const int length = snprintf(request, REQUEST_SIZE,
        "{\"model\":\"" OLLAMA_MODEL "\",\"messages\":[{\"role\":\"system\",\"content\":\"Sos un robot de escritorio. Responde en espanol argentino, natural, breve y conversacional. Tus respuestas seran reproducidas mediante voz.\"},{\"role\":\"user\",\"content\":\"%s\"}],\"think\":false,\"stream\":false,\"options\":{\"num_ctx\":4096,\"temperature\":0.7}}", escaped);
    if (length < 0 || length >= REQUEST_SIZE) {
        free(escaped);
        free(request);
        free(response);
        return ESP_ERR_INVALID_SIZE;
    }
    const esp_http_client_config_t config = {
        .url = OLLAMA_CHAT_URL, .method = HTTP_METHOD_POST, .timeout_ms = 60000,
        .buffer_size = 2048, .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        free(escaped);
        free(request);
        free(response);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_http_client_set_header(client, "Content-Type", "application/json");
    if (err == ESP_OK) err = esp_http_client_open(client, length);
    if (err == ESP_OK && esp_http_client_write(client, request, length) != length) err = ESP_FAIL;
    if (err == ESP_OK) esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);
    int read = 0;
    if (err == ESP_OK && status >= 200 && status < 300) {
        read = esp_http_client_read(client, response, RESPONSE_SIZE - 1);
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(escaped);
    free(request);
    if (err != ESP_OK) {
        free(response);
        return err;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGE(TAG, "HTTP %d", status);
        free(response);
        return ESP_FAIL;
    }
    if (read < 0) {
        free(response);
        return ESP_FAIL;
    }
    if (!extract_answer(response)) {
        ESP_LOGE(TAG, "No se encontro el texto en la respuesta: %s", response);
        free(response);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Respuesta: %s", s_last_answer);
    free(response);
    return ESP_OK;
}

const char *llm_get_last_answer(void)
{
    return s_last_answer;
}
