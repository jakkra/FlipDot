#include <stdio.h>
#include <math.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "string.h"
#include "mdns.h"
#include "lwip/apps/netbiosns.h"
#include "esp_http_client.h"

#include "web_server.h"
#include "flip_dot_driver.h"
#include "esp_sntp.h"
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

#define DEFAULT_SOLAR_MODE_ROTATION_INTERVAL_MS 10000

typedef enum Mode_t {
    MODE_CLOCK,
    MODE_SCROLL_TEXT,
    MODE_REMOTE_CONTROL,
    MODE_SOLAR,
    MODE_PREVENTIVE_MAINTENANCE_MODE
} Mode_t;

static Mode_t mode = MODE_REMOTE_CONTROL;
static bool websocket_connected = false;
static bool mode_changed = true;
static char ip_addr[100] = "Waiting ip...";
static char scrolling_text[100] = "Scrolling text looks OK...";

static void handleModeSolar(void);
static void handleModeClock(bool first_run);
static void handleModeScrollingText(bool first_run, char* text);
static void handle_preventive_maintenance(bool first_run);
static void redraw_flip_dot(uint8_t* framebuffer);
static esp_err_t fetch_home_assistant_sensor_state(const char* sensor_id, uint32_t* sensor_value);

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
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
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

    mode = new_mode;
    mode_changed = true;

    ESP_ERROR_CHECK(nvs_open("storage", NVS_READWRITE, &nvs_handle));
    ESP_ERROR_CHECK(nvs_set_u32(nvs_handle, "mode", new_mode));
    if (strlen(extra_arg) > 0 && MODE_SCROLL_TEXT) {
        ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "scroll_text", extra_arg));
        strncpy(scrolling_text, extra_arg, sizeof(scrolling_text));
    }
    ESP_ERROR_CHECK(nvs_commit(nvs_handle));
    nvs_close(nvs_handle);
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
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_set_time_sync_notification_cb(time_sync_notification_cb);
#ifdef CONFIG_SNTP_TIME_SYNC_METHOD_SMOOTH
    sntp_set_sync_mode(SNTP_SYNC_MODE_SMOOTH);
#endif
    sntp_init();
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

    int err = fetch_home_assistant_sensor_state(CONFIG_HOME_ASSISTANT_SENSOR_ENTITY_SOLAR_PRODUCTION_ID, &solar_production_watt);

    if (err == ESP_OK && solar_production_watt > 0) {
        snprintf(draw_buf, sizeof(draw_buf), "Now");
        framebuffer = framebuffer_draw_string(draw_buf, 6, 1, &font_3x6, false);
        uint32_t digit1 = solar_production_watt / 1000;
        uint32_t digit2 = round((solar_production_watt / 100.0) - (digit1 * 10));
        snprintf(draw_buf, sizeof(draw_buf), "%d.%dkW", digit1, digit2);
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
        vTaskDelay(pdMS_TO_TICKS(1000));
    } else {
        handleModeClock(true);
    }
}

static void handleModeClock(bool first_run)
{
    time_t now;
    char strftime_buf[64];
    struct tm timeinfo;
    uint8_t* framebuffer;
    uint32_t temperature_inside = 0;
    esp_err_t err;

    if (first_run) {
        framebuffer_clear();
    }

    time(&now);
    localtime_r(&now, &timeinfo);

    // Adjust for daylight saving time
    if (timeinfo.tm_isdst) {
        now = mktime(&timeinfo);
        now -= 3600;
        localtime_r(&now, &timeinfo);
    }

    if (timeinfo.tm_sec % 2 == 0) {
        strftime(strftime_buf, sizeof(strftime_buf), "%H:%M", &timeinfo);
    } else {
        strftime(strftime_buf, sizeof(strftime_buf), "%H %M", &timeinfo);
    }
    framebuffer_clear();
    framebuffer = framebuffer_draw_string(strftime_buf, 0, 1, &font_3x6, false);

    strftime(strftime_buf, sizeof(strftime_buf), "%a %d", &timeinfo);
    framebuffer = framebuffer_draw_string(strftime_buf, 3, font_3x6.font_height + 2, &font_3x6, false);

    err = fetch_home_assistant_sensor_state(CONFIG_HOME_ASSISTANT_SENSOR_ENTITY_ID, &temperature_inside);

    if (err == ESP_OK) {
        snprintf(strftime_buf, sizeof(strftime_buf), "%d", temperature_inside);
        framebuffer = framebuffer_draw_string(strftime_buf, (FRAMEBUFFER_WIDTH - 1) - 3 * strlen(strftime_buf) - 1, 1, &font_3x6, false);
        // Manually add a "celcius" character
        framebuffer = framebuffer_set_pixel_value(FRAMEBUFFER_WIDTH - 1, 0, 1);
        // Draw a line between the time and temperature
        // TODO Implement framebuffer_draw_line
        framebuffer = framebuffer_set_pixel_value(18, 0, 1);
        framebuffer = framebuffer_set_pixel_value(18, 1, 1);
        framebuffer = framebuffer_set_pixel_value(18, 2, 1);
        framebuffer = framebuffer_set_pixel_value(18, 3, 1);
        framebuffer = framebuffer_set_pixel_value(18, 4, 1);
        framebuffer = framebuffer_set_pixel_value(18, 5, 1);
        framebuffer = framebuffer_set_pixel_value(18, 6, 1);
    }

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
    for (int iterations = 0; iterations < 2; iterations++) {
        for (int row = 0; row < FRAMEBUFFER_HEIGHT; row++) {
            for (int col = 0; col < FRAMEBUFFER_WIDTH; col++) {
                framebuffer = framebuffer_set_pixel_value(col, row, on);
                flip_dot_driver_draw(framebuffer, FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT);
                vTaskDelay(pdMS_TO_TICKS(15));
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
    
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = NULL,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_add_auth(client);
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

    ret = nvs_get_u32(nvs_handle, "mode", &mode);
    if (ret != ESP_OK) {
        mode = MODE_REMOTE_CONTROL;
    }

    nvs_close(nvs_handle);

    return mode;
}

void app_main() {
    uint8_t* framebuffer;
    nvs_handle_t nvs_handle;
    uint32_t max_len;
    struct tm timeinfo;

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(nvs_open("storage", NVS_READWRITE, &nvs_handle));

    ret = nvs_get_u32(nvs_handle, "mode", &mode);
    if (ret != ESP_OK) {
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

    setenv("TZ", "CET-1CEST", 1);
    tzset();

    flip_dot_driver_init();
    // In case display has been off for a while
    // just flip all dots a few times to make sure none
    // are stuck.
    handle_preventive_maintenance(true);

    ESP_LOGW(TAG, "Started and running\n");

    framebuffer_init();
    framebuffer_clear();

    while (true) {
        bool temp_mode_changed = mode_changed;
        mode_changed = false;

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
            case MODE_PREVENTIVE_MAINTENANCE_MODE:
                handle_preventive_maintenance(temp_mode_changed);
                break;
            default:
                break;
        }
    }
}


