#pragma once
#include "esp_err.h"
esp_err_t transcription_send_last_recording(void);
const char *transcription_get_last_text(void);
