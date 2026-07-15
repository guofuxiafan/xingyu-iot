#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Connect to WiFi STA using credentials stored in NVS.
 *
 * Reads SSID and password from the wifi_creds NVS namespace.
 * If NVS is not provisioned, returns ESP_ERR_NVS_NOT_FOUND immediately
 * without calling esp_wifi_init.
 *
 * Blocks until:
 *   - IP_EVENT_STA_GOT_IP → returns ESP_OK
 *   - WIFI_EVENT_STA_DISCONNECTED exceeds CONFIG_EXAMPLE_PROVISION_STA_MAX_RETRY → returns ESP_FAIL
 *
 * @return ESP_OK on successful connection, ESP_ERR_NVS_NOT_FOUND if no credentials,
 *         ESP_FAIL after exhausting retries
 */
esp_err_t sta_connect_from_nvs(void);

#ifdef __cplusplus
}
#endif
