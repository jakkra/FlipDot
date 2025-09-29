#pragma once

#include <stdbool.h>

#include "esp_err.h"

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
