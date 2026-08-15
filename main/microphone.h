#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Inicializa el bus I2S, pero no genera relojes ni captura hasta grabar. */
esp_err_t microphone_init(void);

/* Captura cinco segundos y escribe en el monitor serie los niveles de audio. */
esp_err_t microphone_record_five_seconds(void);
esp_err_t microphone_get_last_recording(const int16_t **samples, size_t *sample_count);
