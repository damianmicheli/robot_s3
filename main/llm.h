#pragma once
#include "esp_err.h"
esp_err_t llm_answer(const char *user_text);
const char *llm_get_last_answer(void);
