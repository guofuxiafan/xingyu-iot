/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: ESPRESSIF MIT
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_video_device.h"
#include "linux/videodev2.h"
#include "lwip/apps/netbiosns.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "camera_source.h"
#include "example_video_common.h"
#include "provisioning_button.h"
#include "provisioning_manager.h"
#include "remote_log.h"
#include "sta_connect.h"
#include "wifi_cred_store.h"
#include "ws_streamer.h"

#define EXAMPLE_MDNS_INSTANCE  CONFIG_EXAMPLE_MDNS_INSTANCE
#define EXAMPLE_MDNS_HOST_NAME CONFIG_EXAMPLE_MDNS_HOST_NAME
#define CSI_JPEG_QUALITY       CONFIG_EXAMPLE_JPEG_COMPRESSION_QUALITY
#define USB_JPEG_QUALITY       70

extern const uint8_t index_html_gz_start[] asm("_binary_index_html_gz_start");
extern const uint8_t index_html_gz_end[] asm("_binary_index_html_gz_end");
extern const uint8_t loading_jpg_gz_start[] asm("_binary_loading_jpg_gz_start");
extern const uint8_t loading_jpg_gz_end[] asm("_binary_loading_jpg_gz_end");
extern const uint8_t favicon_ico_gz_start[] asm("_binary_favicon_ico_gz_start");
extern const uint8_t favicon_ico_gz_end[] asm("_binary_favicon_ico_gz_end");
extern const uint8_t assets_index_js_gz_start[] asm("_binary_index_js_gz_start");
extern const uint8_t assets_index_js_gz_end[] asm("_binary_index_js_gz_end");
extern const uint8_t assets_index_css_gz_start[] asm("_binary_index_css_gz_start");
extern const uint8_t assets_index_css_gz_end[] asm("_binary_index_css_gz_end");

static const char *TAG = "camera_app";
static camera_source_t *s_sources[CAMERA_SOURCE_COUNT_MAX];
static uint8_t s_source_count;

static esp_err_t camera_frame_cb(const camera_source_frame_t *frame, void *user_ctx)
{
    (void)user_ctx;

    ws_streamer_frame_t ws_frame = {
        .camera_id = frame->camera_id,
        .timestamp_ms = frame->timestamp_ms,
        .jpeg_data = frame->jpeg_data,
        .jpeg_len = frame->jpeg_len,
    };
    return ws_streamer_submit(&ws_frame);
}

static char *get_cameras_json(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *cameras = cJSON_CreateArray();
    if (!root || !cameras) {
        cJSON_Delete(root);
        cJSON_Delete(cameras);
        return NULL;
    }
    cJSON_AddItemToObject(root, "cameras", cameras);

    for (int i = 0; i < s_source_count; i++) {
        camera_source_info_t info;
        camera_source_get_info(s_sources[i], &info);
        if (!info.active) {
            continue;
        }

        char text[48];
        cJSON *camera = cJSON_CreateObject();
        cJSON_AddNumberToObject(camera, "index", info.camera_id);
        cJSON_AddStringToObject(camera, "src", "");
        cJSON_AddBoolToObject(camera, "websocketOnly", true);
        cJSON_AddNumberToObject(camera, "currentFrameRate", info.frame_rate);
        cJSON_AddNumberToObject(camera, "currentImageFormat", 0);

        snprintf(text, sizeof(text), "JPEG %" PRIu32 "x%" PRIu32, info.width, info.height);
        cJSON_AddStringToObject(camera, "currentImageFormatDescription", text);

        cJSON *current_resolution = cJSON_CreateObject();
        cJSON_AddNumberToObject(current_resolution, "width", info.width);
        cJSON_AddNumberToObject(current_resolution, "height", info.height);
        cJSON_AddItemToObject(camera, "currentResolution", current_resolution);

        cJSON *image_formats = cJSON_CreateArray();
        cJSON *image_format = cJSON_CreateObject();
        cJSON_AddNumberToObject(image_format, "id", 0);
        cJSON_AddStringToObject(image_format, "description", text);
        if (info.quality_supported) {
            cJSON_AddNumberToObject(camera, "currentQuality", info.jpeg_quality);

            cJSON *quality = cJSON_CreateObject();
            cJSON_AddNumberToObject(quality, "min", 1);
            cJSON_AddNumberToObject(quality, "max", 100);
            cJSON_AddNumberToObject(quality, "step", 1);
            cJSON_AddNumberToObject(quality, "default", info.kind == CAMERA_SOURCE_KIND_USB_UVC ? USB_JPEG_QUALITY : CSI_JPEG_QUALITY);
            cJSON_AddItemToObject(image_format, "quality", quality);
        }
        cJSON_AddItemToArray(image_formats, image_format);
        cJSON_AddItemToObject(camera, "imageFormats", image_formats);

        cJSON_AddStringToObject(camera, "sourceType", info.kind == CAMERA_SOURCE_KIND_USB_UVC ? "usb_uvc" : "csi");
        cJSON_AddItemToArray(cameras, camera);
    }

    char *output = cJSON_Print(root);
    cJSON_Delete(root);
    return output;
}

static esp_err_t camera_info_handler(httpd_req_t *req)
{
    char *output = get_cameras_json();
    ESP_RETURN_ON_FALSE(output, ESP_ERR_NO_MEM, TAG, "failed to render camera JSON");

    httpd_resp_set_type(req, "application/json");
    esp_err_t ret = httpd_resp_sendstr(req, output);
    free(output);
    return ret;
}

static esp_err_t camera_settings_handler(httpd_req_t *req)
{
    esp_err_t ret = ESP_FAIL;
    cJSON *root = NULL;
    char *content = calloc(1, req->content_len + 1);
    ESP_RETURN_ON_FALSE(content, ESP_ERR_NO_MEM, TAG, "failed to allocate request body");

    int received = httpd_req_recv(req, content, req->content_len);
    if (received <= 0) {
        ret = received;
        ESP_LOGE(TAG, "failed to receive request body");
        goto fail;
    }

    root = cJSON_Parse(content);
    ESP_GOTO_ON_FALSE(root, ESP_ERR_INVALID_ARG, fail, TAG, "failed to parse JSON");

    cJSON *json_index = cJSON_GetObjectItem(root, "index");
    cJSON *json_quality = cJSON_GetObjectItem(root, "jpeg_quality");
    ESP_GOTO_ON_FALSE(json_index && cJSON_IsNumber(json_index), ESP_ERR_INVALID_ARG, cleanup_json, TAG, "missing index");
    ESP_GOTO_ON_FALSE(json_quality && cJSON_IsNumber(json_quality), ESP_ERR_INVALID_ARG, cleanup_json, TAG, "missing jpeg_quality");

    int index = json_index->valueint;
    ESP_GOTO_ON_FALSE(index >= 0 && index < s_source_count && camera_source_is_active(s_sources[index]),
                      ESP_ERR_INVALID_ARG, cleanup_json, TAG, "invalid camera index");

    ret = camera_source_set_jpeg_quality(s_sources[index], (uint8_t)json_quality->valueint);
    if (ret == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "cam%d does not support runtime JPEG quality control", index);
        ret = ESP_OK;
    }
    ESP_GOTO_ON_ERROR(ret, cleanup_json, TAG, "failed to set JPEG quality");

    cJSON_Delete(root);
    free(content);
    return httpd_resp_sendstr(req, "OK");

cleanup_json:
    cJSON_Delete(root);
fail:
    free(content);
    if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
        httpd_resp_send_408(req);
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid camera config");
    }
    return ret;
}

static esp_err_t static_file_handler(httpd_req_t *req)
{
    const char *uri = req->uri;

    if (strcmp(uri, "/") == 0) {
        httpd_resp_set_type(req, "text/html");
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        return httpd_resp_send(req, (const char *)index_html_gz_start, index_html_gz_end - index_html_gz_start);
    } else if (strcmp(uri, "/loading.jpg") == 0) {
        httpd_resp_set_type(req, "image/jpeg");
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        return httpd_resp_send(req, (const char *)loading_jpg_gz_start, loading_jpg_gz_end - loading_jpg_gz_start);
    } else if (strcmp(uri, "/favicon.ico") == 0) {
        httpd_resp_set_type(req, "image/x-icon");
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        return httpd_resp_send(req, (const char *)favicon_ico_gz_start, favicon_ico_gz_end - favicon_ico_gz_start);
    } else if (strcmp(uri, "/assets/index.js") == 0) {
        httpd_resp_set_type(req, "application/javascript");
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        return httpd_resp_send(req, (const char *)assets_index_js_gz_start, assets_index_js_gz_end - assets_index_js_gz_start);
    } else if (strcmp(uri, "/assets/index.css") == 0) {
        httpd_resp_set_type(req, "text/css");
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
        return httpd_resp_send(req, (const char *)assets_index_css_gz_start, assets_index_css_gz_end - assets_index_css_gz_start);
    }

    httpd_resp_send_404(req);
    return ESP_FAIL;
}

static esp_err_t http_server_init(void)
{
    httpd_handle_t httpd;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.stack_size = 1024 * 6;

    httpd_uri_t camera_info_uri = {
        .uri = "/api/get_camera_info",
        .method = HTTP_GET,
        .handler = camera_info_handler,
    };
    httpd_uri_t camera_settings_uri = {
        .uri = "/api/set_camera_config",
        .method = HTTP_POST,
        .handler = camera_settings_handler,
    };
    httpd_uri_t static_file_uri = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = static_file_handler,
    };

    ESP_LOGI(TAG, "Starting HTTP server on port %d", config.server_port);
    ESP_RETURN_ON_ERROR(httpd_start(&httpd, &config), TAG, "failed to start HTTP server");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(httpd, &camera_info_uri), TAG, "failed to register camera info API");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(httpd, &camera_settings_uri), TAG, "failed to register camera settings API");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(httpd, &static_file_uri), TAG, "failed to register static handler");
    return ESP_OK;
}

static void usb_dual_retry_task(void *arg);

static esp_err_t start_camera_sources(void)
{
    const camera_source_config_t configs[] = {
#if EXAMPLE_ENABLE_MIPI_CSI_CAM_SENSOR
        {
            .camera_id = 0,
            .kind = CAMERA_SOURCE_KIND_CSI,
            .dev_name = ESP_VIDEO_MIPI_CSI_DEVICE_NAME,
            .width = CAMERA_SOURCE_CSI_WIDTH,
            .height = CAMERA_SOURCE_CSI_HEIGHT,
            .jpeg_quality = CSI_JPEG_QUALITY,
            .frame_cb = camera_frame_cb,
        },
#endif
#if EXAMPLE_ENABLE_USB_UVC_CAM_SENSOR
        {
            .camera_id = 1,
            .kind = CAMERA_SOURCE_KIND_USB_UVC,
            .dev_name = ESP_VIDEO_USB_UVC_DEVICE_NAME(0),
            .width = CAMERA_SOURCE_USB_WIDTH,
            .height = CAMERA_SOURCE_USB_HEIGHT,
            .target_pixel_format = V4L2_PIX_FMT_YUYV,
            .target_frame_rate = 10,
            .jpeg_quality = USB_JPEG_QUALITY,
            .frame_cb = camera_frame_cb,
        },
        {
            .camera_id = 2,
            .kind = CAMERA_SOURCE_KIND_USB_UVC,
            .dev_name = ESP_VIDEO_USB_UVC_DEVICE_NAME(1),
            .width = CAMERA_SOURCE_USB_WIDTH,
            .height = CAMERA_SOURCE_USB_HEIGHT,
            .target_pixel_format = V4L2_PIX_FMT_YUYV,
            .target_frame_rate = 10,
            .jpeg_quality = USB_JPEG_QUALITY,
            .frame_cb = camera_frame_cb,
        },
#endif
    };

    int valid_count = 0;
    bool has_usb = false;
    for (int i = 0; i < sizeof(configs) / sizeof(configs[0]) && i < CAMERA_SOURCE_COUNT_MAX; i++) {
        if (configs[i].kind == CAMERA_SOURCE_KIND_USB_UVC) {
            has_usb = true;
            continue; /* USB cameras started atomically below */
        }

        camera_source_t *source = NULL;
        esp_err_t ret = camera_source_create(&configs[i], &source);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "cam%d skipped: %s", configs[i].camera_id, esp_err_to_name(ret));
            continue;
        }

        ret = camera_source_start(source);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "cam%d start failed: %s", configs[i].camera_id, esp_err_to_name(ret));
            camera_source_destroy(source);
            continue;
        }

        s_sources[configs[i].camera_id] = source;
        valid_count++;
    }

    s_source_count = CAMERA_SOURCE_COUNT_MAX;
    ESP_LOGI(TAG, "camera init summary: configured=%d valid=%d (USB deferred)",
             (int)(sizeof(configs) / sizeof(configs[0])), valid_count);

    if (has_usb) {
        xTaskCreate(usb_dual_retry_task, "usb_dual_retry", 4096, NULL, 5, NULL);
    }

    return valid_count > 0 ? ESP_OK : ESP_ERR_NOT_FOUND;
}

static bool s_usb_dual_ready = false;

static void usb_dual_retry_task(void *arg)
{
    const camera_source_config_t usb_configs[2] = {
        {
            .camera_id = 1,
            .kind = CAMERA_SOURCE_KIND_USB_UVC,
            .dev_name = ESP_VIDEO_USB_UVC_DEVICE_NAME(0),
            .width = CAMERA_SOURCE_USB_WIDTH,
            .height = CAMERA_SOURCE_USB_HEIGHT,
            .target_pixel_format = V4L2_PIX_FMT_YUYV,
            .target_frame_rate = 10,
            .jpeg_quality = USB_JPEG_QUALITY,
            .frame_cb = camera_frame_cb,
        },
        {
            .camera_id = 2,
            .kind = CAMERA_SOURCE_KIND_USB_UVC,
            .dev_name = ESP_VIDEO_USB_UVC_DEVICE_NAME(1),
            .width = CAMERA_SOURCE_USB_WIDTH,
            .height = CAMERA_SOURCE_USB_HEIGHT,
            .target_pixel_format = V4L2_PIX_FMT_YUYV,
            .target_frame_rate = 10,
            .jpeg_quality = USB_JPEG_QUALITY,
            .frame_cb = camera_frame_cb,
        },
    };

    while (!s_usb_dual_ready) {
        camera_source_t *srcs[2] = {NULL, NULL};
        bool both_ok = true;

        for (int i = 0; i < 2; i++) {
            if (camera_source_create(&usb_configs[i], &srcs[i]) != ESP_OK) {
                both_ok = false;
                break;
            }
            if (camera_source_start(srcs[i]) != ESP_OK) {
                both_ok = false;
                break;
            }
        }

        if (both_ok) {
            s_sources[1] = srcs[0];
            s_sources[2] = srcs[1];
            s_usb_dual_ready = true;
            ESP_LOGI(TAG, "Dual USB cameras ready");
        } else {
            for (int i = 0; i < 2; i++) {
                if (srcs[i]) {
                    camera_source_destroy(srcs[i]);
                }
            }
            ESP_LOGW(TAG, "Dual USB init failed, retrying in 2s");
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }
    vTaskDelete(NULL);
}

static void initialise_mdns(void)
{
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set(EXAMPLE_MDNS_HOST_NAME));
    ESP_ERROR_CHECK(mdns_instance_name_set(EXAMPLE_MDNS_INSTANCE));

    mdns_txt_item_t serviceTxtData[] = {
        {"board", CONFIG_IDF_TARGET},
        {"path", "/"},
    };

    ESP_ERROR_CHECK(mdns_service_add("ESP32-WebServer", "_http", "_tcp", 80, serviceTxtData,
                                     sizeof(serviceTxtData) / sizeof(serviceTxtData[0])));
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(example_video_init());

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    initialise_mdns();
    netbiosns_init();
    netbiosns_set_name(EXAMPLE_MDNS_HOST_NAME);

    provisioning_button_init();

    if (wifi_cred_store_is_provisioned()) {
        esp_err_t sta_ret = sta_connect_from_nvs();
        if (sta_ret == ESP_OK) {
            char pc_ip[16] = {0};
            char ws_url[64] = {0};

            if (wifi_cred_store_load_pc_ip(pc_ip, sizeof(pc_ip)) == ESP_OK && pc_ip[0] != '\0') {
                snprintf(ws_url, sizeof(ws_url), "ws://%s:8765", pc_ip);
                remote_log_start(pc_ip, REMOTE_LOG_UDP_PORT);
                ESP_LOGI(TAG, "Remote UDP log enabled: %s:%d", pc_ip, REMOTE_LOG_UDP_PORT);
            }

            ESP_ERROR_CHECK(ws_streamer_start(ws_url[0] ? ws_url : NULL));
            ESP_ERROR_CHECK(start_camera_sources());
            ESP_ERROR_CHECK(http_server_init());

            ESP_LOGI(TAG, "Camera web server starts");
        } else {
            ESP_LOGW(TAG, "STA connect failed after retries, restarting to retry...");
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_restart();
        }
    } else {
        ESP_LOGI(TAG, "NVS not provisioned, entering AP provisioning mode");
        provisioning_manager_run_ap_mode();
    }
}
