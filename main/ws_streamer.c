#include "ws_streamer.h"

#include <inttypes.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_cache.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"

#define WS_POOL_NODES 3
#define WS_QUEUE_LEN  3
#define WS_PROCESSING_CORE 1

typedef struct {
    uint8_t data[WS_STREAMER_FRAME_HEADER_SIZE + WS_STREAMER_MAX_JPEG_SIZE];
    uint32_t payload_len;
    uint8_t in_use;
} __attribute__((aligned(64))) ws_frame_node_t;

static const char *TAG = "ws_streamer";

static esp_websocket_client_handle_t s_ws_client;
static SemaphoreHandle_t s_send_mutex;
static SemaphoreHandle_t s_pool_lock;
static QueueHandle_t s_queue;
static TaskHandle_t s_send_task;
static ws_frame_node_t *s_pool;
static volatile uint32_t s_pool_drop_count;
static volatile uint32_t s_queue_drop_count;
static volatile int64_t s_next_drop_log_us;

static void log_submit_drop_limited(bool pool_empty)
{
    if (pool_empty) {
        __atomic_fetch_add(&s_pool_drop_count, 1, __ATOMIC_RELAXED);
    } else {
        __atomic_fetch_add(&s_queue_drop_count, 1, __ATOMIC_RELAXED);
    }

    int64_t now_us = esp_timer_get_time();
    int64_t next_us = __atomic_load_n(&s_next_drop_log_us, __ATOMIC_RELAXED);
    if (now_us < next_us ||
            !__atomic_compare_exchange_n(&s_next_drop_log_us, &next_us,
                                         now_us + 5000000, false,
                                         __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
        return;
    }

    uint32_t pool_drops = __atomic_exchange_n(&s_pool_drop_count, 0, __ATOMIC_RELAXED);
    uint32_t queue_drops = __atomic_exchange_n(&s_queue_drop_count, 0, __ATOMIC_RELAXED);
    ESP_LOGW(TAG, "submit drops/5000ms: pool_empty=%" PRIu32 " queue_full=%" PRIu32,
             pool_drops, queue_drops);
}

static ws_frame_node_t *grab_pool_node(void)
{
    if (!s_pool || xSemaphoreTake(s_pool_lock, pdMS_TO_TICKS(20)) != pdPASS) {
        return NULL;
    }

    for (int i = 0; i < WS_POOL_NODES; i++) {
        if (!s_pool[i].in_use) {
            s_pool[i].in_use = 1;
            xSemaphoreGive(s_pool_lock);
            return &s_pool[i];
        }
    }

    xSemaphoreGive(s_pool_lock);
    return NULL;
}

static void release_pool_node(ws_frame_node_t *node)
{
    if (!node || !s_pool_lock) {
        return;
    }

    if (xSemaphoreTake(s_pool_lock, pdMS_TO_TICKS(20)) == pdPASS) {
        node->in_use = 0;
        xSemaphoreGive(s_pool_lock);
    }
}

static void ws_send_task(void *arg)
{
    (void)arg;
    ws_frame_node_t *node;

    while (1) {
        if (xQueueReceive(s_queue, &node, portMAX_DELAY) != pdPASS) {
            continue;
        }

        esp_cache_msync(node, (sizeof(ws_frame_node_t) + 63) & ~63UL,
                        ESP_CACHE_MSYNC_FLAG_DIR_M2C |
                        ESP_CACHE_MSYNC_FLAG_TYPE_DATA |
                        ESP_CACHE_MSYNC_FLAG_INVALIDATE);

        if (s_ws_client && esp_websocket_client_is_connected(s_ws_client)) {
            if (xSemaphoreTake(s_send_mutex, pdMS_TO_TICKS(500)) == pdPASS) {
                int sent = esp_websocket_client_send_bin(s_ws_client,
                                                         (const char *)node->data,
                                                         node->payload_len,
                                                         pdMS_TO_TICKS(2000));
                if (sent < 0) {
                    ESP_LOGW(TAG, "WebSocket send failed: %d", sent);
                } else if ((uint32_t)sent != node->payload_len) {
                    ESP_LOGW(TAG, "WebSocket partial send %d/%" PRIu32, sent, node->payload_len);
                }
                xSemaphoreGive(s_send_mutex);
            }
        }

        release_pool_node(node);
    }
}

esp_err_t ws_streamer_start(const char *url)
{
    if (url == NULL || url[0] == '\0') {
        ESP_LOGI(TAG, "WebSocket disabled (no PC IP configured in NVS)");
        return ESP_OK;
    }

    if (!s_pool) {
        s_pool = heap_caps_aligned_alloc(64, sizeof(ws_frame_node_t) * WS_POOL_NODES, MALLOC_CAP_SPIRAM);
        if (!s_pool) {
            s_pool = heap_caps_aligned_alloc(64, sizeof(ws_frame_node_t) * WS_POOL_NODES, MALLOC_CAP_8BIT);
        }
        ESP_RETURN_ON_FALSE(s_pool, ESP_ERR_NO_MEM, TAG, "failed to allocate WebSocket frame pool");
        memset(s_pool, 0, sizeof(ws_frame_node_t) * WS_POOL_NODES);
    }

    if (!s_pool_lock) {
        s_pool_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_pool_lock, ESP_ERR_NO_MEM, TAG, "failed to create pool mutex");
    }
    if (!s_send_mutex) {
        s_send_mutex = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_send_mutex, ESP_ERR_NO_MEM, TAG, "failed to create send mutex");
    }
    if (!s_queue) {
        s_queue = xQueueCreate(WS_QUEUE_LEN, sizeof(ws_frame_node_t *));
        ESP_RETURN_ON_FALSE(s_queue, ESP_ERR_NO_MEM, TAG, "failed to create WebSocket queue");
    }
    if (!s_send_task) {
        BaseType_t ok = xTaskCreatePinnedToCore(ws_send_task, "ws_send", 1024 * 6,
                                                NULL, 5, &s_send_task,
                                                WS_PROCESSING_CORE);
        ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "failed to create WebSocket send task");
    }

    esp_websocket_client_config_t ws_cfg = {
        .uri = url,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 5000,
        .disable_auto_reconnect = false,
    };
    s_ws_client = esp_websocket_client_init(&ws_cfg);
    ESP_RETURN_ON_FALSE(s_ws_client, ESP_FAIL, TAG, "failed to init WebSocket client");

    ESP_LOGI(TAG, "WebSocket connecting to %s", url);
    return esp_websocket_client_start(s_ws_client);
}

esp_err_t ws_streamer_submit(const ws_streamer_frame_t *frame)
{
    if (!frame || !frame->jpeg_data || !frame->jpeg_len) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_ws_client || !esp_websocket_client_is_connected(s_ws_client)) {
        return ESP_ERR_INVALID_STATE;
    }

    if (frame->jpeg_len > WS_STREAMER_MAX_JPEG_SIZE) {
        ESP_LOGW(TAG, "cam%d JPEG %" PRIu32 " exceeds %" PRIu32 ", dropping complete frame",
                 frame->camera_id, frame->jpeg_len, (uint32_t)WS_STREAMER_MAX_JPEG_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }

    ws_frame_node_t *node = grab_pool_node();
    if (!node) {
        log_submit_drop_limited(true);
        return ESP_ERR_NO_MEM;
    }

    uint32_t data_len = frame->jpeg_len;
    uint64_t ts = frame->timestamp_ms;
    node->data[0] = (ts >> 0) & 0xFF;
    node->data[1] = (ts >> 8) & 0xFF;
    node->data[2] = (ts >> 16) & 0xFF;
    node->data[3] = (ts >> 24) & 0xFF;
    node->data[4] = (ts >> 32) & 0xFF;
    node->data[5] = (ts >> 40) & 0xFF;
    node->data[6] = (ts >> 48) & 0xFF;
    node->data[7] = (ts >> 56) & 0xFF;
    node->data[8] = frame->camera_id;
    node->data[9] = (data_len >> 24) & 0xFF;
    node->data[10] = (data_len >> 16) & 0xFF;
    node->data[11] = (data_len >> 8) & 0xFF;
    node->data[12] = data_len & 0xFF;
    memcpy(node->data + WS_STREAMER_FRAME_HEADER_SIZE, frame->jpeg_data, data_len);
    node->payload_len = WS_STREAMER_FRAME_HEADER_SIZE + data_len;

    esp_cache_msync(node, (sizeof(ws_frame_node_t) + 63) & ~63UL,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);

    if (xQueueSend(s_queue, &node, 0) != pdPASS) {
        release_pool_node(node);
        log_submit_drop_limited(false);
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}
