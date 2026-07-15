#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start AP provisioning mode (blocks until esp_restart).
 *
 * Creates a SoftAP hotspot, starts a DNS hijack server, serves a captive
 * portal HTTP form on port 80, and handles POST /api/set_wifi_config.
 *
 * On successful credential save, calls esp_restart() to reboot into STA mode.
 * This function never returns.
 *
 * @return Never returns (calls esp_restart on success)
 */
esp_err_t provisioning_manager_run_ap_mode(void);

#ifdef __cplusplus
}
#endif
