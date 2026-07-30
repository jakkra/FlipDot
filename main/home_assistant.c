#include "home_assistant.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "sdkconfig.h"

static const char* TAG = "HomeAssistant";

#define MAX_HTTP_RECV_BUFFER 1000
#define SENSOR_POLL_INTERVAL_MS 30000

typedef struct {
    int32_t temperature_inside;
    esp_err_t temperature_status;
    TickType_t last_temperature_update;

    uint32_t solar_production_watt;
    esp_err_t solar_status;
    TickType_t last_solar_update;

    float weather_temperature;
    float weather_humidity;
    float weather_pressure;
    char weather_condition[32];
    esp_err_t weather_status;
    TickType_t last_weather_update;

    SemaphoreHandle_t mutex;
} sensor_cache_t;

static sensor_cache_t sensor_cache;

// Forward declarations
static esp_err_t fetch_home_assistant_sensor_state(const char* sensor_id, int32_t* sensor_value);
static esp_err_t fetch_home_assistant_weather_state(void);
static void home_assistant_poll_task(void* arg);

void home_assistant_cache_init(void)
{
    memset(&sensor_cache, 0, sizeof(sensor_cache));
    sensor_cache.temperature_status = ESP_FAIL;
    sensor_cache.solar_status = ESP_FAIL;
    sensor_cache.weather_status = ESP_FAIL;
    sensor_cache.mutex = xSemaphoreCreateMutex();
    if (sensor_cache.mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
    }
}

void home_assistant_start_polling(void)
{
    BaseType_t result = xTaskCreate(home_assistant_poll_task, "ha_poll", 4096, NULL, 5, NULL);
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create polling task");
    }
}

bool home_assistant_get_temperature(int32_t* value)
{
    bool available = false;

    if (sensor_cache.mutex == NULL || value == NULL) {
        return false;
    }

    if (xSemaphoreTake(sensor_cache.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (sensor_cache.temperature_status == ESP_OK) {
            *value = sensor_cache.temperature_inside;
            available = true;
        }
        xSemaphoreGive(sensor_cache.mutex);
    }

    return available;
}

bool home_assistant_get_solar(uint32_t* value)
{
    bool available = false;

    if (sensor_cache.mutex == NULL || value == NULL) {
        return false;
    }

    if (xSemaphoreTake(sensor_cache.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (sensor_cache.solar_status == ESP_OK) {
            *value = sensor_cache.solar_production_watt;
            available = true;
        }
        xSemaphoreGive(sensor_cache.mutex);
    }

    return available;
}

bool home_assistant_get_weather(float* temperature, float* humidity, float* pressure, 
                                  char* condition, size_t condition_size)
{
    bool available = false;

    if (sensor_cache.mutex == NULL || temperature == NULL || humidity == NULL || 
        pressure == NULL || condition == NULL) {
        return false;
    }

    if (xSemaphoreTake(sensor_cache.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (sensor_cache.weather_status == ESP_OK) {
            *temperature = sensor_cache.weather_temperature;
            *humidity = sensor_cache.weather_humidity;
            *pressure = sensor_cache.weather_pressure;
            strncpy(condition, sensor_cache.weather_condition, condition_size - 1);
            condition[condition_size - 1] = '\0';
            available = true;
        }
        xSemaphoreGive(sensor_cache.mutex);
    }

    return available;
}

static esp_err_t fetch_home_assistant_sensor_state(const char* sensor_id, int32_t* sensor_value)
{
    esp_err_t err = ESP_OK;
    char *buffer = malloc(MAX_HTTP_RECV_BUFFER + 1);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "Cannot malloc http receive buffer");
        return ESP_FAIL;
    }
    memset(buffer, 0, MAX_HTTP_RECV_BUFFER + 1);
    char url[200] = {0};

    snprintf(url, sizeof(url), "http://%s/api/states/%s", CONFIG_HOME_ASSISTANT_IP_ADDR, sensor_id);

    ESP_LOGI(TAG, "Fetching sensor state from url: %s", url);
    
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = NULL,
        .timeout_ms = 5000,
        .keep_alive_enable = false,
        .disable_auto_redirect = true,
        .buffer_size = MAX_HTTP_RECV_BUFFER,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        free(buffer);
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Authorization", CONFIG_HOME_ASSISTANT_BEARER_TOKEN);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Connection", "close");
 
    if ((err = esp_http_client_open(client, 0)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        free(buffer);
        return ESP_FAIL;
    }
    
    esp_err_t header_status = esp_http_client_fetch_headers(client);
    if (header_status < 0) {
        ESP_LOGE(TAG, "Failed to fetch headers: %s", esp_err_to_name(header_status));
        err = ESP_FAIL;
    }

    int status_code = esp_http_client_get_status_code(client);
    if (err == ESP_OK && (status_code <= 0 || status_code >= 400)) {
        ESP_LOGE(TAG, "HTTP status %d", status_code);
        err = ESP_FAIL;
    }

    int content_length = esp_http_client_get_content_length(client);
    int total_read_len = 0;
    while (err == ESP_OK && total_read_len < MAX_HTTP_RECV_BUFFER) {
        int chunk_len = esp_http_client_read(client, buffer + total_read_len,
                                             MAX_HTTP_RECV_BUFFER - total_read_len);
        if (chunk_len < 0) {
            ESP_LOGE(TAG, "Error reading data");
            err = ESP_FAIL;
            break;
        }
        if (chunk_len == 0) {
            break;
        }
        total_read_len += chunk_len;
        if (content_length > 0 && total_read_len >= content_length) {
            break;
        }
    }

    if (err == ESP_OK) {
        if (total_read_len == 0) {
            ESP_LOGE(TAG, "Empty response body");
            err = ESP_FAIL;
        } else {
            if (total_read_len > MAX_HTTP_RECV_BUFFER) {
                total_read_len = MAX_HTTP_RECV_BUFFER;
            }
            buffer[total_read_len] = 0;
            ESP_LOGD(TAG, "read_len = %d", total_read_len);
        }
    }

    if (err == ESP_OK) {
        char* needle = "\"state\":\"";
        char* value_location = strstr(buffer, needle);
        if (value_location != NULL) {
            value_location += strlen(needle);
            *sensor_value = (int32_t)lround(atof(value_location));
        } else {
            err = ESP_FAIL;
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(buffer);

    return err;
}

static esp_err_t fetch_home_assistant_weather_state(void)
{
    esp_err_t err = ESP_OK;
    char *buffer = malloc(MAX_HTTP_RECV_BUFFER + 1);
    if (buffer == NULL) {
        ESP_LOGE(TAG, "Cannot malloc http receive buffer");
        return ESP_FAIL;
    }
    memset(buffer, 0, MAX_HTTP_RECV_BUFFER + 1);
    char url[200] = {0};

    snprintf(url, sizeof(url), "http://%s/api/states/weather.home", CONFIG_HOME_ASSISTANT_IP_ADDR);

    ESP_LOGI(TAG, "Fetching weather state from url: %s", url);
    
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = NULL,
        .timeout_ms = 5000,
        .keep_alive_enable = false,
        .disable_auto_redirect = true,
        .buffer_size = MAX_HTTP_RECV_BUFFER,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        free(buffer);
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Authorization", CONFIG_HOME_ASSISTANT_BEARER_TOKEN);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Connection", "close");
 
    if ((err = esp_http_client_open(client, 0)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        free(buffer);
        return ESP_FAIL;
    }
    
    esp_err_t header_status = esp_http_client_fetch_headers(client);
    if (header_status < 0) {
        ESP_LOGE(TAG, "Failed to fetch headers: %s", esp_err_to_name(header_status));
        err = ESP_FAIL;
    }

    int status_code = esp_http_client_get_status_code(client);
    if (err == ESP_OK && (status_code <= 0 || status_code >= 400)) {
        ESP_LOGE(TAG, "HTTP status %d", status_code);
        err = ESP_FAIL;
    }

    int content_length = esp_http_client_get_content_length(client);
    int total_read_len = 0;
    while (err == ESP_OK && total_read_len < MAX_HTTP_RECV_BUFFER) {
        int chunk_len = esp_http_client_read(client, buffer + total_read_len,
                                             MAX_HTTP_RECV_BUFFER - total_read_len);
        if (chunk_len < 0) {
            ESP_LOGE(TAG, "Error reading data");
            err = ESP_FAIL;
            break;
        }
        if (chunk_len == 0) {
            break;
        }
        total_read_len += chunk_len;
        if (content_length > 0 && total_read_len >= content_length) {
            break;
        }
    }

    if (err == ESP_OK) {
        if (total_read_len == 0) {
            ESP_LOGE(TAG, "Empty response body");
            err = ESP_FAIL;
        } else {
            if (total_read_len > MAX_HTTP_RECV_BUFFER) {
                total_read_len = MAX_HTTP_RECV_BUFFER;
            }
            buffer[total_read_len] = 0;
            ESP_LOGD(TAG, "read_len = %d", total_read_len);
        }
    }

    if (err == ESP_OK) {
        // Parse JSON response for weather data
        char* temp_location = strstr(buffer, "\"temperature\":");
        char* humidity_location = strstr(buffer, "\"humidity\":");
        char* pressure_location = strstr(buffer, "\"pressure\":");
        char* state_location = strstr(buffer, "\"state\":\"");

        if (temp_location && humidity_location && pressure_location && state_location) {
            // Parse temperature
            temp_location += strlen("\"temperature\":");
#ifndef CONFIG_HOME_ASSISTANT_SENSOR_TEMP_OUTSIDE_ENTITY_ID
            sensor_cache.weather_temperature = atof(temp_location);
#endif
            // Parse humidity  
            humidity_location += strlen("\"humidity\":");
            sensor_cache.weather_humidity = atof(humidity_location);

            // Parse pressure
            pressure_location += strlen("\"pressure\":");
            sensor_cache.weather_pressure = atof(pressure_location);

            // Parse weather condition/state
            state_location += strlen("\"state\":\"");
            char* end_quote = strchr(state_location, '"');
            if (end_quote) {
                size_t len = end_quote - state_location;
                if (len >= sizeof(sensor_cache.weather_condition)) {
                    len = sizeof(sensor_cache.weather_condition) - 1;
                }
                strncpy(sensor_cache.weather_condition, state_location, len);
                sensor_cache.weather_condition[len] = '\0';
            }
        } else {
            err = ESP_FAIL;
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(buffer);

    return err;
}

static void home_assistant_poll_task(void* arg)
{
    (void)arg;
    TickType_t last_temp_poll = 0;
    TickType_t last_solar_poll = 0;
    TickType_t last_weather_poll = 0;

    const TickType_t poll_interval = pdMS_TO_TICKS(SENSOR_POLL_INTERVAL_MS);

    while (true) {
        TickType_t now = xTaskGetTickCount();

        if ((now - last_temp_poll >= poll_interval) || sensor_cache.temperature_status != ESP_OK) {
            int32_t value = 0;
#ifdef CONFIG_HOME_ASSISTANT_SENSOR_ENTITY_ID
            esp_err_t err = fetch_home_assistant_sensor_state(CONFIG_HOME_ASSISTANT_SENSOR_ENTITY_ID, &value);
            if (xSemaphoreTake(sensor_cache.mutex, portMAX_DELAY) == pdTRUE) {
                sensor_cache.temperature_status = err;
                if (err == ESP_OK) {
                    sensor_cache.temperature_inside = value;
                }
                sensor_cache.last_temperature_update = now;
                xSemaphoreGive(sensor_cache.mutex);
            }
#endif
#ifdef CONFIG_HOME_ASSISTANT_SENSOR_TEMP_OUTSIDE_ENTITY_ID
            value = 0;
            err = fetch_home_assistant_sensor_state(CONFIG_HOME_ASSISTANT_SENSOR_TEMP_OUTSIDE_ENTITY_ID, &value);
            if (xSemaphoreTake(sensor_cache.mutex, portMAX_DELAY) == pdTRUE) {
                sensor_cache.temperature_status = err;
                if (err == ESP_OK) {
                    sensor_cache.weather_temperature = value;
                }
                sensor_cache.last_temperature_update = now;
                xSemaphoreGive(sensor_cache.mutex);
            }
#endif
            last_temp_poll = now;
        }

#ifdef CONFIG_HOME_ASSISTANT_SENSOR_ENTITY_SOLAR_PRODUCTION_ID
        if ((now - last_solar_poll >= poll_interval) || sensor_cache.solar_status != ESP_OK) {
            int32_t value = 0;
            esp_err_t err = fetch_home_assistant_sensor_state(CONFIG_HOME_ASSISTANT_SENSOR_ENTITY_SOLAR_PRODUCTION_ID, &value);
            if (xSemaphoreTake(sensor_cache.mutex, portMAX_DELAY) == pdTRUE) {
                sensor_cache.solar_status = err;
                if (err == ESP_OK) {
                    sensor_cache.solar_production_watt = value < 0 ? 0 : (uint32_t)value;
                }
                sensor_cache.last_solar_update = now;
                xSemaphoreGive(sensor_cache.mutex);
            }
            last_solar_poll = now;
        }
#endif

        // Weather polling
        if ((now - last_weather_poll >= poll_interval) || sensor_cache.weather_status != ESP_OK) {
            esp_err_t err = fetch_home_assistant_weather_state();
            if (xSemaphoreTake(sensor_cache.mutex, portMAX_DELAY) == pdTRUE) {
                sensor_cache.weather_status = err;
                sensor_cache.last_weather_update = now;
                xSemaphoreGive(sensor_cache.mutex);
            }
            last_weather_poll = now;

            // If all requests failed, wait a bit longer before retrying
            if (sensor_cache.temperature_status != ESP_OK &&
                sensor_cache.solar_status != ESP_OK && 
                sensor_cache.weather_status != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(5000)); // Wait 5 seconds on total failure
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
