#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

void llm_reset_conversation(void);
esp_err_t llm_generate_greeting(void);
esp_err_t llm_chat(const char *user_text, bool *out_is_farewell);
const char *llm_get_last_answer(void);

/* Mantenido por compatibilidad */
esp_err_t llm_answer(const char *user_text);

#ifdef __cplusplus
}
#endif
