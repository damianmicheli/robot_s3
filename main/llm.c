#include "llm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "private_config.h"

static const char *TAG = "OLLAMA";

#define ESCAPED_USER_TEXT_SIZE 1024
#define REQUEST_BUFFER_SIZE    8192
#define RESPONSE_BUFFER_SIZE   16384
#define LAST_ANSWER_CAPACITY   2048

#define MAX_CONV_TURNS         12
#define MAX_TURN_TEXT_LEN      512

typedef struct {
    char role[12];
    char text[MAX_TURN_TEXT_LEN];
} conv_turn_t;

static conv_turn_t *s_history = NULL;
static size_t s_history_count = 0;
static bool s_last_was_farewell = false;
static char s_last_answer[LAST_ANSWER_CAPACITY];

/* Escribe el buffer completo en el cliente HTTP incluso si se envía en varios fragmentos */
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

/* Escapa el texto del usuario para insertarlo de forma segura en el JSON */
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

/* Extrae el campo "content" del JSON de Ollama decodificando caracteres escapados */
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
    text++; /* Salta comilla de apertura */

    size_t pos = 0;
    while (*text && pos + 1 < out_capacity) {
        if (*text == '"') {
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

/* Elimina una etiqueta específica del texto y remueve espacios finales */
static void strip_tag(char *str, const char *tag)
{
    char *p = strstr(str, tag);
    if (p != NULL) {
        size_t tag_len = strlen(tag);
        memmove(p, p + tag_len, strlen(p + tag_len) + 1);

        size_t len = strlen(str);
        while (len > 0 && (str[len - 1] == ' ' || str[len - 1] == '\t' || str[len - 1] == '\r' || str[len - 1] == '\n')) {
            str[--len] = '\0';
        }
    }
}

/* Detecta expresiones claras de despedida en español */
static bool text_contains_farewell(const char *text)
{
    if (text == NULL) return false;

    char lower[256];
    size_t i = 0;
    for (; text[i] && i + 1 < sizeof(lower); i++) {
        unsigned char c = (unsigned char)text[i];
        if (c >= 'A' && c <= 'Z') {
            lower[i] = (char)(c + ('a' - 'A'));
        } else {
            lower[i] = (char)c;
        }
    }
    lower[i] = '\0';

    static const char *const farewell_words[] = {
        "chau",
        "adios",
        "adiós",
        "hasta luego",
        "hasta pronto",
        "hasta mañana",
        "hasta manana",
        "nos vemos",
        "despues hablamos",
        "después hablamos",
        "hablamos despues",
        "hablamos después",
        "eso es todo",
        "buenas noches",
        "bye",
    };

    for (size_t f = 0; f < sizeof(farewell_words) / sizeof(farewell_words[0]); f++) {
        if (strstr(lower, farewell_words[f]) != NULL) {
            return true;
        }
    }
    return false;
}

void llm_reset_conversation(void)
{
    s_history_count = 0;
    s_last_was_farewell = false;
    ESP_LOGI(TAG, "Historial de conversacion reiniciado.");
}

bool llm_is_farewell(void)
{
    return s_last_was_farewell;
}

esp_err_t llm_answer(const char *user_text)
{
    if (s_history == NULL) {
        s_history = heap_caps_calloc(MAX_CONV_TURNS, sizeof(conv_turn_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_history == NULL) {
            ESP_LOGE(TAG, "No se pudo asignar memoria para historial");
            return ESP_ERR_NO_MEM;
        }
    }

    /* Mantenemos una ventana deslizante si se supera el maximo de turnos */
    if (s_history_count >= MAX_CONV_TURNS) {
        memmove(&s_history[0], &s_history[2], (s_history_count - 2) * sizeof(conv_turn_t));
        s_history_count -= 2;
    }

    /* Registramos el turno del usuario */
    strncpy(s_history[s_history_count].role, "user", sizeof(s_history[s_history_count].role) - 1);
    strncpy(s_history[s_history_count].text, user_text, sizeof(s_history[s_history_count].text) - 1);
    s_history[s_history_count].text[sizeof(s_history[s_history_count].text) - 1] = '\0';
    s_history_count++;

    char *request = heap_caps_malloc(REQUEST_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *response = heap_caps_calloc(1, RESPONSE_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *turn_escaped = heap_caps_malloc(ESCAPED_USER_TEXT_SIZE, MALLOC_CAP_8BIT);

    if (request == NULL || response == NULL || turn_escaped == NULL) {
        free(request);
        free(response);
        free(turn_escaped);
        return ESP_ERR_NO_MEM;
    }

    /* Construimos el JSON incluyendo el prompt de sistema y todo el historial de la conversacion */
    int offset = snprintf(request, REQUEST_BUFFER_SIZE,
        "{\"model\":\"" OLLAMA_MODEL "\",\"messages\":["
        "{\"role\":\"system\",\"content\":\""
        "Sos Bomb\xc3\xb3n de Las Chicas Superpoderosas (un robot de escritorio amigable). "
        "Responde en espa\xc3\xb1ol argentino, de forma natural, dulce, breve y conversacional (m\xc3\xa1ximo 1 o 2 oraciones cortas). "
        "No uses listas, asteriscos ni emojis ya que tu respuesta ser\xc3\xa1 sintetizada por voz. "
        "Si el usuario se est\xc3\xa1 despidiendo o dando por finalizada la conversaci\xc3\xb3n "
        "(por ejemplo diciendo chau, hasta luego, despu\xc3\xa9s hablamos, nos vemos, o cualquier otra despedida), "
        "despidite con cari\xc3\xb1o e inclu\xc3\xad la etiqueta [DESPEDIDA] al final de tu respuesta."
        "\"}");

    for (size_t i = 0; i < s_history_count && offset < REQUEST_BUFFER_SIZE - 200; i++) {
        json_escape_user_text(turn_escaped, ESCAPED_USER_TEXT_SIZE, s_history[i].text);
        offset += snprintf(request + offset, REQUEST_BUFFER_SIZE - offset,
            ",{\"role\":\"%s\",\"content\":\"%s\"}",
            s_history[i].role, turn_escaped);
    }

    offset += snprintf(request + offset, REQUEST_BUFFER_SIZE - offset,
        "],\"think\":false,\"stream\":false,\"options\":{\"num_ctx\":4096,\"temperature\":0.7}}");

    free(turn_escaped);

    if (offset < 0 || offset >= REQUEST_BUFFER_SIZE) {
        free(request);
        free(response);
        return ESP_ERR_INVALID_SIZE;
    }

    const esp_http_client_config_t config = {
        .url = OLLAMA_CHAT_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 60000,
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        free(request);
        free(response);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_set_header(client, "Content-Type", "application/json");
    if (err == ESP_OK) err = esp_http_client_open(client, offset);
    if (err == ESP_OK) err = write_all(client, request, offset);
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

    s_last_answer[0] = '\0';
    if (!extract_answer(response, s_last_answer, sizeof(s_last_answer))) {
        ESP_LOGE(TAG, "No se encontro el texto en la respuesta (%u bytes): %s",
                 (unsigned int)total_read, response);
        free(response);
        return ESP_FAIL;
    }
    free(response);

    /* Verificamos si se detecta despedida */
    s_last_was_farewell = false;
    if (strstr(s_last_answer, "[DESPEDIDA]") != NULL ||
        strstr(s_last_answer, "[despedida]") != NULL ||
        strstr(s_last_answer, "[FIN]") != NULL ||
        strstr(s_last_answer, "[fin]") != NULL) {
        s_last_was_farewell = true;
        strip_tag(s_last_answer, "[DESPEDIDA]");
        strip_tag(s_last_answer, "[despedida]");
        strip_tag(s_last_answer, "[FIN]");
        strip_tag(s_last_answer, "[fin]");
    }

    if (text_contains_farewell(user_text)) {
        s_last_was_farewell = true;
    }

    ESP_LOGI(TAG, "Respuesta Bombon: %s %s", s_last_answer,
             s_last_was_farewell ? "(Despedida)" : "(Conversacion activa)");

    /* Guardamos el turno del asistente en el historial para mantener la continuidad conversacional */
    if (s_history_count < MAX_CONV_TURNS) {
        strncpy(s_history[s_history_count].role, "assistant", sizeof(s_history[s_history_count].role) - 1);
        strncpy(s_history[s_history_count].text, s_last_answer, sizeof(s_history[s_history_count].text) - 1);
        s_history[s_history_count].text[sizeof(s_history[s_history_count].text) - 1] = '\0';
        s_history_count++;
    }

    return ESP_OK;
}

const char *llm_get_last_answer(void)
{
    return s_last_answer;
}
