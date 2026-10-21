#pragma once

#include "esp_err.h"

esp_err_t display_init(void);
void display_show_standby(void);
void display_show_recording(void);
void display_show_thinking(void);
void display_show_speaking(void);
