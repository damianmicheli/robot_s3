#pragma once

#include "esp_err.h"
#include "esp_http_client.h"

esp_err_t speaker_init(void);
esp_err_t speaker_play_startup_tone(void);
/* Recibe el WAV desde HTTP y lo envía a I2S sin guardarlo entero en RAM. */
esp_err_t speaker_play_wav_http(esp_http_client_handle_t client);
