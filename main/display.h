#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t display_init(void);
void display_show_button_pressed(void);
void display_set_power(bool enabled);
