#include "remote_log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#define REMOTE_LOG_QUEUE_LEN 64
#define REMOTE_LOG_LINE_LEN 256

typedef struct {
    char text[REMOTE_LOG_LINE_LEN];
} remote_log_line_t;

static QueueHandle_t s_log_queue;
static vprintf_like_t s_original_vprintf;
static char s_host_ip[16];
static int s_host_port;

static int remote_log_vprintf(const char *fmt, va_list args)
{
    va_list args_for_serial;
    va_copy(args_for_serial, args);
    int ret = s_original_vprintf ? s_original_vprintf(fmt, args_for_serial) : vprintf(fmt, args_for_serial);
    va_end(args_for_serial);

    QueueHandle_t queue = s_log_queue;
    if (queue) {
        remote_log_line_t line;
        va_list args_for_udp;

        va_copy(args_for_udp, args);
        vsnprintf(line.text, sizeof(line.text), fmt, args_for_udp);
        va_end(args_for_udp);

        (void)xQueueSend(queue, &line, 0);
    }

    return ret;
}

static void remote_log_task(void *arg)
{
    (void)arg;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in dest = {0};
    dest.sin_family = AF_INET;
    dest.sin_port = htons((uint16_t)s_host_port);
    if (inet_pton(AF_INET, s_host_ip, &dest.sin_addr) != 1) {
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    remote_log_line_t line;
    while (1) {
        if (xQueueReceive(s_log_queue, &line, portMAX_DELAY) == pdPASS) {
            size_t len = strnlen(line.text, sizeof(line.text));
            if (len > 0) {
                (void)sendto(sock, line.text, len, 0, (struct sockaddr *)&dest, sizeof(dest));
            }
        }
    }
}

esp_err_t remote_log_start(const char *host_ip, int port)
{
    if (host_ip == NULL || host_ip[0] == '\0' || port <= 0 || port > 65535) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_log_queue) {
        return ESP_OK;
    }

    snprintf(s_host_ip, sizeof(s_host_ip), "%s", host_ip);
    s_host_port = port;

    s_log_queue = xQueueCreate(REMOTE_LOG_QUEUE_LEN, sizeof(remote_log_line_t));
    if (!s_log_queue) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(remote_log_task, "remote_log", 4096, NULL, 4, NULL) != pdPASS) {
        vQueueDelete(s_log_queue);
        s_log_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_original_vprintf = esp_log_set_vprintf(remote_log_vprintf);
    return ESP_OK;
}
