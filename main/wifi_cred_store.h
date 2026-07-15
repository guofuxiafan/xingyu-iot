#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Save WiFi credentials to NVS with two-phase commit.
 *
 * Writes ssid, password, then a "provisioned" flag, followed by nvs_commit.
 * Any failure before commit leaves NVS in an incomplete state which
 * is_provisioned() correctly detects as NOT provisioned.
 *
 * @param ssid      WiFi SSID, 1-32 characters (null-terminated)
 * @param password  WiFi password, 0-64 characters (null-terminated, empty string OK for open networks)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if lengths invalid
 */
esp_err_t wifi_cred_store_save(const char *ssid, const char *password);

/**
 * @brief Load WiFi credentials from NVS.
 *
 * @param ssid_out    Output buffer for SSID
 * @param ssid_cap    Capacity of ssid_out buffer (including null terminator)
 * @param password_out Output buffer for password
 * @param pass_cap    Capacity of password_out buffer (including null terminator)
 * @return ESP_OK on success, ESP_ERR_NVS_NOT_FOUND if not provisioned
 */
esp_err_t wifi_cred_store_load(char *ssid_out, size_t ssid_cap,
                               char *password_out, size_t pass_cap);

/**
 * @brief Check whether valid credentials are stored in NVS.
 *
 * Returns true ONLY when the "provisioned" flag is set AND
 * an ssid key exists with length between 1 and 32 (inclusive).
 *
 * @return true if provisioned and ssid is valid, false otherwise
 */
bool wifi_cred_store_is_provisioned(void);

/**
 * @brief Erase all credential keys from NVS.
 *
 * Removes ssid, password, pc_ip, and provisioned keys. Safe to call
 * even if keys don't exist.
 *
 * @return ESP_OK on success
 */
esp_err_t wifi_cred_store_erase(void);

/**
 * @brief Save PC IP address for WebSocket frame pushing.
 *
 * @param pc_ip  IPv4 address string (e.g. "192.168.1.100"), max 15 chars
 * @return ESP_OK on success
 */
esp_err_t wifi_cred_store_save_pc_ip(const char *pc_ip);

/**
 * @brief Load PC IP address from NVS.
 *
 * @param pc_ip_out  Output buffer, at least 16 bytes
 * @param pc_ip_cap  Capacity of output buffer
 * @return ESP_OK on success, ESP_ERR_NVS_NOT_FOUND if not set
 */
esp_err_t wifi_cred_store_load_pc_ip(char *pc_ip_out, size_t pc_ip_cap);

#ifdef __cplusplus
}
#endif
