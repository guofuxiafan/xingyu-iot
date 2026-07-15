#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CAMERA_SOURCE_COUNT_MAX 3
#define CAMERA_SOURCE_CSI_WIDTH 1920
#define CAMERA_SOURCE_CSI_HEIGHT 1080
#define CAMERA_SOURCE_USB_WIDTH 1280
#define CAMERA_SOURCE_USB_HEIGHT 720

typedef enum {
    CAMERA_SOURCE_KIND_CSI,
    CAMERA_SOURCE_KIND_USB_UVC,
} camera_source_kind_t;

typedef struct {
    uint8_t camera_id;
    uint64_t timestamp_ms;
    const uint8_t *jpeg_data;
    uint32_t jpeg_len;
    uint32_t width;
    uint32_t height;
} camera_source_frame_t;

typedef esp_err_t (*camera_source_frame_cb_t)(const camera_source_frame_t *frame, void *user_ctx);

typedef struct {
    uint8_t camera_id;
    camera_source_kind_t kind;
    const char *dev_name;
    uint32_t width;
    uint32_t height;
    uint32_t target_pixel_format;
    uint32_t target_frame_rate;
    uint8_t jpeg_quality;
    camera_source_frame_cb_t frame_cb;
    void *user_ctx;
} camera_source_config_t;

typedef struct {
    uint8_t camera_id;
    camera_source_kind_t kind;
    bool active;
    uint32_t width;
    uint32_t height;
    uint32_t frame_rate;
    uint32_t pixel_format;
    uint8_t jpeg_quality;
    bool quality_supported;
} camera_source_info_t;

typedef struct camera_source camera_source_t;

esp_err_t camera_source_create(const camera_source_config_t *config, camera_source_t **out_source);
esp_err_t camera_source_start(camera_source_t *source);
esp_err_t camera_source_set_jpeg_quality(camera_source_t *source, uint8_t quality);
void camera_source_get_info(camera_source_t *source, camera_source_info_t *info);
bool camera_source_is_active(camera_source_t *source);
void camera_source_destroy(camera_source_t *source);

#ifdef __cplusplus
}
#endif
