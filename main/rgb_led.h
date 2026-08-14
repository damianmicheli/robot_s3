#pragma once

#include <stdint.h>

#include "esp_err.h"

esp_err_t rgb_led_init(void);
void rgb_led_set_color(uint8_t red, uint8_t green, uint8_t blue);
void rgb_led_off(void);
