#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t microphone_init(void);
esp_err_t microphone_start_stream(void);
size_t microphone_read_pcm(int16_t *buf, size_t samples_wanted);
esp_err_t microphone_stop_stream(void);
void microphone_flush(void);
void microphone_discard_samples(uint32_t ms);

esp_err_t microphone_record_five_seconds(void);
esp_err_t microphone_listen_and_record(uint32_t max_wait_sec,
                                       uint32_t max_speech_sec,
                                       bool (*abort_check)(void),
                                       bool *out_heard_speech);
esp_err_t microphone_get_last_recording(const int16_t **samples, size_t *sample_count);

#ifdef __cplusplus
}
#endif
