#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the long-press button monitoring task on GPIO35 (BOOT).
 *
 * Creates a FreeRTOS task that polls the BOOT button (default GPIO35)
 * at 100 ms intervals. On a continuous press >= CONFIG_EXAMPLE_PROVISION_LONG_PRESS_MS
 * (default 3000 ms), the task erases WiFi NVS credentials and calls esp_restart()
 * to reboot into AP provisioning mode.
 *
 * The task runs at priority 5 with 4096 bytes stack on CPU0.
 * Short taps and noise are ignored by design (debounce via consecutive-read counter).
 *
 * @return ESP_OK on success
 */
esp_err_t provisioning_button_init(void);

#ifdef __cplusplus
}
#endif
