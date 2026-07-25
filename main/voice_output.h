#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize the speaker and watch /storage for new or updated JSON files. */
esp_err_t voice_output_start(void);

/** Enqueue JSON for TTS playback via voice_task (non-blocking, safe for ISR/callback). */
esp_err_t voice_output_speak_json(const char *json_str);

#ifdef __cplusplus
}
#endif
