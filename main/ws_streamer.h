#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WS_STREAMER_FRAME_HEADER_SIZE 13
#define WS_STREAMER_MAX_JPEG_SIZE     (900 * 1024)

typedef struct {
    uint8_t camera_id;
    uint64_t timestamp_ms;
    const uint8_t *jpeg_data;
    uint32_t jpeg_len;
} ws_streamer_frame_t;

esp_err_t ws_streamer_start(const char *url);
esp_err_t ws_streamer_submit(const ws_streamer_frame_t *frame);

#ifdef __cplusplus
}
#endif
