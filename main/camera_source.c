#include "camera_source.h"

#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/time.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_video_device.h"
#include "esp_video_ioctl.h"
#include "example_video_common.h"
#include "linux/videodev2.h"

#define CSI_VIDEO_BUFFER_NUMBER CONFIG_EXAMPLE_CAMERA_VIDEO_BUFFER_NUMBER
#define USB_VIDEO_BUFFER_NUMBER CONFIG_EXAMPLE_USB_CAMERA_VIDEO_BUFFER_NUMBER
#if USB_VIDEO_BUFFER_NUMBER > CSI_VIDEO_BUFFER_NUMBER
#define CAMERA_VIDEO_BUFFER_MAX USB_VIDEO_BUFFER_NUMBER
#else
#define CAMERA_VIDEO_BUFFER_MAX CSI_VIDEO_BUFFER_NUMBER
#endif
#define JPEG_MIN_SIZE           16
#define USB_OPEN_RETRIES        20
#define USB_OPEN_RETRY_DELAY_MS 2000
#define USB_WS_SEND_INTERVAL_MS 50
#define CSI_WS_SEND_INTERVAL_MS 50
#define CAMERA_PROCESSING_CORE  1
#if CONFIG_EXAMPLE_USB_MINIMAL_VALIDATION_MODE || CONFIG_EXAMPLE_USB_NETWORK_VALIDATION_MODE
#define DROP_LOG_INTERVAL_MS    5000
#else
#define DROP_LOG_INTERVAL_MS    1000
#endif
#define USB_DQBUF_TIMEOUT_MS    1000
#define USB_DQBUF_FAILURE_LIMIT 10
#define CAPTURE_STATS_WINDOW_MS 5000

struct camera_source {
    camera_source_config_t config;
    int fd;
    uint8_t *buffer[CAMERA_VIDEO_BUFFER_MAX];
    uint32_t buffer_size[CAMERA_VIDEO_BUFFER_MAX];
    uint32_t buffer_count;
    uint32_t pixel_format;
    uint32_t frame_rate;
    uint8_t jpeg_quality;
    bool quality_supported;
    example_encoder_handle_t encoder_handle;
    uint8_t *jpeg_out_buf;
    uint32_t jpeg_out_size;
    SemaphoreHandle_t encoder_lock;
    volatile TaskHandle_t task;
    volatile bool active;
    volatile bool stop_requested;
    uint32_t incomplete_mjpeg_drop_count;
    uint64_t next_incomplete_mjpeg_log_ms;
    uint32_t mjpeg_padding_trim_count;
    uint64_t mjpeg_padding_trim_bytes;
    uint32_t mjpeg_padding_trim_max;
    uint64_t next_mjpeg_padding_log_ms;
    uint32_t mjpeg_boundary_merge_count;
    uint64_t next_mjpeg_boundary_merge_log_ms;
    uint32_t jpeg_encode_fail_count;
    uint64_t next_jpeg_encode_fail_log_ms;
};

static const char *TAG = "camera_source";

static bool is_complete_jpeg(const uint8_t *data, uint32_t len)
{
    return data && len >= JPEG_MIN_SIZE &&
           data[0] == 0xFF && data[1] == 0xD8 &&
           data[len - 2] == 0xFF && data[len - 1] == 0xD9;
}

static bool normalize_mjpeg_frame(const uint8_t *data, uint32_t input_len,
                                  uint32_t *jpeg_len, uint32_t *trailing_padding,
                                  uint32_t *trailing_soi_offset)
{
    if (!jpeg_len || !trailing_padding || !trailing_soi_offset) {
        return false;
    }

    *jpeg_len = 0;
    *trailing_padding = 0;
    *trailing_soi_offset = UINT32_MAX;
    if (!data || input_len < JPEG_MIN_SIZE || data[0] != 0xFF || data[1] != 0xD8) {
        return false;
    }

    for (uint32_t i = 2; i < input_len; i++) {
        if (data[i - 1] == 0xFF && data[i] == 0xD9) {
            *jpeg_len = i + 1;
            *trailing_padding = input_len - *jpeg_len;
            for (uint32_t j = *jpeg_len; j + 1 < input_len; j++) {
                if (data[j] == 0xFF && data[j + 1] == 0xD8) {
                    *trailing_soi_offset = j;
                    break;
                }
            }
            return true;
        }
    }

    return false;
}

static bool is_raw_bayer_format(uint32_t pixfmt)
{
    return pixfmt == V4L2_PIX_FMT_SBGGR8 || pixfmt == V4L2_PIX_FMT_SGBRG8 ||
           pixfmt == V4L2_PIX_FMT_SGRBG8 || pixfmt == V4L2_PIX_FMT_SRGGB8 ||
           pixfmt == V4L2_PIX_FMT_SBGGR10 || pixfmt == V4L2_PIX_FMT_SGBRG10 ||
           pixfmt == V4L2_PIX_FMT_SGRBG10 || pixfmt == V4L2_PIX_FMT_SRGGB10 ||
           pixfmt == V4L2_PIX_FMT_SBGGR12 || pixfmt == V4L2_PIX_FMT_SGBRG12 ||
           pixfmt == V4L2_PIX_FMT_SGRBG12 || pixfmt == V4L2_PIX_FMT_SRGGB12;
}

static uint32_t get_buffer_count(camera_source_t *source)
{
    return source->config.kind == CAMERA_SOURCE_KIND_USB_UVC ?
           USB_VIDEO_BUFFER_NUMBER : CSI_VIDEO_BUFFER_NUMBER;
}

static void yuyv_to_uyvy_in_place(uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0; i + 3 < len; i += 4) {
        uint8_t y0 = data[i];
        uint8_t u = data[i + 1];
        uint8_t y1 = data[i + 2];
        uint8_t v = data[i + 3];
        data[i] = u;
        data[i + 1] = y0;
        data[i + 2] = v;
        data[i + 3] = y1;
    }
}

static void log_drop_limited(camera_source_t *source, uint32_t *count, uint64_t *next_log_ms,
                             const char *reason, uint32_t last_len)
{
    (*count)++;

    uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
    if (*next_log_ms == 0 || now_ms >= *next_log_ms) {
        ESP_LOGW(TAG, "cam%d: %s x%" PRIu32 " in last %dms, last_len=%" PRIu32,
                 source->config.camera_id, reason, *count, DROP_LOG_INTERVAL_MS, last_len);
        *count = 0;
        *next_log_ms = now_ms + DROP_LOG_INTERVAL_MS;
    }
}

static void log_mjpeg_padding_limited(camera_source_t *source, uint32_t padding_len)
{
    if (padding_len == 0) {
        return;
    }

    source->mjpeg_padding_trim_count++;
    source->mjpeg_padding_trim_bytes += padding_len;
    source->mjpeg_padding_trim_max = MAX(source->mjpeg_padding_trim_max, padding_len);

    uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
    if (source->next_mjpeg_padding_log_ms == 0 || now_ms >= source->next_mjpeg_padding_log_ms) {
        uint64_t average = source->mjpeg_padding_trim_bytes / source->mjpeg_padding_trim_count;
        ESP_LOGI(TAG, "cam%d: trimmed MJPEG trailing padding x%" PRIu32
                 " in last %dms, last=%" PRIu32 " avg=%" PRIu64 " max=%" PRIu32,
                 source->config.camera_id, source->mjpeg_padding_trim_count,
                 DROP_LOG_INTERVAL_MS, padding_len, average, source->mjpeg_padding_trim_max);
        source->mjpeg_padding_trim_count = 0;
        source->mjpeg_padding_trim_bytes = 0;
        source->mjpeg_padding_trim_max = 0;
        source->next_mjpeg_padding_log_ms = now_ms + DROP_LOG_INTERVAL_MS;
    }
}

static void log_mjpeg_boundary_merge_limited(camera_source_t *source,
                                              uint32_t soi_offset,
                                              uint32_t trailing_len)
{
    source->mjpeg_boundary_merge_count++;
    uint64_t now_ms = (uint64_t)(esp_timer_get_time() / 1000);
    if (source->next_mjpeg_boundary_merge_log_ms == 0 ||
            now_ms >= source->next_mjpeg_boundary_merge_log_ms) {
        ESP_LOGW(TAG, "cam%d: possible merged MJPEG boundary x%" PRIu32
                 " in last %dms, trailing_soi_offset=%" PRIu32 " trailing_len=%" PRIu32,
                 source->config.camera_id, source->mjpeg_boundary_merge_count,
                 DROP_LOG_INTERVAL_MS, soi_offset, trailing_len);
        source->mjpeg_boundary_merge_count = 0;
        source->next_mjpeg_boundary_merge_log_ms = now_ms + DROP_LOG_INTERVAL_MS;
    }
}

static esp_err_t set_v4l2_jpeg_quality(camera_source_t *source, uint8_t quality)
{
    struct v4l2_query_ext_ctrl qctrl = {
        .id = V4L2_CID_JPEG_COMPRESSION_QUALITY,
    };

    if (ioctl(source->fd, VIDIOC_QUERY_EXT_CTRL, &qctrl) != 0) {
        source->quality_supported = false;
        ESP_LOGW(TAG, "cam%d: JPEG quality control is not supported", source->config.camera_id);
        return ESP_ERR_NOT_SUPPORTED;
    }

    int quality_reset = quality;
    if (quality_reset > qctrl.maximum) {
        quality_reset = qctrl.maximum;
    } else if (quality_reset < qctrl.minimum) {
        quality_reset = qctrl.minimum;
    } else if (qctrl.step > 0 && ((quality_reset - qctrl.minimum) % qctrl.step) != 0) {
        quality_reset = qctrl.minimum + ((quality_reset - qctrl.minimum) / qctrl.step) * qctrl.step;
    }

    struct v4l2_ext_control control = {
        .id = V4L2_CID_JPEG_COMPRESSION_QUALITY,
        .value = quality_reset,
    };
    struct v4l2_ext_controls controls = {
        .ctrl_class = V4L2_CID_JPEG_CLASS,
        .count = 1,
        .controls = &control,
    };

    ESP_RETURN_ON_FALSE(ioctl(source->fd, VIDIOC_S_EXT_CTRLS, &controls) == 0,
                        ESP_FAIL, TAG, "cam%d: failed to set JPEG quality", source->config.camera_id);

    source->jpeg_quality = quality_reset;
    source->quality_supported = true;
    ESP_LOGI(TAG, "cam%d: set device JPEG quality to %d", source->config.camera_id, quality_reset);
    return ESP_OK;
}

static esp_err_t configure_format(camera_source_t *source)
{
    struct v4l2_format format = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
    };

    ESP_RETURN_ON_FALSE(ioctl(source->fd, VIDIOC_G_FMT, &format) == 0,
                        ESP_FAIL, TAG, "failed to get fmt from %s", source->config.dev_name);

    ESP_LOGI(TAG, "cam%d initial format " V4L2_FMT_STR " %" PRIu32 "x%" PRIu32,
             source->config.camera_id,
             V4L2_FMT_STR_ARG(format.fmt.pix.pixelformat),
             format.fmt.pix.width,
             format.fmt.pix.height);

    format.fmt.pix.width = source->config.width;
    format.fmt.pix.height = source->config.height;
    if (source->config.target_pixel_format) {
        format.fmt.pix.pixelformat = source->config.target_pixel_format;
    } else if (is_raw_bayer_format(format.fmt.pix.pixelformat)) {
        format.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
    }

    ESP_RETURN_ON_FALSE(ioctl(source->fd, VIDIOC_S_FMT, &format) == 0,
                        ESP_FAIL, TAG, "cam%d: failed to set target format", source->config.camera_id);
    ESP_RETURN_ON_FALSE(ioctl(source->fd, VIDIOC_G_FMT, &format) == 0,
                        ESP_FAIL, TAG, "cam%d: failed to read target format", source->config.camera_id);

    if (format.fmt.pix.width != source->config.width || format.fmt.pix.height != source->config.height) {
        ESP_LOGE(TAG, "cam%d: strict target resolution %" PRIu32 "x%" PRIu32 " not available, got %" PRIu32 "x%" PRIu32,
                 source->config.camera_id,
                 source->config.width,
                 source->config.height,
                 format.fmt.pix.width,
                 format.fmt.pix.height);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (source->config.target_frame_rate) {
        struct v4l2_streamparm sparm = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        };
        sparm.parm.capture.timeperframe.numerator = 1;
        sparm.parm.capture.timeperframe.denominator = source->config.target_frame_rate;
        ESP_RETURN_ON_FALSE(ioctl(source->fd, VIDIOC_S_PARM, &sparm) == 0,
                            ESP_ERR_NOT_SUPPORTED, TAG, "cam%d: target frame rate %" PRIu32 "fps is unavailable",
                            source->config.camera_id, source->config.target_frame_rate);
    }

    if (source->config.target_pixel_format &&
        format.fmt.pix.pixelformat != source->config.target_pixel_format) {
        ESP_LOGE(TAG, "cam%d: strict target format " V4L2_FMT_STR " not available, got " V4L2_FMT_STR,
                 source->config.camera_id,
                 V4L2_FMT_STR_ARG(source->config.target_pixel_format),
                 V4L2_FMT_STR_ARG(format.fmt.pix.pixelformat));
        return ESP_ERR_NOT_SUPPORTED;
    }

#if CONFIG_EXAMPLE_SELECT_JPEG_HW_DRIVER
    if (format.fmt.pix.pixelformat == V4L2_PIX_FMT_RGB565X) {
#if CONFIG_ESP_VIDEO_ENABLE_SWAP_BYTE
        format.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
        ESP_RETURN_ON_FALSE(ioctl(source->fd, VIDIOC_S_FMT, &format) == 0,
                            ESP_FAIL, TAG, "cam%d: failed to switch RGB565 endian", source->config.camera_id);
        ESP_RETURN_ON_FALSE(ioctl(source->fd, VIDIOC_G_FMT, &format) == 0,
                            ESP_FAIL, TAG, "cam%d: failed to re-read RGB565 endian", source->config.camera_id);
#else
        ESP_LOGE(TAG, "cam%d: hardware JPEG encoder does not support RGB565 big endian", source->config.camera_id);
        return ESP_ERR_NOT_SUPPORTED;
#endif
    }
#endif

    source->pixel_format = format.fmt.pix.pixelformat;

    struct v4l2_streamparm sparm = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
    };
    if (ioctl(source->fd, VIDIOC_G_PARM, &sparm) == 0) {
        struct v4l2_fract *timeperframe = &sparm.parm.capture.timeperframe;
        source->frame_rate = timeperframe->numerator ?
                             (timeperframe->denominator / timeperframe->numerator) : 0;
    }

    if (source->config.target_frame_rate && source->frame_rate != source->config.target_frame_rate) {
        ESP_LOGE(TAG, "cam%d: strict target frame rate %" PRIu32 "fps not available, got %" PRIu32 "fps",
                 source->config.camera_id, source->config.target_frame_rate, source->frame_rate);
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_LOGI(TAG, "cam%d target format " V4L2_FMT_STR " %" PRIu32 "x%" PRIu32 " @%" PRIu32 "fps",
             source->config.camera_id,
             V4L2_FMT_STR_ARG(source->pixel_format),
             source->config.width,
             source->config.height,
             source->frame_rate);
    return ESP_OK;
}

static esp_err_t setup_buffers(camera_source_t *source)
{
    uint32_t requested_count = get_buffer_count(source);
    struct v4l2_requestbuffers req = {
        .count = requested_count,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    ESP_RETURN_ON_FALSE(ioctl(source->fd, VIDIOC_REQBUFS, &req) == 0,
                        ESP_FAIL, TAG, "cam%d: failed to request buffers", source->config.camera_id);
    ESP_RETURN_ON_FALSE(req.count > 0 && req.count <= CAMERA_VIDEO_BUFFER_MAX,
                        ESP_ERR_INVALID_SIZE, TAG, "cam%d: invalid buffer count %" PRIu32,
                        source->config.camera_id, req.count);
    if (req.count < requested_count) {
        ESP_LOGW(TAG, "cam%d: requested %" PRIu32 " buffers, got %" PRIu32,
                 source->config.camera_id, requested_count, req.count);
    }
    source->buffer_count = req.count;

    for (uint32_t i = 0; i < source->buffer_count; i++) {
        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
            .index = i,
        };
        ESP_RETURN_ON_FALSE(ioctl(source->fd, VIDIOC_QUERYBUF, &buf) == 0,
                            ESP_FAIL, TAG, "cam%d: failed to query buffer", source->config.camera_id);

        source->buffer[i] = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, source->fd, buf.m.offset);
        ESP_RETURN_ON_FALSE(source->buffer[i] != MAP_FAILED,
                            ESP_ERR_NO_MEM, TAG, "cam%d: failed to mmap buffer", source->config.camera_id);
        source->buffer_size[i] = buf.length;

        ESP_RETURN_ON_FALSE(ioctl(source->fd, VIDIOC_QBUF, &buf) == 0,
                            ESP_FAIL, TAG, "cam%d: failed to queue buffer", source->config.camera_id);
    }

    return ESP_OK;
}

static void cleanup_buffers(camera_source_t *source)
{
    if (!source) {
        return;
    }

    for (uint32_t i = 0; i < source->buffer_count; i++) {
        if (source->buffer[i] && source->buffer[i] != MAP_FAILED && source->buffer_size[i] > 0) {
            munmap(source->buffer[i], source->buffer_size[i]);
            source->buffer[i] = NULL;
            source->buffer_size[i] = 0;
        }
    }
}

static void cleanup_encoder(camera_source_t *source)
{
    if (!source) {
        return;
    }

    if (source->encoder_lock) {
        vSemaphoreDelete(source->encoder_lock);
        source->encoder_lock = NULL;
    }

    if (source->encoder_handle) {
        if (source->jpeg_out_buf) {
            example_encoder_free_output_buffer(source->encoder_handle, source->jpeg_out_buf);
            source->jpeg_out_buf = NULL;
            source->jpeg_out_size = 0;
        }
        example_encoder_deinit(source->encoder_handle);
        source->encoder_handle = NULL;
    }
}

static esp_err_t setup_encoder(camera_source_t *source)
{
    source->jpeg_quality = source->config.jpeg_quality;

    if (source->pixel_format == V4L2_PIX_FMT_JPEG) {
        esp_err_t ret = set_v4l2_jpeg_quality(source, source->config.jpeg_quality);
        if (ret == ESP_ERR_NOT_SUPPORTED) {
            source->jpeg_quality = source->config.jpeg_quality;
            return ESP_OK;
        }
        return ret;
    }

    example_encoder_config_t encoder_config = {
        .width = source->config.width,
        .height = source->config.height,
        .pixel_format = source->pixel_format == V4L2_PIX_FMT_YUYV ? V4L2_PIX_FMT_UYVY : source->pixel_format,
        .quality = source->config.jpeg_quality,
    };
    ESP_RETURN_ON_ERROR(example_encoder_init(&encoder_config, &source->encoder_handle),
                        TAG, "cam%d: failed to init JPEG encoder", source->config.camera_id);
    ESP_RETURN_ON_ERROR(example_encoder_alloc_output_buffer(source->encoder_handle,
                                                           &source->jpeg_out_buf,
                                                           &source->jpeg_out_size),
                        TAG, "cam%d: failed to allocate JPEG output", source->config.camera_id);
    source->quality_supported = true;

    source->encoder_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(source->encoder_lock, ESP_ERR_NO_MEM, TAG, "cam%d: failed to create encoder lock", source->config.camera_id);
    return ESP_OK;
}

static void capture_task(void *arg)
{
    camera_source_t *source = (camera_source_t *)arg;
    uint64_t last_send_ms = 0;
    uint64_t stats_start_ms = 0;
    uint32_t captured_frames = 0;
    uint32_t dqbuf_failures = 0;
    const uint32_t send_interval_ms = source->config.kind == CAMERA_SOURCE_KIND_CSI ?
                                      CSI_WS_SEND_INTERVAL_MS : USB_WS_SEND_INTERVAL_MS;

    while (!__atomic_load_n(&source->stop_requested, __ATOMIC_ACQUIRE)) {
        struct v4l2_buffer buf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP,
        };

        if (ioctl(source->fd, VIDIOC_DQBUF, &buf) != 0) {
            if (source->config.kind == CAMERA_SOURCE_KIND_USB_UVC &&
                ++dqbuf_failures >= USB_DQBUF_FAILURE_LIMIT) {
                ESP_LOGW(TAG, "cam%d: no USB frames for %dms, marking source offline",
                         source->config.camera_id,
                         USB_DQBUF_TIMEOUT_MS * USB_DQBUF_FAILURE_LIMIT);
                __atomic_store_n(&source->active, false, __ATOMIC_RELEASE);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        dqbuf_failures = 0;

        if (!(buf.flags & V4L2_BUF_FLAG_DONE) || buf.index >= source->buffer_count) {
            ioctl(source->fd, VIDIOC_QBUF, &buf);
            continue;
        }

        esp_cache_msync(source->buffer[buf.index], source->buffer_size[buf.index],
                        ESP_CACHE_MSYNC_FLAG_DIR_M2C |
                        ESP_CACHE_MSYNC_FLAG_TYPE_DATA |
                        ESP_CACHE_MSYNC_FLAG_INVALIDATE);

        uint64_t timestamp_ms = (uint64_t)(esp_timer_get_time() / 1000);
        if (source->config.kind == CAMERA_SOURCE_KIND_USB_UVC) {
            if (stats_start_ms == 0) {
                stats_start_ms = timestamp_ms;
            }
            captured_frames++;
            uint64_t stats_elapsed_ms = timestamp_ms - stats_start_ms;
            if (stats_elapsed_ms >= CAPTURE_STATS_WINDOW_MS) {
                uint32_t fps_x100 = (uint32_t)(((uint64_t)captured_frames * 100000) / stats_elapsed_ms);
                ESP_LOGI(TAG, "cam%d: USB capture %" PRIu32 ".%02" PRIu32 "fps (%" PRIu32 " frames/%" PRIu64 "ms)",
                         source->config.camera_id,
                         fps_x100 / 100, fps_x100 % 100,
                         captured_frames, stats_elapsed_ms);
                stats_start_ms = timestamp_ms;
                captured_frames = 0;
            }
        }
        if (last_send_ms != 0 && (timestamp_ms - last_send_ms) < send_interval_ms) {
            ioctl(source->fd, VIDIOC_QBUF, &buf);
            continue;
        }

        const uint8_t *jpeg_src = NULL;
        uint32_t jpeg_len = 0;

        if (source->pixel_format == V4L2_PIX_FMT_JPEG) {
            uint8_t *mjpeg_data = source->buffer[buf.index];
            jpeg_src = mjpeg_data;
            uint32_t input_len = buf.bytesused;
            uint32_t trailing_padding = 0;
            uint32_t trailing_soi_offset = UINT32_MAX;
            if (input_len > source->buffer_size[buf.index] || input_len < JPEG_MIN_SIZE ||
                    mjpeg_data[0] != 0xFF || mjpeg_data[1] != 0xD8) {
                log_drop_limited(source,
                                 &source->incomplete_mjpeg_drop_count,
                                 &source->next_incomplete_mjpeg_log_ms,
                                 "drop incomplete MJPEG frame",
                                 input_len);
                ioctl(source->fd, VIDIOC_QBUF, &buf);
                continue;
            }

            if (normalize_mjpeg_frame(mjpeg_data, input_len, &jpeg_len,
                                      &trailing_padding, &trailing_soi_offset)) {
                if (trailing_soi_offset != UINT32_MAX) {
                    log_mjpeg_boundary_merge_limited(source, trailing_soi_offset,
                                                      trailing_padding);
                }
                log_mjpeg_padding_limited(source, trailing_padding);
            } else {
                log_drop_limited(source,
                                 &source->incomplete_mjpeg_drop_count,
                                 &source->next_incomplete_mjpeg_log_ms,
                                 "drop MJPEG frame without EOI",
                                 input_len);
                ioctl(source->fd, VIDIOC_QBUF, &buf);
                continue;
            }
        } else {
            uint32_t source_size = source->buffer_size[buf.index];
            if (source->pixel_format == V4L2_PIX_FMT_YUYV) {
                uint32_t expected_size = source->config.width * source->config.height * 2;
                if (buf.bytesused < expected_size || source->buffer_size[buf.index] < expected_size) {
                    log_drop_limited(source,
                                     &source->jpeg_encode_fail_count,
                                     &source->next_jpeg_encode_fail_log_ms,
                                     "short YUYV frame",
                                     buf.bytesused);
                    ioctl(source->fd, VIDIOC_QBUF, &buf);
                    continue;
                }
                source_size = expected_size;
                yuyv_to_uyvy_in_place(source->buffer[buf.index], source_size);
                esp_cache_msync(source->buffer[buf.index], source_size,
                                ESP_CACHE_MSYNC_FLAG_DIR_C2M |
                                ESP_CACHE_MSYNC_FLAG_TYPE_DATA |
                                ESP_CACHE_MSYNC_FLAG_UNALIGNED);
            }

            if (xSemaphoreTake(source->encoder_lock, pdMS_TO_TICKS(100)) != pdPASS) {
                ioctl(source->fd, VIDIOC_QBUF, &buf);
                continue;
            }

            esp_err_t ret = example_encoder_process(source->encoder_handle,
                                                    source->buffer[buf.index],
                                                    source_size,
                                                    source->jpeg_out_buf,
                                                    source->jpeg_out_size,
                                                    &jpeg_len);
            xSemaphoreGive(source->encoder_lock);

            if (ret != ESP_OK || jpeg_len < JPEG_MIN_SIZE ||
                    jpeg_len > source->jpeg_out_size ||
                    !is_complete_jpeg(source->jpeg_out_buf, jpeg_len)) {
                log_drop_limited(source,
                                 &source->jpeg_encode_fail_count,
                                 &source->next_jpeg_encode_fail_log_ms,
                                 "JPEG encode failed",
                                 jpeg_len);
                ioctl(source->fd, VIDIOC_QBUF, &buf);
                continue;
            }
            jpeg_src = source->jpeg_out_buf;
        }

        camera_source_frame_t frame = {
            .camera_id = source->config.camera_id,
            .timestamp_ms = timestamp_ms,
            .jpeg_data = jpeg_src,
            .jpeg_len = jpeg_len,
            .width = source->config.width,
            .height = source->config.height,
        };
        if (source->config.frame_cb) {
            source->config.frame_cb(&frame, source->config.user_ctx);
        }
        last_send_ms = timestamp_ms;

        ioctl(source->fd, VIDIOC_QBUF, &buf);
    }

    __atomic_store_n(&source->active, false, __ATOMIC_RELEASE);
    __atomic_store_n(&source->task, NULL, __ATOMIC_RELEASE);
    vTaskDelete(NULL);
}

esp_err_t camera_source_create(const camera_source_config_t *config, camera_source_t **out_source)
{
    ESP_RETURN_ON_FALSE(config && out_source && config->dev_name, ESP_ERR_INVALID_ARG, TAG, "invalid arguments");
    esp_err_t ret = ESP_OK;

    camera_source_t *source = heap_caps_calloc(1, sizeof(camera_source_t), MALLOC_CAP_8BIT);
    ESP_RETURN_ON_FALSE(source, ESP_ERR_NO_MEM, TAG, "failed to allocate camera source");
    source->config = *config;
    source->fd = -1;

    int retries = source->config.kind == CAMERA_SOURCE_KIND_USB_UVC ? USB_OPEN_RETRIES : 1;
    for (int i = 0; i < retries; i++) {
        source->fd = open(source->config.dev_name, O_RDWR);
        if (source->fd >= 0) {
            break;
        }
        if (source->config.kind == CAMERA_SOURCE_KIND_USB_UVC) {
            ESP_LOGW(TAG, "cam%d: open %s failed (%d/%d), retrying",
                     source->config.camera_id, source->config.dev_name, i + 1, retries);
            vTaskDelay(pdMS_TO_TICKS(USB_OPEN_RETRY_DELAY_MS));
        }
    }
    ESP_GOTO_ON_FALSE(source->fd >= 0, ESP_ERR_NOT_FOUND, fail, TAG,
                      "cam%d: open %s failed", source->config.camera_id, source->config.dev_name);

    ESP_GOTO_ON_ERROR(configure_format(source), fail, TAG, "cam%d: format setup failed", source->config.camera_id);
    ESP_GOTO_ON_ERROR(setup_buffers(source), fail, TAG, "cam%d: buffer setup failed", source->config.camera_id);
    ESP_GOTO_ON_ERROR(setup_encoder(source), fail, TAG, "cam%d: encoder setup failed", source->config.camera_id);

    if (source->config.kind == CAMERA_SOURCE_KIND_USB_UVC) {
        struct timeval timeout = {
            .tv_sec = USB_DQBUF_TIMEOUT_MS / 1000,
            .tv_usec = (USB_DQBUF_TIMEOUT_MS % 1000) * 1000,
        };
        ESP_GOTO_ON_FALSE(ioctl(source->fd, VIDIOC_S_DQBUF_TIMEOUT, &timeout) == 0,
                          ESP_FAIL, fail, TAG, "cam%d: failed to set dequeue timeout",
                          source->config.camera_id);
    }

    *out_source = source;
    return ESP_OK;

fail:
    cleanup_encoder(source);
    cleanup_buffers(source);
    if (source->fd >= 0) {
        close(source->fd);
    }
    heap_caps_free(source);
    return ret;
}

esp_err_t camera_source_start(camera_source_t *source)
{
    ESP_RETURN_ON_FALSE(source && source->fd >= 0, ESP_ERR_INVALID_ARG, TAG, "invalid camera source");

    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ESP_RETURN_ON_FALSE(ioctl(source->fd, VIDIOC_STREAMON, &type) == 0,
                        ESP_FAIL, TAG, "cam%d: failed to stream on", source->config.camera_id);

    char task_name[16];
    snprintf(task_name, sizeof(task_name), "cam_src_%d", source->config.camera_id);
    TaskHandle_t task = NULL;
    __atomic_store_n(&source->stop_requested, false, __ATOMIC_RELEASE);
    BaseType_t ok = xTaskCreatePinnedToCore(capture_task, task_name, 1024 * 7,
                                            source, 6, &task, CAMERA_PROCESSING_CORE);
    if (ok != pdPASS) {
        ioctl(source->fd, VIDIOC_STREAMOFF, &type);
        ESP_LOGE(TAG, "cam%d: failed to create capture task", source->config.camera_id);
        return ESP_ERR_NO_MEM;
    }
    __atomic_store_n(&source->task, task, __ATOMIC_RELEASE);
    __atomic_store_n(&source->active, true, __ATOMIC_RELEASE);

    ESP_LOGI(TAG, "cam%d: started %s %" PRIu32 "x%" PRIu32,
             source->config.camera_id,
             source->config.kind == CAMERA_SOURCE_KIND_CSI ? "CSI" : "USB",
             source->config.width,
             source->config.height);
    return ESP_OK;
}

esp_err_t camera_source_set_jpeg_quality(camera_source_t *source, uint8_t quality)
{
    ESP_RETURN_ON_FALSE(source, ESP_ERR_INVALID_ARG, TAG, "invalid camera source");

    if (source->pixel_format == V4L2_PIX_FMT_JPEG) {
        return set_v4l2_jpeg_quality(source, quality);
    }

    ESP_RETURN_ON_FALSE(source->encoder_handle, ESP_ERR_INVALID_STATE, TAG, "cam%d: encoder not available", source->config.camera_id);
    ESP_RETURN_ON_ERROR(example_encoder_set_jpeg_quality(source->encoder_handle, quality),
                        TAG, "cam%d: failed to set encoder quality", source->config.camera_id);
    source->jpeg_quality = quality;
    source->quality_supported = true;
    ESP_LOGI(TAG, "cam%d: set encoder JPEG quality to %u", source->config.camera_id, quality);
    return ESP_OK;
}

void camera_source_get_info(camera_source_t *source, camera_source_info_t *info)
{
    if (!info) {
        return;
    }
    memset(info, 0, sizeof(*info));
    if (!source) {
        return;
    }

    info->camera_id = source->config.camera_id;
    info->kind = source->config.kind;
    info->active = __atomic_load_n(&source->active, __ATOMIC_ACQUIRE);
    info->width = source->config.width;
    info->height = source->config.height;
    info->frame_rate = source->frame_rate;
    info->pixel_format = source->pixel_format;
    info->jpeg_quality = source->jpeg_quality;
    info->quality_supported = source->quality_supported;
}

bool camera_source_is_active(camera_source_t *source)
{
    return source && __atomic_load_n(&source->active, __ATOMIC_ACQUIRE);
}

void camera_source_destroy(camera_source_t *source)
{
    if (!source) {
        return;
    }

    __atomic_store_n(&source->stop_requested, true, __ATOMIC_RELEASE);
    __atomic_store_n(&source->active, false, __ATOMIC_RELEASE);

    for (int i = 0; i < 120 && __atomic_load_n(&source->task, __ATOMIC_ACQUIRE); i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    bool stream_off_called = false;
    if (__atomic_load_n(&source->task, __ATOMIC_ACQUIRE) && source->fd >= 0) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(source->fd, VIDIOC_STREAMOFF, &type);
        stream_off_called = true;
    }

    for (int i = 0; i < 30 && __atomic_load_n(&source->task, __ATOMIC_ACQUIRE); i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    TaskHandle_t task = __atomic_exchange_n(&source->task, NULL, __ATOMIC_ACQ_REL);
    if (task) {
        vTaskDelete(task);
    }

    if (source->fd >= 0) {
        if (!stream_off_called) {
            int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            ioctl(source->fd, VIDIOC_STREAMOFF, &type);
        }
        close(source->fd);
        source->fd = -1;
    }

    cleanup_encoder(source);
    cleanup_buffers(source);
    heap_caps_free(source);
}
