#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

typedef enum {
    FLIPDOT_OTA_STATUS_START = 0,
    FLIPDOT_OTA_STATUS_PROGRESS,
    FLIPDOT_OTA_STATUS_SUCCESS,
    FLIPDOT_OTA_STATUS_FAILED,
} flipdot_ota_status_t;

typedef void (*flipdot_ota_status_cb_t)(flipdot_ota_status_t status, size_t bytes_written, size_t total_bytes, void *ctx);

/**
 * @brief Schedule an OTA update using the image available at the supplied URL.
 *
 * The OTA process runs on a dedicated FreeRTOS task. If an update is already in
 * progress, ESP_ERR_INVALID_STATE is returned.
 */
esp_err_t flipdot_ota_start(const char *url);

/**
 * @brief Check whether an OTA update task is currently running.
 */
bool flipdot_ota_is_running(void);

/**
 * @brief Register a callback invoked on OTA status changes.
 */
void flipdot_ota_set_status_callback(flipdot_ota_status_cb_t callback, void *ctx);
