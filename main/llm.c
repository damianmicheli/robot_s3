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

#define MAX_CHAT_MESSAGES      14
#define REQUEST_BUFFER_SIZE    8192
#define RESPONSE_BUFFER_SIZE   16384
#define LAST_ANSWER_CAPACITY   2048

static const char *SYSTEM_PROMPT =
    "Sos Bombón, una robot de escritorio amigable, inteligente y dulce. "
    "Hablas en espanol argentino natural y conversacional. "
    "Respuestas breves de maximo 2 oraciones cortas, directas para sintetizar por voz. "
    "No uses listas, asteriscos ni emojis ya que tu respuesta sera sintetizada por voz. "
    "IMPORTANTE: Si el usuario se despide (diciendo chau, hasta luego, nos vemos, adios, etc.), "
    "responde despidiendote calidamente y agrega exactamente [CHAU] al final de tu respuesta. "
    "Si no se esta despidiendo, NO agregues esa etiqueta.";

typedef struct {
    char role[12];
    char *content;
} chat_msg_t;

static chat_msg_t s_messages[MAX_CHAT_MESSAGES];
static int s_message_count = 0;
static char s_last_answer[LAST_ANSWER_CAPACITY];

void llm_reset_conversation(void)
{
    for (int i = 0; i < s_message_count; i++) {
        if (s_messages[i].content != NULL) {
            free(s_messages[i].content);
            s_messages[i].content = NULL;
        }
    }
    s_message_count = 0;
    s_last_answer[0] = '\0';
}

static void add_message(const char *role, const char *content)
{
    if (content == NULL) return;

    if (s_message_count >= MAX_CHAT_MESSAGES) {
        /* Descartar el par user/assistant mas antiguo (indices 1 y 2), preservando system en 0 */
        if (s_message_count >= 3) {
            free(s_messages[1].content);
            free(s_messages[2].content);
            for (int i = 1; i + 2 < s_message_count; i++) {
                s_messages[i] = s_messages[i + 2];
            }
            s_message_count -= 2;
        }
    }

    strncpy(s_messages[s_message_count].role, role, sizeof(s_messages[s_message_count].role) - 1);
    s_messages[s_message_count].role[sizeof(s_messages[s_message_count].role) - 1] = '\0';
    s_messages[s_message_count].content = strdup(content);
    if (s_messages[s_message_count].content != NULL) {
        s_message_count++;
    }
}

/* Escapa el texto para insertarlo con seguridad en el JSON */
static size_t json_escape_text(char *out, size_t capacity, const char *text)
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
    text++;

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

/* Comprueba si el texto contiene palabras clave comunes de despedida */
static bool contains_farewell_keyword(const char *text)
{
    if (text == NULL) return false;
    char lower[256];
    size_t len = strlen(text);
    if (len >= sizeof(lower)) len = sizeof(lower) - 1;
    for (size_t i = 0; i < len; i++) {
        char c = text[i];
        if (c >= 'A' && c <= 'Z') c += ('a' - 'A');
        lower[i] = c;
    }
    lower[len] = '\0';

    if (strstr(lower, "chau") != NULL ||
        strstr(lower, "chao") != NULL ||
        strstr(lower, "adios") != NULL ||
        strstr(lower, "adiós") != NULL ||
        strstr(lower, "hasta luego") != NULL ||
        strstr(lower, "nos vemos") != NULL ||
        strstr(lower, "hasta mañana") != NULL ||
        strstr(lower, "hasta pronto") != NULL ||
        strstr(lower, "me voy") != NULL ||
        strstr(lower, "bye") != NULL) {
        return true;
    }
    return false;
}

/* Detecta y elimina la etiqueta [CHAU] de la respuesta del LLM */
static bool process_farewell(char *answer, const char *user_input)
{
    bool farewell = false;

    char *tag = strstr(answer, "[CHAU]");
    if (tag != NULL) {
        farewell = true;
        *tag = '\0';
        int len = (int)strlen(answer);
        while (len > 0 && ((unsigned char)answer[len - 1] <= 32)) {
            answer[--len] = '\0';
        }
    }

    if (!farewell && user_input != NULL && contains_farewell_keyword(user_input)) {
        farewell = true;
    }

    return farewell;
}

static esp_err_t perform_chat_request(bool *out_is_farewell, const char *user_input)
{
    char *request = heap_caps_malloc(REQUEST_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *response = heap_caps_calloc(1, RESPONSE_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (request == NULL || response == NULL) {
        if (request) free(request);
        if (response) free(response);
        return ESP_ERR_NO_MEM;
    }

    size_t offset = 0;
    offset += snprintf(request + offset, REQUEST_BUFFER_SIZE - offset,
                       "{\"model\":\"" OLLAMA_MODEL "\",\"messages\":[");

    for (int i = 0; i < s_message_count; i++) {
        size_t esc_len = strlen(s_messages[i].content) * 2 + 16;
        char *escaped = heap_caps_malloc(esc_len, MALLOC_CAP_8BIT);
        if (escaped == NULL) {
            free(request);
            free(response);
            return ESP_ERR_NO_MEM;
        }
        json_escape_text(escaped, esc_len, s_messages[i].content);

        int written = snprintf(request + offset, REQUEST_BUFFER_SIZE - offset,
                               "%s{\"role\":\"%s\",\"content\":\"%s\"}",
                               (i > 0) ? "," : "",
                               s_messages[i].role,
                               escaped);
        free(escaped);

        if (written < 0 || (size_t)written >= REQUEST_BUFFER_SIZE - offset) {
            free(request);
            free(response);
            return ESP_ERR_INVALID_SIZE;
        }
        offset += written;
    }

    int closing = snprintf(request + offset, REQUEST_BUFFER_SIZE - offset,
                           "],\"think\":false,\"stream\":false,\"options\":{\"num_ctx\":4096,\"temperature\":0.75}}");
    if (closing < 0 || (size_t)closing >= REQUEST_BUFFER_SIZE - offset) {
        free(request);
        free(response);
        return ESP_ERR_INVALID_SIZE;
    }
    offset += closing;

    const esp_http_client_config_t config = {
        .url = OLLAMA_CHAT_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 60000,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        free(request);
        free(response);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_set_header(client, "Content-Type", "application/json");
    if (err == ESP_OK) err = esp_http_client_open(client, (int)offset);
    if (err == ESP_OK && esp_http_client_write(client, request, (int)offset) != (int)offset) err = ESP_FAIL;
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

    if (!extract_answer(response, s_last_answer, sizeof(s_last_answer))) {
        ESP_LOGE(TAG, "No se encontro el texto en la respuesta (%u bytes)", (unsigned int)total_read);
        free(response);
        return ESP_FAIL;
    }

    free(response);

    bool is_farewell = process_farewell(s_last_answer, user_input);
    if (out_is_farewell != NULL) {
        *out_is_farewell = is_farewell;
    }

    ESP_LOGI(TAG, "Respuesta LLM: %s %s", s_last_answer, is_farewell ? "[DESPEDIDA]" : "");

    /* Guardar en el historial de la conversacion */
    add_message("assistant", s_last_answer);

    return ESP_OK;
}

/*
 * Genera el saludo inicial en tiempo real (dinamico, no pre-seteado).
 */
esp_err_t llm_generate_greeting(void)
{
    llm_reset_conversation();
    add_message("system", SYSTEM_PROMPT);
    add_message("user", "Hola Bombon");

    bool dummy = false;
    return perform_chat_request(&dummy, "Hola Bombon");
}

/*
 * Envia el turno del usuario dentro de la conversacion fluida continua.
 */
esp_err_t llm_chat(const char *user_text, bool *out_is_farewell)
{
    if (user_text == NULL || *user_text == '\0') return ESP_ERR_INVALID_ARG;
    if (out_is_farewell != NULL) *out_is_farewell = false;

    if (s_message_count == 0) {
        add_message("system", SYSTEM_PROMPT);
    }

    add_message("user", user_text);
    return perform_chat_request(out_is_farewell, user_text);
}

esp_err_t llm_answer(const char *user_text)
{
    bool dummy = false;
    return llm_chat(user_text, &dummy);
}

const char *llm_get_last_answer(void)
{
    return s_last_answer;
}
