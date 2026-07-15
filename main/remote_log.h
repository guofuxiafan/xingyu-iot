#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define REMOTE_LOG_UDP_PORT 8766

esp_err_t remote_log_start(const char *host_ip, int port);

#ifdef __cplusplus
}
#endif
