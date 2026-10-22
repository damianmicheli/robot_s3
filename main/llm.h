#pragma once
#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t llm_answer(const char *user_text);
const char *llm_get_last_answer(void);
void llm_reset_conversation(void);
bool llm_is_farewell(void);

#ifdef __cplusplus
}
#endif
