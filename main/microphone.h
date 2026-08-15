#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Inicializa el bus I2S, pero no genera relojes ni captura hasta grabar. */
esp_err_t microphone_init(void);

/* Captura cinco segundos de audio PCM 16-bit mono en PSRAM. */
esp_err_t microphone_record_five_seconds(void);

/* Devuelve el audio de la captura más reciente para enviarlo por la red. */
esp_err_t microphone_get_last_recording(const int16_t **samples, size_t *sample_count);
