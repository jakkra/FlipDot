#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <assert.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "string.h"
#include <mdns.h>
#include "lwip/apps/netbiosns.h"
#include "esp_http_client.h"

#include "web_server.h"
#include "flip_dot_driver.h"
#include "esp_sntp.h"
#include "esp_netif_sntp.h"
#include "framebuffer.h"
#include "fonts/font_3x5.h"
#include "fonts/font_3x6.h"
#include "fonts/font_pzim3x5.h"
#include "fonts/font_bmspa.h"
#include "fonts/font_homespun.h"

static char TAG[] = "FlipDot";

#define MAX_HTTP_RECV_BUFFER 1000

#define MAINTENANCE_HOUR    2
#define MAINTENANCE_MINUTE  30

typedef enum Mode_t {
    MODE_CLOCK,
    MODE_SCROLL_TEXT,
    MODE_REMOTE_CONTROL,
    MODE_SOLAR,
    MODE_ALERT,
    MODE_PREVENTIVE_MAINTENANCE_MODE
} Mode_t;

static Mode_t mode = MODE_REMOTE_CONTROL;
static Mode_t previous_mode = MODE_REMOTE_CONTROL;
static bool websocket_connected = false;
static bool mode_changed = true;
static char ip_addr[100] = "Waiting ip...";
static char scrolling_text[100] = "Scrolling text looks OK...";
static char alert_message[128] = {0};
static TickType_t alert_expire_tick = 0;

typedef struct {
    uint32_t temperature_inside;
    esp_err_t temperature_status;
    TickType_t last_temperature_update;

    uint32_t solar_production_watt;
    esp_err_t solar_status;
    TickType_t last_solar_update;

    SemaphoreHandle_t mutex;
} sensor_cache_t;

static sensor_cache_t sensor_cache;

#define ALERT_DISPLAY_DURATION_MS 10000
#define SENSOR_POLL_INTERVAL_MS 30000

static void handleModeSolar(void);
static void handleModeClock(bool first_run);
static void handleModeAnalogClock(bool first_run);
static void handleModeScrollingText(bool first_run, char* text);
static void handle_preventive_maintenance(bool first_run);
static void handleModeAlert(bool first_run);
static void redraw_flip_dot(uint8_t* framebuffer);
static esp_err_t fetch_home_assistant_sensor_state(const char* sensor_id, uint32_t* sensor_value);
static void trigger_alert(const char* message);
static void sensor_cache_init(void);
static void home_assistant_poll_task(void* arg);
static bool sensor_cache_get_temperature(uint32_t* value);
static bool sensor_cache_get_solar(uint32_t* value);
static void get_time(struct tm* timeinfo);

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "Failed to connect WiFi");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        memset(ip_addr, 0, sizeof(ip_addr));
        snprintf(ip_addr, sizeof(ip_addr), IPSTR, IP2STR(&event->ip_info.ip));
        if (mode == MODE_REMOTE_CONTROL) {
            mode_changed = true; // Trigger re-draw ip addr on screen
        }
    }
}

static void start_station(void)
{
    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT,  IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_WIFI_SSID,
            .password = CONFIG_WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG, "WiFi Sta Started");
}

static void handle_websocket_event(websocket_event_t event, uint8_t* data, uint32_t len) {
    if (event == WEBSOCKET_EVENT_CONNECTED) {
        websocket_connected = true;
        // Change mode automatically when ws connects
        mode = MODE_REMOTE_CONTROL;
        mode_changed = true;
        framebuffer_clear();
    } else if (event == WEBSOCKET_EVENT_DISCONNECTED) {
        websocket_connected = false;
        if (mode == MODE_REMOTE_CONTROL) {
            mode_changed = true; // Trigger re-draw of ip address
        }
    } else if (event == WEBSOCKET_EVENT_DATA) {
        if (mode == MODE_REMOTE_CONTROL) {
            flip_dot_driver_draw(data, len);
        }
    } else {
        assert(false); // Unhandled
    }
}

static void handle_mode_changed(uint32_t new_mode, char* extra_arg) {
    nvs_handle_t nvs_handle;

    if (new_mode == UINT32_MAX) {
        trigger_alert(extra_arg);
        return;
    }

    Mode_t requested_mode = (Mode_t)new_mode;

    if (requested_mode == MODE_ALERT) {
        trigger_alert(extra_arg);
        return;
    }

    mode = requested_mode;
    mode_changed = true;

    ESP_ERROR_CHECK(nvs_open("storage", NVS_READWRITE, &nvs_handle));
    ESP_ERROR_CHECK(nvs_set_u32(nvs_handle, "mode", requested_mode));
    if (strlen(extra_arg) > 0 && requested_mode == MODE_SCROLL_TEXT) {
        ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "scroll_text", extra_arg));
        strncpy(scrolling_text, extra_arg, sizeof(scrolling_text) - 1);
        scrolling_text[sizeof(scrolling_text) - 1] = '\0';
    }
    ESP_ERROR_CHECK(nvs_commit(nvs_handle));
    nvs_close(nvs_handle);
}

static void trigger_alert(const char* message)
{
    const char* safe_message = message != NULL ? message : "";

    if (safe_message[0] == '\0') {
        safe_message = "Alert!";
    }

    if (mode != MODE_ALERT) {
        previous_mode = mode;
    }

    snprintf(alert_message, sizeof(alert_message), "%s", safe_message);
    alert_message[sizeof(alert_message) - 1] = '\0';

    framebuffer_clear();
    alert_expire_tick = xTaskGetTickCount() + pdMS_TO_TICKS(ALERT_DISPLAY_DURATION_MS);
    mode = MODE_ALERT;
    mode_changed = true;
}

static void sensor_cache_init(void)
{
    memset(&sensor_cache, 0, sizeof(sensor_cache));
    sensor_cache.temperature_status = ESP_FAIL;
    sensor_cache.solar_status = ESP_FAIL;
    sensor_cache.mutex = xSemaphoreCreateMutex();
    assert(sensor_cache.mutex != NULL);
}

static bool sensor_cache_get_temperature(uint32_t* value)
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

static bool sensor_cache_get_solar(uint32_t* value)
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


static void home_assistant_poll_task(void* arg)
{
    (void)arg;
    TickType_t last_temp_poll = 0;
    TickType_t last_solar_poll = 0;

    const TickType_t poll_interval = pdMS_TO_TICKS(SENSOR_POLL_INTERVAL_MS);

    while (true) {
        TickType_t now = xTaskGetTickCount();

#ifdef CONFIG_HOME_ASSISTANT_SENSOR_ENTITY_ID
        if ((now - last_temp_poll >= poll_interval) || sensor_cache.temperature_status != ESP_OK) {
            uint32_t value = 0;
            esp_err_t err = fetch_home_assistant_sensor_state(CONFIG_HOME_ASSISTANT_SENSOR_ENTITY_ID, &value);
            if (xSemaphoreTake(sensor_cache.mutex, portMAX_DELAY) == pdTRUE) {
                sensor_cache.temperature_status = err;
                if (err == ESP_OK) {
                    sensor_cache.temperature_inside = value;
                }
                sensor_cache.last_temperature_update = now;
                xSemaphoreGive(sensor_cache.mutex);
            }
            last_temp_poll = now;
        }
#endif

#ifdef CONFIG_HOME_ASSISTANT_SENSOR_ENTITY_SOLAR_PRODUCTION_ID
        if ((now - last_solar_poll >= poll_interval) || sensor_cache.solar_status != ESP_OK) {
            uint32_t value = 0;
            esp_err_t err = fetch_home_assistant_sensor_state(CONFIG_HOME_ASSISTANT_SENSOR_ENTITY_SOLAR_PRODUCTION_ID, &value);
            if (xSemaphoreTake(sensor_cache.mutex, portMAX_DELAY) == pdTRUE) {
                sensor_cache.solar_status = err;
                if (err == ESP_OK) {
                    sensor_cache.solar_production_watt = value;
                }
                sensor_cache.last_solar_update = now;
                xSemaphoreGive(sensor_cache.mutex);
            }
            last_solar_poll = now;
        }
#endif

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
static void initialise_mdns(void)
{
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set("flip-dot"));
    ESP_ERROR_CHECK(mdns_instance_name_set("flip-dot-instance"));

    //structure with TXT records
    mdns_txt_item_t serviceTxtData[2] = {
        {"board","esp32"},
        {"path", "/"}
    };

    ESP_ERROR_CHECK(mdns_service_add("FlipDotDisplay", "_http", "_tcp", 80, serviceTxtData, sizeof(serviceTxtData) / sizeof(serviceTxtData[0])));
    netbiosns_init();
    netbiosns_set_name("flip-dot");
}

void time_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "Notification of a time synchronization event");
}

static void initialize_sntp(void)
{
    ESP_LOGI(TAG, "Initializing SNTP");
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    config.sync_cb = time_sync_notification_cb;
#ifdef CONFIG_SNTP_TIME_SYNC_METHOD_SMOOTH
    config.smooth_sync = true;
#endif
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "SNTP already initialized");
    } else {
        ESP_ERROR_CHECK(err);
    }
}

static void handleModeScrollingText(bool first_run, char* text)
{   
    if (first_run) {
        framebuffer_clear();
        framebuffer_scrolling_text(text, 0, 3, 200, &font_homespun_7x7, redraw_flip_dot);
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
}

static void handleModeSolar(void)
{   
    uint32_t solar_production_watt = 0;
    uint8_t* framebuffer;
    char draw_buf[64];

    framebuffer_clear();

    bool solar_available = sensor_cache_get_solar(&solar_production_watt);
    ESP_LOGD(TAG, "Solar production cached: %dW, available: %d", solar_production_watt, solar_available);
    if (solar_available && solar_production_watt > 0) {
        snprintf(draw_buf, sizeof(draw_buf), "Now");
        framebuffer = framebuffer_draw_string(draw_buf, 6, 1, &font_3x6, false);
        uint32_t digit1 = solar_production_watt / 1000;
        uint32_t digit2 = (uint32_t)round((solar_production_watt / 100.0) - (digit1 * 10));
        snprintf(draw_buf, sizeof(draw_buf), "%lu.%lukW", (unsigned long)digit1, (unsigned long)digit2);
        framebuffer = framebuffer_draw_string(draw_buf, 1, FRAMEBUFFER_HEIGHT - font_3x6.font_height, &font_3x6, false);

        static const uint8_t sun_icon[9][9] = {
            {0, 0, 0, 0, 1, 0, 0, 0, 0},
            {0, 1, 0, 0, 0, 0, 0, 1, 0},
            {0, 0, 0, 1, 1, 1, 0, 0, 0},
            {0, 0, 1, 1, 1, 1, 1, 0, 0},
            {1, 0, 1, 1, 1, 1, 1, 0, 1},
            {0, 0, 1, 1, 1, 1, 1, 0, 0},
            {0, 0, 0, 1, 1, 1, 0, 0, 0},
            {0, 1, 0, 0, 0, 0, 0, 1, 0},
            {0, 0, 0, 0, 1, 0, 0, 0, 0}
        };
        framebuffer = framebuffer_draw_bitmap(9, 9, sun_icon, FRAMEBUFFER_WIDTH - 9 , 0, false);

        static const uint8_t electric_icon[7][5] = {
            {0, 1, 1, 1, 1},
            {0, 1, 1, 1, 0},
            {1, 1, 1, 0, 0},
            {1, 1, 1, 1, 1},
            {0, 0, 1, 1, 0},
            {0, 1, 1, 0, 0},
            {0, 1, 0, 0, 0}
        };
        framebuffer = framebuffer_draw_bitmap(5, 7, electric_icon, 0 , 0, false);

        flip_dot_driver_draw(framebuffer, FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT);
        vTaskDelay(pdMS_TO_TICKS(10000));
    } else {
        handleModeClock(true);
    }
}

static void handleModeAlert(bool first_run)
{
    uint8_t* framebuffer;

    if (first_run) {
        framebuffer_clear();
        if (framebuffer_scrolling_text(alert_message, 0, 4, 200, &font_homespun_7x7, redraw_flip_dot) != ESP_OK) {
            framebuffer = framebuffer_draw_string(alert_message, 0, 0, &font_3x6, true);
            flip_dot_driver_draw(framebuffer, FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT);
        }
    }

    vTaskDelay(pdMS_TO_TICKS(150));
}

static void handleModeClock(bool first_run)
{
    time_t now;
    char strftime_buf[64];
    struct tm timeinfo;
    uint8_t* framebuffer;

    if (first_run) {
        framebuffer_clear();
    }

    time(&now);
    localtime_r(&now, &timeinfo);

    uint32_t temperature_inside = 0;
    bool temp_available = sensor_cache_get_temperature(&temperature_inside);

    if (timeinfo.tm_sec % 2 == 0) {
        strftime(strftime_buf, sizeof(strftime_buf), "%H:%M", &timeinfo);
    } else {
        strftime(strftime_buf, sizeof(strftime_buf), "%H %M", &timeinfo);
    }
    framebuffer_clear();
    framebuffer = framebuffer_draw_string(strftime_buf, 0, 1, &font_3x6, false);

    strftime(strftime_buf, sizeof(strftime_buf), "%a %d", &timeinfo);
    framebuffer = framebuffer_draw_string(strftime_buf, 3, font_3x6.font_height + 2, &font_3x6, false);

    if (temp_available) {
        snprintf(strftime_buf, sizeof(strftime_buf), "%lu", (unsigned long)temperature_inside);
        framebuffer = framebuffer_draw_string(strftime_buf, (FRAMEBUFFER_WIDTH - 1) - 3 * strlen(strftime_buf) - 1, 1, &font_3x6, false);
        // Manually add a "celcius" character
        framebuffer = framebuffer_set_pixel_value(FRAMEBUFFER_WIDTH - 1, 0, 1);
        // Draw a line between the time and temperature
        framebuffer = framebuffer_draw_line(18, 0, 18, 6, 1);
    }

    flip_dot_driver_draw(framebuffer, FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT);
    vTaskDelay(pdMS_TO_TICKS(1000));
}

static void handleModeAnalogClock(bool first_run)
{
    (void)first_run;
    struct tm timeinfo;
    uint8_t* framebuffer;

    get_time(&timeinfo);
    framebuffer_clear();
    framebuffer = framebuffer_draw_analog_clock((uint8_t)timeinfo.tm_hour, (uint8_t)timeinfo.tm_min, (uint8_t)timeinfo.tm_sec, true);
    flip_dot_driver_draw(framebuffer, FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT);
    vTaskDelay(pdMS_TO_TICKS(1000));
}

static void handle_preventive_maintenance(bool first_run)
{
    uint8_t* framebuffer;
    bool on = 0;

    if (first_run) {
        framebuffer_clear();
    }
    for (int iterations = 0; iterations < 10; iterations++) {
        for (int row = 0; row < FRAMEBUFFER_HEIGHT; row++) {
            for (int col = 0; col < FRAMEBUFFER_WIDTH; col++) {
                framebuffer = framebuffer_set_pixel_value(col, row, on);
                flip_dot_driver_draw(framebuffer, FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT);
                vTaskDelay(pdMS_TO_TICKS(1));
            }
        }
        on = !on;
    }
}

static void redraw_flip_dot(uint8_t* framebuffer)
{
    flip_dot_driver_draw(framebuffer, FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT);
}

static esp_err_t fetch_home_assistant_sensor_state(const char* sensor_id, uint32_t* sensor_value)
{
    esp_err_t err = ESP_OK;
    char *buffer = malloc(MAX_HTTP_RECV_BUFFER + 1);
    memset(buffer, 0, MAX_HTTP_RECV_BUFFER + 1);
    char url[200] = {0};

    snprintf(url, sizeof(url), "http://%s/api/states/%s", CONFIG_HOME_ASSISTANT_IP_ADDR, sensor_id);

    ESP_LOGI(TAG, "Fetching sensor state from url: %s", url);
    
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = NULL,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Authorization", CONFIG_HOME_ASSISTANT_BEARER_TOKEN);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    
    if (buffer == NULL) {
        ESP_LOGE(TAG, "Cannot malloc http receive buffer");
        return ESP_FAIL;
    }
 
    if ((err = esp_http_client_open(client, 0)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        free(buffer);
        return ESP_FAIL;
    }
    int content_length =  esp_http_client_fetch_headers(client);
    int total_read_len = 0, read_len;
    if (total_read_len < content_length && content_length <= MAX_HTTP_RECV_BUFFER) {
        read_len = esp_http_client_read(client, buffer, content_length);
        if (read_len <= 0) {
            ESP_LOGE(TAG, "Error read data");
        }
        buffer[read_len] = 0;
        ESP_LOGD(TAG, "read_len = %d", read_len);
    }

    char* needle = "\"state\":\"";
    if (needle != NULL) {
        char* value_location = strstr(buffer, needle);
        if (value_location != NULL) {
            value_location += strlen(needle);
            *sensor_value = (uint32_t)round(atof(value_location));
        } else {
            err = ESP_FAIL;
        }
    } else {
        err = ESP_FAIL;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(buffer);

    return err;
}

static void get_time(struct tm* timeinfo) {
    time_t now;
    time(&now);
    localtime_r(&now, timeinfo);
}

static Mode_t get_mode_nvs(void) {
    Mode_t mode;
    nvs_handle_t nvs_handle;
    esp_err_t ret;

    ESP_ERROR_CHECK(nvs_open("storage", NVS_READWRITE, &nvs_handle));

    uint32_t stored_mode = MODE_REMOTE_CONTROL;
    ret = nvs_get_u32(nvs_handle, "mode", &stored_mode);
    if (ret != ESP_OK) {
        mode = MODE_REMOTE_CONTROL;
    } else {
        mode = (Mode_t)stored_mode;
    }

    nvs_close(nvs_handle);

    return mode;
}

void app_main() {
    uint8_t* framebuffer;
    nvs_handle_t nvs_handle;
    size_t max_len;
    struct tm timeinfo;

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(nvs_open("storage", NVS_READWRITE, &nvs_handle));

    uint32_t stored_mode = MODE_REMOTE_CONTROL;
    ret = nvs_get_u32(nvs_handle, "mode", &stored_mode);
    if (ret == ESP_OK) {
        mode = (Mode_t)stored_mode;
    } else {
        mode = MODE_REMOTE_CONTROL;
    }
    max_len = sizeof(scrolling_text);
    nvs_get_str(nvs_handle, "scroll_text", scrolling_text, &max_len);

    nvs_close(nvs_handle);

    webserver_init(&handle_websocket_event, &handle_mode_changed);
    start_station();

    webserver_start();
    initialise_mdns();
    initialize_sntp();

    setenv("TZ", "CET-1CEST,M3.5.0/02,M10.5.0/03", 1);
    tzset();

    flip_dot_driver_init();
    // In case display has been off for a while
    // just flip all dots a few times to make sure none
    // are stuck.
    //handle_preventive_maintenance(true);

    ESP_LOGW(TAG, "Started and running\n");

    framebuffer_init();
    framebuffer_clear();

    sensor_cache_init();
    assert(xTaskCreate(home_assistant_poll_task, "ha_poll", 4096, NULL, 5, NULL) == pdPASS);

    while (true) {
        bool temp_mode_changed = mode_changed;
        mode_changed = false;

        if (mode == MODE_ALERT && alert_expire_tick != 0) {
            TickType_t now_ticks = xTaskGetTickCount();
            if ((int32_t)(alert_expire_tick - now_ticks) <= 0) {
                framebuffer_clear();
                mode = previous_mode;
                mode_changed = true;
                alert_expire_tick = 0;
                continue;
            }
        }

        get_time(&timeinfo);
        if (timeinfo.tm_hour == MAINTENANCE_HOUR && timeinfo.tm_min == MAINTENANCE_MINUTE) {
            if (mode != MODE_PREVENTIVE_MAINTENANCE_MODE) {
                temp_mode_changed = true;
                mode = MODE_PREVENTIVE_MAINTENANCE_MODE;
                ESP_LOGI(TAG, "Entering mainenatnce mode for one minute");
            }
        } else if ((timeinfo.tm_hour != MAINTENANCE_HOUR || timeinfo.tm_min != MAINTENANCE_MINUTE) && mode == MODE_PREVENTIVE_MAINTENANCE_MODE) {
            temp_mode_changed = true;
            mode = get_mode_nvs();
            ESP_LOGI(TAG, "Leaving mainenatnce mode");
        }

        ESP_LOGI(TAG, "Mode: %d", mode);

        switch (mode) {
            case MODE_CLOCK:
                handleModeClock(temp_mode_changed);
                break;
            case MODE_SCROLL_TEXT:
                handleModeScrollingText(temp_mode_changed, scrolling_text);
                break;
            case MODE_REMOTE_CONTROL:
                if (temp_mode_changed && !websocket_connected) {
                    framebuffer_clear();
                    framebuffer = framebuffer_draw_string(ip_addr, 0, 0, &font_3x6, true);
                    flip_dot_driver_draw(framebuffer, FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT);
                }
                vTaskDelay(pdMS_TO_TICKS(1000));
                break;
            case MODE_SOLAR:
                handleModeSolar();
                break;
            case MODE_ANALOG_CLOCK:
                handleModeAnalogClock(temp_mode_changed);
                break;
            case MODE_ALERT:
                handleModeAlert(temp_mode_changed);
                break;
            case MODE_PREVENTIVE_MAINTENANCE_MODE:
                handle_preventive_maintenance(temp_mode_changed);
                break;
            default:
                break;
        }
    }
}
