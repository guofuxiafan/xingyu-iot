/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <inttypes.h>
#include <string.h> // For memcpy

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "uvc_stream.h" // For uvc_host_stream_pause()
#include "uvc_types_priv.h"
#include "uvc_check_priv.h"
#include "uvc_frame_priv.h"
#include "uvc_critical_priv.h"
#include "uvc_isoc_priv.h"

static const char *TAG = "uvc-isoc";

#define UVC_ISOC_LOG_INTERVAL_MS 1000

static inline void isoc_counter_increment(volatile uint32_t *counter)
{
    __atomic_fetch_add(counter, 1, __ATOMIC_RELAXED);
}

static uint32_t isoc_counter_take(volatile uint32_t *counter)
{
    return __atomic_exchange_n(counter, 0, __ATOMIC_RELAXED);
}

static void isoc_diagnostics_task(void *arg)
{
    uvc_stream_t *uvc_stream = (uvc_stream_t *)arg;

    while (!__atomic_load_n(&uvc_stream->diagnostics.stop, __ATOMIC_ACQUIRE)) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(UVC_ISOC_LOG_INTERVAL_MS));
        if (__atomic_load_n(&uvc_stream->diagnostics.stop, __ATOMIC_ACQUIRE)) {
            break;
        }

        uint32_t packet_error = isoc_counter_take(&uvc_stream->diagnostics.packet_error);
        uint32_t packet_lost = isoc_counter_take(&uvc_stream->diagnostics.packet_lost);
        uint32_t invalid_header = isoc_counter_take(&uvc_stream->diagnostics.invalid_header);
        uint32_t frame_error = isoc_counter_take(&uvc_stream->diagnostics.frame_error);
        uint32_t invalid_soi = isoc_counter_take(&uvc_stream->diagnostics.invalid_soi);
        uint32_t missed_eof = isoc_counter_take(&uvc_stream->diagnostics.missed_eof);
        uint32_t buffer_underflow = isoc_counter_take(&uvc_stream->diagnostics.buffer_underflow);
        uint32_t buffer_overflow = isoc_counter_take(&uvc_stream->diagnostics.buffer_overflow);

        if (packet_error || packet_lost || invalid_header || frame_error || invalid_soi || missed_eof ||
            buffer_underflow || buffer_overflow) {
            ESP_LOGW(TAG,
                     "stream=%p if=%u stats/%dms: err=%" PRIu32 " lost=%" PRIu32
                     " header=%" PRIu32 " frame=%" PRIu32 " soi=%" PRIu32 " eof=%" PRIu32
                     " under=%" PRIu32 " over=%" PRIu32,
                     uvc_stream, uvc_stream->constant.bInterfaceNumber, UVC_ISOC_LOG_INTERVAL_MS,
                     packet_error, packet_lost, invalid_header, frame_error, invalid_soi, missed_eof,
                     buffer_underflow, buffer_overflow);
        }
    }

    __atomic_store_n(&uvc_stream->diagnostics.task, NULL, __ATOMIC_RELEASE);
    vTaskDelete(NULL);
}

esp_err_t uvc_isoc_diagnostics_start(uvc_stream_t *uvc_stream)
{
    if (!uvc_stream || uvc_stream->diagnostics.task) {
        return uvc_stream ? ESP_OK : ESP_ERR_INVALID_ARG;
    }

    __atomic_store_n(&uvc_stream->diagnostics.stop, false, __ATOMIC_RELEASE);
    BaseType_t result = xTaskCreate(isoc_diagnostics_task, "uvc_isoc_diag", 3072,
                                    uvc_stream, 3, &uvc_stream->diagnostics.task);
    return result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void uvc_isoc_diagnostics_stop(uvc_stream_t *uvc_stream)
{
    if (!uvc_stream) {
        return;
    }

    __atomic_store_n(&uvc_stream->diagnostics.stop, true, __ATOMIC_RELEASE);
    TaskHandle_t task = __atomic_load_n(&uvc_stream->diagnostics.task, __ATOMIC_ACQUIRE);
    if (task) {
        xTaskNotifyGive(task);
        while (__atomic_load_n(&uvc_stream->diagnostics.task, __ATOMIC_ACQUIRE)) {
            vTaskDelay(1);
        }
    }
}

static void isoc_drop_current_frame(uvc_stream_t *uvc_stream)
{
    uvc_host_frame_t *dropped_frame = NULL;

    UVC_ENTER_CRITICAL();
    dropped_frame = uvc_stream->dynamic.current_frame;
    uvc_stream->dynamic.current_frame = NULL;
    uvc_stream->single_thread.skip_current_frame = true;
    UVC_EXIT_CRITICAL();

    if (dropped_frame) {
        uvc_host_frame_return(uvc_stream, dropped_frame);
    }
}

/**
 * @brief Callback function for handling Isochronous USB transfers from a UVC camera.
 *
 * This function processes isochronous transfer packets, which may contain video frame data. The following key points
 * are handled in the transfer:
 *
 * - **Start of Frame (SoF)**: Detected by a change in Frame ID, which toggles between 0 and 1.
 * - **End of Frame (EoF)**: Signaled in the packet header.
 * - **Transfer Characteristics**:
 *   - **No CRC**: Data corruption is possible.
 *   - **No ACK**: Packets can be missed.
 *   - **Packet Header**: Each packet includes a header used to detect errors, missed packets, and other issues.
 *
 * The callback performs the following tasks:
 * 1. Checks the status of each isochronous packet and handles various USB transfer statuses (e.g., completed,
 *    error, device disconnected).
 * 2. Parses packet headers to detect the start of new frames, handles errors, and manages frame buffers.
 * 3. Aggregates valid data into a frame buffer, ensuring no buffer overflow occurs.
 * 4. Signals the end of a frame and invokes user-defined callbacks if necessary.
 *
 * @param[in] transfer Pointer to the completed USB transfer structure.
 */
void isoc_transfer_callback(usb_transfer_t *transfer)
{
    uvc_stream_t *uvc_stream = (uvc_stream_t *)transfer->context;

    // USB_TRANSFER_STATUS_NO_DEVICE is set in transfer->status.
    // Other error codes are saved in status of each ISOC packet descriptor
    if (transfer->status == USB_TRANSFER_STATUS_NO_DEVICE) {
        (void)uvc_host_stream_pause(uvc_stream);
    }

    if (!UVC_ATOMIC_LOAD(uvc_stream->dynamic.streaming)) {
        return; // If the streaming was turned off, we don't have to do anything
    }

    const uint8_t *payload = transfer->data_buffer;
    for (int i = 0; i < transfer->num_isoc_packets; i++) {
        usb_isoc_packet_desc_t *isoc_desc = &transfer->isoc_packet_desc[i];

        // Check USB status
        switch (isoc_desc->status) {
        case USB_TRANSFER_STATUS_COMPLETED:
            break;
        case USB_TRANSFER_STATUS_NO_DEVICE:
        case USB_TRANSFER_STATUS_CANCELED:
            (void)uvc_host_stream_pause(uvc_stream);
            return; // No need to process the rest
        case USB_TRANSFER_STATUS_ERROR:
        case USB_TRANSFER_STATUS_OVERFLOW:
        case USB_TRANSFER_STATUS_STALL:
            isoc_counter_increment(&uvc_stream->diagnostics.packet_error);
            isoc_drop_current_frame(uvc_stream);
            goto next_isoc_packet; // Data corrupted

        case USB_TRANSFER_STATUS_TIMED_OUT:
        case USB_TRANSFER_STATUS_SKIPPED:
            isoc_counter_increment(&uvc_stream->diagnostics.packet_lost);
            isoc_drop_current_frame(uvc_stream);
            goto next_isoc_packet; // Missing ISOC packets corrupt MJPEG frame payloads.
        default:
            isoc_counter_increment(&uvc_stream->diagnostics.packet_error);
            isoc_drop_current_frame(uvc_stream);
            goto next_isoc_packet;
        }

        // Check for start of new frame
        const uvc_payload_header_t *payload_header = (const uvc_payload_header_t *)payload;
        if (!uvc_frame_payload_header_validate(payload_header, isoc_desc->actual_num_bytes)) {
            isoc_counter_increment(&uvc_stream->diagnostics.invalid_header);
            isoc_drop_current_frame(uvc_stream);
            goto next_isoc_packet;
        }

        // Derive payload data pointer/length once and reuse below
        const uint8_t *payload_data = payload + payload_header->bHeaderLength;
        const size_t payload_data_len = isoc_desc->actual_num_bytes - payload_header->bHeaderLength;

        if (payload_data_len == 0) {
            goto next_isoc_packet;
        }

        // Check for error flag
        if (payload_header->bmHeaderInfo.error) {
            isoc_counter_increment(&uvc_stream->diagnostics.frame_error);
            isoc_drop_current_frame(uvc_stream);
            goto next_isoc_packet;
        }

        const bool start_of_frame = (uvc_stream->single_thread.current_frame_id != payload_header->bmHeaderInfo.frame_id);
        if (start_of_frame) {
            // We detected start of new frame. Update Frame ID and start fetching this frame
            uvc_stream->single_thread.current_frame_id   = payload_header->bmHeaderInfo.frame_id;
            uvc_stream->single_thread.skip_current_frame = payload_header->bmHeaderInfo.error;

            // Check mjpeg frame start
            if (uvc_stream->dynamic.vs_format.format == UVC_VS_FORMAT_MJPEG &&
                    (payload_data_len < 2 || payload_data[0] != JPEG_MARKER || payload_data[1] != JPEG_SOI)) {
                // We received frame with invalid frame, skip this frame
                uvc_stream->single_thread.skip_current_frame = true;
                isoc_counter_increment(&uvc_stream->diagnostics.invalid_soi);
            }

            // Get free frame buffer for this new frame
            UVC_ENTER_CRITICAL();
            const bool need_new_frame = (uvc_stream->dynamic.streaming && !uvc_stream->dynamic.current_frame && !uvc_stream->single_thread.skip_current_frame);
            if (need_new_frame) {
                UVC_EXIT_CRITICAL();
                uvc_stream->dynamic.current_frame = uvc_frame_get_empty(uvc_stream);
                if (uvc_stream->dynamic.current_frame == NULL) {
                    // There is no free frame buffer now, skipping this frame
                    uvc_stream->single_thread.skip_current_frame = true;
                    isoc_counter_increment(&uvc_stream->diagnostics.buffer_underflow);
                    goto next_isoc_packet;
                }
            } else if (uvc_stream->dynamic.current_frame) {
                // We received SoF but current_frame is not NULL: We missed EoF - reset the frame buffer
                isoc_counter_increment(&uvc_stream->diagnostics.missed_eof);
                UVC_EXIT_CRITICAL();
                isoc_drop_current_frame(uvc_stream);
            } else {
                UVC_EXIT_CRITICAL();
            }
        }

        // Add received data to frame buffer
        if (!uvc_stream->single_thread.skip_current_frame) {
            uvc_host_frame_t *current_frame = UVC_ATOMIC_LOAD(uvc_stream->dynamic.current_frame);
            if (!current_frame) {
                isoc_counter_increment(&uvc_stream->diagnostics.buffer_underflow);
                uvc_stream->single_thread.skip_current_frame = true;
                goto next_isoc_packet;
            }

            esp_err_t ret = uvc_frame_add_data(current_frame, payload_data, payload_data_len);
            if (ret != ESP_OK) {
                isoc_counter_increment(&uvc_stream->diagnostics.buffer_overflow);
                isoc_drop_current_frame(uvc_stream);
                goto next_isoc_packet;
            }
        }

        // End of Frame. Pass the frame to user
        if (payload_header->bmHeaderInfo.end_of_frame) {
            bool return_frame = true; // In case streaming is stopped ATM, we must return the frame

            // Check if the user did not stop the stream in the meantime
            UVC_ENTER_CRITICAL();
            uvc_host_frame_t *this_frame = uvc_stream->dynamic.current_frame;
            uvc_stream->dynamic.current_frame = NULL; // Stop writing more data to this frame

            // Determine if we should invoke the frame callback:
            // Only invoke the callback if streaming is active, a frame callback exists,
            // and we have a valid frame to pass to the user.
            const bool invoke_fb_callback = (uvc_stream->dynamic.streaming && uvc_stream->constant.frame_cb && this_frame && !uvc_stream->single_thread.skip_current_frame);
            UVC_EXIT_CRITICAL();

            if (invoke_fb_callback) {
                return_frame = uvc_stream->constant.frame_cb(this_frame, uvc_stream->constant.cb_arg);
            }
            if (return_frame && this_frame) {
                // The user has processed the frame in his callback, return it back to empty queue
                uvc_host_frame_return(uvc_stream, this_frame);
            }
        }
next_isoc_packet:
        payload += isoc_desc->num_bytes;
        continue;
    }

    if (UVC_ATOMIC_LOAD(uvc_stream->dynamic.streaming)) {
        usb_host_transfer_submit(transfer); // Restart the transfer
    }
}
