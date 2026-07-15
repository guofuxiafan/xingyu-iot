/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "uvc_types_priv.h"

esp_err_t uvc_isoc_diagnostics_start(uvc_stream_t *uvc_stream);
void uvc_isoc_diagnostics_stop(uvc_stream_t *uvc_stream);
