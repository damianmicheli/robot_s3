#include "llm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "private_config.h"

static const char *TAG = "OLLAMA";

#define ESCAPED_USER_TEXT_SIZE 1024
#define REQUEST_BUFFER_SIZE    2048
#define RESPONSE_BUFFER_SIZE   16384
#define LAST_ANSWER_CAPACITY   2048

static char s_last_answer[LAST_ANSWER_CAPACITY];

/* Escapa el texto del usuario para insertarlo en el JSON de la peticion */
static size_t json_escape_user_text(char *out, size_t capacity, const char *text)
{
    size_t pos = 0;
    for (; *text && pos + 2 < capacity; text++) {
        if (*text == '"' || *text == '\\') {
            out[pos++] = '\\';
            out[pos++] = *text;
        } else if (*text == '\n' || *text == '\r') {
            out[pos++] = ' ';
        } else {
            out[pos++] = *text;
        }
    }
    out[pos] = '\0';
    return pos;
}

/* Extrae el campo "content" del JSON de Ollama decodificando escapes */
static bool extract_answer(const char *json, char *out, size_t out_capacity)
{
    const char *text = strstr(json, "\"content\":");
    if (text == NULL) {
        text = strstr(json, "\"response\":");
        if (text == NULL) return false;
    }
    text = strchr(text, ':');
    if (text == NULL) return false;
    while (*++text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') {}
    if (*text != '"') return false;
    text++; /* Salta las comillas de apertura */

    size_t pos = 0;
    while (*text && pos + 1 < out_capacity) {
        if (*text == '"') {
            /* Comilla de cierre sin escapar: fin del mensaje */
            break;
        }
        if (*text == '\\') {
            text++;
            if (*text == '"') {
                out[pos++] = '"';
                text++;
            } else if (*text == '\\') {
                out[pos++] = '\\';
                text++;
            } else if (*text == 'n' || *text == 'r' || *text == 't') {
                out[pos++] = ' ';
                text++;
            } else if (*text == 'u' && text[1] && text[2] && text[3] && text[4]) {
                /* Secuencia unicode \uXXXX; saltamos el prefijo para no corromper */
                text += 5;
            } else if (*text) {
                out[pos++] = *text++;
            } else {
                break;
            }
            continue;
        }
        out[pos++] = *text++;
    }
    out[pos] = '\0';
    return pos > 0;
}

esp_err_t llm_answer(const char *user_text)
{
    char *escaped = heap_caps_malloc(ESCAPED_USER_TEXT_SIZE, MALLOC_CAP_8BIT);
    char *request = heap_caps_malloc(REQUEST_BUFFER_SIZE, MALLOC_CAP_8BIT);
    char *response = heap_caps_calloc(1, RESPONSE_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (response == NULL) {
        response = heap_caps_calloc(1, 8192, MALLOC_CAP_8BIT);
    }

    if (escaped == NULL || request == NULL || response == NULL) {
        free(escaped);
        free(request);
        free(response);
        return ESP_ERR_NO_MEM;
    }

    json_escape_user_text(escaped, ESCAPED_USER_TEXT_SIZE, user_text);

    const int length = snprintf(request, REQUEST_BUFFER_SIZE,
        "{\"model\":\"" OLLAMA_MODEL "\",\"messages\":["
        "{\"role\":\"system\",\"content\":\"Sos un robot de escritorio amigable. Responde en espanol argentino, de forma natural, breve y conversacional. Maximo 2 o 3 oraciones cortas. No uses listas, asteriscos ni emojis ya que tu respuesta sera sintetizada por voz.\"},"
        "{\"role\":\"user\",\"content\":\"%s\"}"
        "],\"think\":false,\"stream\":false,\"options\":{\"num_ctx\":4096,\"temperature\":0.7}}",
        escaped);

    if (length < 0 || length >= REQUEST_BUFFER_SIZE) {
        free(escaped);
        free(request);
        free(response);
        return ESP_ERR_INVALID_SIZE;
    }

    const esp_http_client_config_t config = {
        .url = OLLAMA_CHAT_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 60000,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
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
    size_t total_read = 0;

    if (err == ESP_OK && status >= 200 && status < 300) {
        const size_t max_read = RESPONSE_BUFFER_SIZE - 1;
        while (total_read < max_read) {
            int read = esp_http_client_read(client, response + total_read, max_read - total_read);
            if (read < 0) {
                err = ESP_FAIL;
                break;
            }
            if (read == 0) {
                if (esp_http_client_is_complete_data_received(client)) break;
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            total_read += read;
        }
        response[total_read] = '\0';
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

    if (!extract_answer(response, s_last_answer, sizeof(s_last_answer))) {
        ESP_LOGE(TAG, "No se encontro el texto en la respuesta (%u bytes)", (unsigned int)total_read);
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
