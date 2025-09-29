#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"

#include "ota_update.h"

#define OTA_TASK_STACK_SIZE 8192
#define OTA_TASK_PRIORITY   5
#define OTA_BUFFER_SIZE     4096

static const char *TAG = "flipdot_ota";

static TaskHandle_t s_ota_task_handle = NULL;

static void ota_task(void *pvParameter)
{
    char *url = (char *)pvParameter;
    ESP_LOGI(TAG, "Starting OTA from %s", url);

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 10000,
        .keep_alive_enable = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        goto cleanup;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        goto cleanup_client;
    }

    int headers_status = esp_http_client_fetch_headers(client);
    if (headers_status < 0) {
        ESP_LOGW(TAG, "Failed to fetch headers: %d", headers_status);
    }

    int status_code = esp_http_client_get_status_code(client);
    if (status_code <= 0 || status_code >= 400) {
        ESP_LOGE(TAG, "HTTP request failed with status %d", status_code);
        err = ESP_FAIL;
        goto cleanup_client;
    }

    int64_t content_length = esp_http_client_get_content_length(client);
    ESP_LOGI(TAG, "Image size: %lld bytes", content_length);

    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "No OTA partition available");
        goto cleanup_client;
    }

    ESP_LOGI(TAG, "Writing to partition subtype %d at offset 0x%lx", update_partition->subtype, update_partition->address);

    esp_ota_handle_t update_handle = 0;
    size_t ota_size = content_length > 0 ? (size_t)content_length : OTA_SIZE_UNKNOWN;
    err = esp_ota_begin(update_partition, ota_size, &update_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed (%s)", esp_err_to_name(err));
        goto cleanup_client;
    }

    uint8_t *buffer = (uint8_t *)malloc(OTA_BUFFER_SIZE);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "Failed to allocate OTA buffer");
        esp_ota_end(update_handle);
        goto cleanup_client;
    }

    int total_written = 0;
    while (1) {
        int data_read = esp_http_client_read(client, (char *)buffer, OTA_BUFFER_SIZE);
        if (data_read < 0) {
            ESP_LOGE(TAG, "Error reading HTTP stream");
            err = ESP_FAIL;
            break;
        }
        if (data_read == 0) {
            ESP_LOGI(TAG, "Download complete");
            err = ESP_OK;
            break;
        }

        err = esp_ota_write(update_handle, buffer, data_read);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed (%s)", esp_err_to_name(err));
            break;
        }
        total_written += data_read;
    }

    free(buffer);

    if (err != ESP_OK) {
        esp_ota_end(update_handle);
        goto cleanup_client;
    }

    err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed (%s)", esp_err_to_name(err));
        goto cleanup_client;
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed (%s)", esp_err_to_name(err));
        goto cleanup_client;
    }

    ESP_LOGI(TAG, "OTA successful, written %d bytes. Restarting...", total_written);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(url);
    s_ota_task_handle = NULL;
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return;

cleanup_client:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

cleanup:
    ESP_LOGE(TAG, "OTA failed");
    free(url);
    s_ota_task_handle = NULL;
    vTaskDelete(NULL);
}

esp_err_t flipdot_ota_start(const char *url)
{
    if (url == NULL || url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_ota_task_handle != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    char *url_copy = strdup(url);
    if (url_copy == NULL) {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t created = xTaskCreate(ota_task, "flipdot_ota", OTA_TASK_STACK_SIZE, url_copy, OTA_TASK_PRIORITY, &s_ota_task_handle);
    if (created != pdPASS) {
        free(url_copy);
        return ESP_FAIL;
    }

    return ESP_OK;
}

bool flipdot_ota_is_running(void)
{
    return s_ota_task_handle != NULL;
}
