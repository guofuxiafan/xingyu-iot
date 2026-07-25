#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize the speaker and watch /storage for new or updated JSON files. */
esp_err_t voice_output_start(void);

#ifdef __cplusplus
}
#endif
