#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wake_word_init(void);
void wake_word_start(void);
void wake_word_stop(void);
bool wake_word_detected(void);
void wake_word_clear_detected(void);

void wake_word_toggle_mute(void);
void wake_word_set_mute(bool mute);
bool wake_word_is_muted(void);

#ifdef __cplusplus
}
#endif
