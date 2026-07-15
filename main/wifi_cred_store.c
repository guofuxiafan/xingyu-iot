#include "wifi_cred_store.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = "wifi_cred_store";
static const char *NVS_NS = "wifi_creds";
static const char *KEY_SSID = "ssid";
static const char *KEY_PASSWORD = "password";
static const char *KEY_PROVISIONED = "provisioned";
static const char *KEY_PC_IP = "pc_ip";

/* ─── helpers ──────────────────────────────────────────────────── */

static inline bool valid_ssid_len(size_t len)
{
    return len >= 1 && len <= 32;
}

static inline bool valid_password_len(size_t len)
{
    return len <= 64;
}

/* ─── public API ────────────────────────────────────────────────── */

esp_err_t wifi_cred_store_save(const char *ssid, const char *password)
{
    if (ssid == NULL || password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t ssid_len = strlen(ssid);
    size_t pass_len = strlen(password);

    if (!valid_ssid_len(ssid_len)) {
        ESP_LOGE(TAG, "save: invalid SSID length %u (must be 1-32)", (unsigned)ssid_len);
        return ESP_ERR_INVALID_ARG;
    }
    if (!valid_password_len(pass_len)) {
        ESP_LOGE(TAG, "save: invalid password length %u (max 64)", (unsigned)pass_len);
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save: nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    /* two-phase commit: ssid → password → provisioned flag → commit */
    err = nvs_set_str(handle, KEY_SSID, ssid);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save: nvs_set_str ssid failed: %s", esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    err = nvs_set_str(handle, KEY_PASSWORD, password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save: nvs_set_str password failed: %s", esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    uint8_t provisioned = 1;
    err = nvs_set_u8(handle, KEY_PROVISIONED, provisioned);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save: nvs_set_u8 provisioned failed: %s", esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "save: nvs_commit failed: %s", esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    nvs_close(handle);
    ESP_LOGI(TAG, "save: credentials saved (SSID=%.32s, password=***)", ssid);
    return ESP_OK;
}

esp_err_t wifi_cred_store_load(char *ssid_out, size_t ssid_cap,
                               char *password_out, size_t pass_cap)
{
    if (ssid_out == NULL || password_out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!wifi_cred_store_is_provisioned()) {
        return ESP_ERR_NVS_NOT_FOUND;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "load: nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    size_t len;

    len = ssid_cap;
    err = nvs_get_str(handle, KEY_SSID, ssid_out, &len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "load: nvs_get_str ssid failed: %s", esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    len = pass_cap;
    err = nvs_get_str(handle, KEY_PASSWORD, password_out, &len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "load: nvs_get_str password failed: %s", esp_err_to_name(err));
        nvs_close(handle);
        return err;
    }

    nvs_close(handle);
    ESP_LOGI(TAG, "load: credentials loaded (SSID=%.32s)", ssid_out);
    return ESP_OK;
}

bool wifi_cred_store_is_provisioned(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return false;
    }

    uint8_t provisioned = 0;
    err = nvs_get_u8(handle, KEY_PROVISIONED, &provisioned);
    if (err != ESP_OK || provisioned != 1) {
        nvs_close(handle);
        return false;
    }

    /* verify ssid exists and has valid length */
    size_t ssid_len = 0;
    err = nvs_get_str(handle, KEY_SSID, NULL, &ssid_len);
    nvs_close(handle);

    if (err != ESP_OK) {
        return false;
    }

    /* ssid_len from nvs_get_str includes null terminator */
    size_t actual_len = (ssid_len > 0) ? (ssid_len - 1) : 0;
    return valid_ssid_len(actual_len);
}

esp_err_t wifi_cred_store_erase(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "erase: nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    /* erase each key individually — safe even if key doesn't exist */
    nvs_erase_key(handle, KEY_SSID);
    nvs_erase_key(handle, KEY_PASSWORD);
    nvs_erase_key(handle, KEY_PROVISIONED);
    nvs_erase_key(handle, KEY_PC_IP);

    err = nvs_commit(handle);
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "erase: nvs_commit failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "erase: credentials erased");
    return ESP_OK;
}

esp_err_t wifi_cred_store_save_pc_ip(const char *pc_ip)
{
    if (pc_ip == NULL || strlen(pc_ip) == 0 || strlen(pc_ip) > 15) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, KEY_PC_IP, pc_ip);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "save_pc_ip: saved PC IP=%.15s", pc_ip);
    }
    return err;
}

esp_err_t wifi_cred_store_load_pc_ip(char *pc_ip_out, size_t pc_ip_cap)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return ESP_ERR_NVS_NOT_FOUND;
    }

    size_t len = pc_ip_cap;
    err = nvs_get_str(handle, KEY_PC_IP, pc_ip_out, &len);
    nvs_close(handle);
    return err;
}
