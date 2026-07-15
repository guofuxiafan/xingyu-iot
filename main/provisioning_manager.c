#include "provisioning_manager.h"
#include "wifi_cred_store.h"
#include "dns_server.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "provisioning_manager";

/* Kconfig defaults */
#ifndef CONFIG_EXAMPLE_PROVISION_AP_SSID_PREFIX
#define CONFIG_EXAMPLE_PROVISION_AP_SSID_PREFIX "ESP-CAM-PROV-"
#endif

#ifndef CONFIG_EXAMPLE_PROVISION_AP_TIMEOUT_MS
#define CONFIG_EXAMPLE_PROVISION_AP_TIMEOUT_MS 300000
#endif

/* ── embedded root.html ─────────────────────────────────────── */
extern const char root_html_start[] asm("_binary_root_html_start");
extern const char root_html_end[] asm("_binary_root_html_end");

/* ── DHCP captive portal helper ─────────────────────────────── */

static void dhcp_set_captiveportal_url(esp_netif_t *ap_netif)
{
    esp_netif_ip_info_t ip_info;
    esp_err_t err = esp_netif_get_ip_info(ap_netif, &ip_info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to get AP IP for DHCP option 114: %s", esp_err_to_name(err));
        return;
    }

    char uri[64];
    snprintf(uri, sizeof(uri), "http://" IPSTR "/", IP2STR(&ip_info.ip));

    err = esp_netif_dhcps_stop(ap_netif);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "dhcps_stop failed: %s", esp_err_to_name(err));
    }

    const char *captive_uri = uri;

    err = esp_netif_dhcps_option(ap_netif,
                                  ESP_NETIF_OP_SET,
                                  ESP_NETIF_CAPTIVEPORTAL_URI,
                                  (void *)captive_uri,
                                  (uint32_t)strlen(captive_uri));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "failed to set DHCP option 114: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "DHCP option 114 set: %s", uri);
    }

    err = esp_netif_dhcps_start(ap_netif);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "dhcps_start failed: %s", esp_err_to_name(err));
    }
}

/* ── HTTP handlers ──────────────────────────────────────────── */

static esp_err_t root_get_handler(httpd_req_t *req)
{
    size_t root_len = root_html_end - root_html_start;
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, root_html_start, root_len);
    return ESP_OK;
}

static esp_err_t set_wifi_config_post_handler(httpd_req_t *req)
{
    char buf[512];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty request body");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    /* Manual URL-encoded form parse: ssid=...&password=...&pc_ip=... */
    char ssid[64] = {0};
    char password[128] = {0};
    char pc_ip[32] = {0};

    char *ssid_start = strstr(buf, "ssid=");
    if (ssid_start == NULL) {
        ESP_LOGE(TAG, "POST: missing ssid field");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing ssid field");
        return ESP_FAIL;
    }
    ssid_start += 5; /* skip "ssid=" */

    char *ssid_end = strchr(ssid_start, '&');
    size_t ssid_encoded_len;
    if (ssid_end) {
        ssid_encoded_len = ssid_end - ssid_start;
    } else {
        ssid_encoded_len = strlen(ssid_start);
    }

    if (ssid_encoded_len >= sizeof(ssid)) {
        ssid_encoded_len = sizeof(ssid) - 1;
    }
    memcpy(ssid, ssid_start, ssid_encoded_len);

    /* URL-decode SSID (simple: replace + with space, %XX with char) */
    {
        char *src = ssid;
        char *dst = ssid;
        while (*src) {
            if (*src == '+') {
                *dst++ = ' ';
                src++;
            } else if (*src == '%' && src[1] && src[2]) {
                char hex[3] = {src[1], src[2], '\0'};
                *dst++ = (char)strtol(hex, NULL, 16);
                src += 3;
            } else {
                *dst++ = *src++;
            }
        }
        *dst = '\0';
    }

    /* Parse password */
    char *pass_start = strstr(buf, "password=");
    if (pass_start) {
        pass_start += 9; /* skip "password=" */
        /* find next & or end */
        char *pass_end = strchr(pass_start, '&');
        size_t pass_encoded_len;
        if (pass_end) {
            pass_encoded_len = pass_end - pass_start;
        } else {
            pass_encoded_len = strlen(pass_start);
        }
        if (pass_encoded_len >= sizeof(password)) {
            pass_encoded_len = sizeof(password) - 1;
        }
        memcpy(password, pass_start, pass_encoded_len);

        /* URL-decode password */
        char *src = password;
        char *dst = password;
        while (*src) {
            if (*src == '+') {
                *dst++ = ' ';
                src++;
            } else if (*src == '%' && src[1] && src[2]) {
                char hex[3] = {src[1], src[2], '\0'};
                *dst++ = (char)strtol(hex, NULL, 16);
                src += 3;
            } else {
                *dst++ = *src++;
            }
        }
        *dst = '\0';
    }

    size_t ssid_len = strlen(ssid);
    size_t pass_len = strlen(password);

    /* Parse optional PC IP */
    char *ip_start = strstr(buf, "pc_ip=");
    if (ip_start) {
        ip_start += 6;
        char *ip_end = strchr(ip_start, '&');
        size_t ip_len = ip_end ? (size_t)(ip_end - ip_start) : strlen(ip_start);
        if (ip_len >= sizeof(pc_ip)) ip_len = sizeof(pc_ip) - 1;
        memcpy(pc_ip, ip_start, ip_len);
        /* URL-decode */
        char *s = pc_ip, *d = pc_ip;
        while (*s) {
            if (*s == '+') { *d++ = ' '; s++; }
            else if (*s == '%' && s[1] && s[2]) { char h[3]={s[1],s[2],0}; *d++=(char)strtol(h,NULL,16); s+=3; }
            else *d++ = *s++;
        }
        *d = '\0';
    }

    ESP_LOGI(TAG, "POST /api/set_wifi_config ssid=%.32s password=*** pc_ip=%.15s", ssid, pc_ip);

    /* Validate */
    if (ssid_len < 1 || ssid_len > 32) {
        ESP_LOGW(TAG, "invalid SSID length %u", (unsigned)ssid_len);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid SSID (1-32 chars)");
        return ESP_FAIL;
    }
    if (pass_len > 64) {
        ESP_LOGW(TAG, "invalid password length %u", (unsigned)pass_len);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid password (max 64 chars)");
        return ESP_FAIL;
    }

    esp_err_t err = wifi_cred_store_save(ssid, password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS save failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save credentials");
        return ESP_FAIL;
    }

    /* Save PC IP if provided (best-effort: don't fail provisioning if IP save fails) */
    if (pc_ip[0] != '\0') {
        esp_err_t ip_err = wifi_cred_store_save_pc_ip(pc_ip);
        if (ip_err != ESP_OK) {
            ESP_LOGW(TAG, "PC IP save failed: %s (continuing)", esp_err_to_name(ip_err));
        }
    }

    ESP_LOGI(TAG, "NVS save OK");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "provisioning complete, restarting...", HTTPD_RESP_USE_STRLEN);

    /* Flush response before restart */
    vTaskDelay(pdMS_TO_TICKS(1500));
    ESP_LOGI(TAG, "provisioning complete, restarting...");
    esp_restart();

    /* never reached */
    return ESP_OK;
}

static esp_err_t http_404_handler(httpd_req_t *req, httpd_err_code_t err)
{
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_FAIL;
}

/* ── SoftAP start ───────────────────────────────────────────── */

static esp_netif_t *provisioning_ap_start(void)
{
    /* MUST create AP netif BEFORE esp_wifi_init for esp_wifi_remote */
    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
    if (ap_netif == NULL) {
        ESP_LOGE(TAG, "esp_netif_create_default_wifi_ap failed");
        return NULL;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return NULL;
    }

    /* Get AP MAC via esp_wifi_remote (routes to C6 co-processor) */
    uint8_t mac[6];
    err = esp_wifi_get_mac(WIFI_IF_AP, mac);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_get_mac failed: %s, using zeros", esp_err_to_name(err));
        memset(mac, 0, sizeof(mac));
    }

    char ssid[33];
    snprintf(ssid, sizeof(ssid), "%s%02X%02X%02X",
             CONFIG_EXAMPLE_PROVISION_AP_SSID_PREFIX,
             mac[3], mac[4], mac[5]);

    wifi_config_t wifi_config = {
        .ap = {
            .ssid_len = (uint8_t)strlen(ssid),
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
        }
    };
    memcpy(wifi_config.ap.ssid, ssid, strlen(ssid));

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &wifi_config);
    esp_wifi_start();

    esp_netif_ip_info_t ip_info;
    err = esp_netif_get_ip_info(ap_netif, &ip_info);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "SoftAP SSID=%s IP=" IPSTR, ssid, IP2STR(&ip_info.ip));
    } else {
        ESP_LOGI(TAG, "SoftAP SSID=%s", ssid);
    }

    dhcp_set_captiveportal_url(ap_netif);
    return ap_netif;
}

/* ── HTTP server start ──────────────────────────────────────── */

static httpd_handle_t provisioning_httpd_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 13;
    config.lru_purge_enable = true;
    config.server_port = 80;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return NULL;
    }

    httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
    };
    httpd_register_uri_handler(server, &root_uri);

    httpd_uri_t set_wifi_uri = {
        .uri = "/api/set_wifi_config",
        .method = HTTP_POST,
        .handler = set_wifi_config_post_handler,
    };
    httpd_register_uri_handler(server, &set_wifi_uri);

    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, http_404_handler);

    ESP_LOGI(TAG, "HTTPD started on port 80");
    return server;
}

/* ── public API ─────────────────────────────────────────────── */

esp_err_t provisioning_manager_run_ap_mode(void)
{
    esp_netif_t *ap_netif = provisioning_ap_start();
    if (ap_netif == NULL) {
        return ESP_FAIL;
    }

    httpd_handle_t httpd = provisioning_httpd_start();
    if (httpd == NULL) {
        return ESP_FAIL;
    }

    esp_err_t err = start_dns_server_on_netif(ap_netif);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DNS server failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "DNS server started");
    ESP_LOGI(TAG, "AP provisioning mode active — waiting for client connection");

    /* Stay in AP mode indefinitely; esp_restart() is called by POST handler */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_EXAMPLE_PROVISION_AP_TIMEOUT_MS));
        ESP_LOGW(TAG, "AP provisioning mode timeout (%d s) — still waiting",
                 CONFIG_EXAMPLE_PROVISION_AP_TIMEOUT_MS / 1000);
    }

    /* unreachable */
    return ESP_OK;
}
