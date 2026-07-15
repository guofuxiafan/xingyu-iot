#include "sta_connect.h"
#include "wifi_cred_store.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

/* Kconfig defaults */
#ifndef CONFIG_EXAMPLE_PROVISION_STA_MAX_RETRY
#define CONFIG_EXAMPLE_PROVISION_STA_MAX_RETRY 5
#endif

#ifndef EXAMPLE_WIFI_SCAN_METHOD
#define EXAMPLE_WIFI_SCAN_METHOD WIFI_ALL_CHANNEL_SCAN
#endif

#ifndef EXAMPLE_WIFI_CONNECT_AP_SORT_METHOD
#define EXAMPLE_WIFI_CONNECT_AP_SORT_METHOD WIFI_CONNECT_AP_BY_SIGNAL
#endif

static const char *TAG = "sta_connect";
static SemaphoreHandle_t s_sem_connect_done;
static int s_retry_count = 0;

static void sta_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "WiFi connected");
#if CONFIG_EXAMPLE_CONNECT_IPV6
        esp_netif_create_ip6_linklocal(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"));
#endif
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_retry_count++;
        wifi_event_sta_disconnected_t *disconn = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "WiFi disconnected, reason=%d, retry %d/%d",
                 disconn->reason, s_retry_count, CONFIG_EXAMPLE_PROVISION_STA_MAX_RETRY);

        if (s_retry_count < CONFIG_EXAMPLE_PROVISION_STA_MAX_RETRY) {
            ESP_LOGI(TAG, "Reconnect attempt %d/%d in 5s...",
                     s_retry_count + 1, CONFIG_EXAMPLE_PROVISION_STA_MAX_RETRY);
            vTaskDelay(pdMS_TO_TICKS(5000));
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "WiFi connection failed after %d retries", CONFIG_EXAMPLE_PROVISION_STA_MAX_RETRY);
            xSemaphoreGive(s_sem_connect_done); /* signal failure */
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Connected, IPv4=" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        xSemaphoreGive(s_sem_connect_done); /* signal success */
    }
}

esp_err_t sta_connect_from_nvs(void)
{
    if (!wifi_cred_store_is_provisioned()) {
        ESP_LOGI(TAG, "NVS not provisioned, skipping STA connect");
        return ESP_ERR_NVS_NOT_FOUND;
    }

    char ssid[33] = {0};
    char password[65] = {0};

    esp_err_t err = wifi_cred_store_load(ssid, sizeof(ssid), password, sizeof(password));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load credentials: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Connecting to SSID=%.32s", ssid);

    /* ── WiFi init ────────────────────────────────── */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Create STA netif — use default API like original example_connect() */
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    if (sta_netif == NULL) {
        ESP_LOGE(TAG, "esp_netif_create_default_wifi_sta failed");
        return ESP_FAIL;
    }

    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);

    /* Register event handlers */
    s_sem_connect_done = xSemaphoreCreateBinary();
    s_retry_count = 0;

    esp_event_handler_instance_t instance_wifi;
    esp_event_handler_instance_t instance_ip;

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                        ESP_EVENT_ANY_ID, &sta_event_handler, NULL, &instance_wifi));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                        IP_EVENT_STA_GOT_IP, &sta_event_handler, NULL, &instance_ip));

    /* Configure WiFi credentials */
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));
    wifi_config.sta.scan_method = EXAMPLE_WIFI_SCAN_METHOD;
    wifi_config.sta.sort_method = EXAMPLE_WIFI_CONNECT_AP_SORT_METHOD;
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed: %s", esp_err_to_name(err));
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_wifi);
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_ip);
        vSemaphoreDelete(s_sem_connect_done);
        return err;
    }

    /* Block until got_ip (success) or max retries exceeded (failure) */
    BaseType_t sem_ret = xSemaphoreTake(s_sem_connect_done, pdMS_TO_TICKS(30000));

    if (s_retry_count >= CONFIG_EXAMPLE_PROVISION_STA_MAX_RETRY || sem_ret == pdFALSE) {
        /* Failure: stop WiFi and clean up */
        esp_wifi_stop();
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_wifi);
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_ip);
        vSemaphoreDelete(s_sem_connect_done);
        return ESP_FAIL;
    }

    /* Success: WiFi stays up, handlers stay registered for auto-reconnect */
    vSemaphoreDelete(s_sem_connect_done);
    return ESP_OK;
}
