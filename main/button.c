#include "button.h"

#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_timer.h"

#define BUTTON_DEBOUNCE_MS 50
#define PIN_BUTTON      GPIO_NUM_4

static int s_raw_state = 1;
static int s_stable_state = 1;
static int64_t s_last_raw_change_ms = 0;

void button_init(void)
{
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << PIN_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };

    ESP_ERROR_CHECK(gpio_config(&config));

    int initial_state = gpio_get_level(PIN_BUTTON);

    s_raw_state = initial_state;
    s_stable_state = initial_state;
    s_last_raw_change_ms = esp_timer_get_time() / 1000;
}

bool button_was_pressed(void)
{
    int raw_state = gpio_get_level(PIN_BUTTON);
    int64_t now_ms = esp_timer_get_time() / 1000;

    if (raw_state != s_raw_state)
    {
        s_raw_state = raw_state;
        s_last_raw_change_ms = now_ms;
    }

    if ((now_ms - s_last_raw_change_ms) < BUTTON_DEBOUNCE_MS)
    {
        return false;
    }

    if (s_stable_state == s_raw_state)
    {
        return false;
    }

    s_stable_state = s_raw_state;

    if (s_stable_state == 0)
    {
        return true;
    }

    return false;
}

bool button_is_pressed(void)
{
    /* button_was_pressed() actualiza este estado estable y elimina rebotes. */
    return s_stable_state == 0;
}
