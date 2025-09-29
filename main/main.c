#include <stdio.h>
#include <stdint.h>
#include <math.h>
#include <assert.h>
#include <sys/socket.h>
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
#include <ctype.h>
#include <mdns.h>
#include "lwip/apps/netbiosns.h"
#include "esp_http_client.h"

#include "web_server.h"
#include "flip_dot_driver.h"
#include "ota_update.h"
#include "esp_sntp.h"
#include "esp_netif_sntp.h"
#include "framebuffer.h"
#include "animated_modes.h"
#include "fonts/font_3x5.h"
#include "fonts/font_3x6.h"
#include "fonts/font_pzim3x5.h"
#include "fonts/font_bmspa.h"
#include "fonts/font_homespun.h"

#include "weather/cloud.xbm"
#include "weather/clouds.xbm"
#include "weather/cloud_moon.xbm"
#include "weather/cloud_sun.xbm"
#include "weather/cloud_wind.xbm"
#include "weather/cloud_wind_moon.xbm"
#include "weather/cloud_wind_sun.xbm"
#include "weather/lightning.xbm"
#include "weather/moon.xbm"
#include "weather/rain0.xbm"
#include "weather/rain1.xbm"
#include "weather/rain1_moon.xbm"
#include "weather/rain1_sun.xbm"
#include "weather/rain2.xbm"
#include "weather/rain_lightning.xbm"
#include "weather/rain_snow.xbm"
#include "weather/snow_moon.xbm"
#include "weather/snow_sun.xbm"
#include "weather/sun.xbm"
#include "weather/wind.xbm"

static char TAG[] = "FlipDot";

#define MAX_HTTP_RECV_BUFFER 1000

#define MAINTENANCE_HOUR    2
#define MAINTENANCE_MINUTE  30

#define CLOCK_WEATHER_SWAP_INTERVAL_MINUTES 1

typedef enum Mode_t {
    MODE_CLOCK = 0,
    MODE_SCROLL_TEXT = 1,
    MODE_REMOTE_CONTROL = 2,
    MODE_SOLAR = 3,
    MODE_PREVENTIVE_MAINTENANCE_MODE = 6,
    MODE_ANALOG_CLOCK = 7,
    MODE_CLOCK_WEATHER = 8,
    MODE_FIREFLIES_IDLE = 20,
    MODE_CELLULAR_AUTOMATA = 21,
    MODE_MATRIX_RAIN = 22,
    MODE_RIPPLE = 23,
    MODE_TUNNEL = 24,
    MODE_BOUNCING_BALLS = 25,
    MODE_TELEPORT = 26,
    MODE_LISSAJOUS = 27,
    MODE_OTA_PROGRESS = 100
} Mode_t;

static Mode_t mode = MODE_REMOTE_CONTROL;
static Mode_t previous_mode = MODE_REMOTE_CONTROL;
static bool websocket_connected = false;
static bool mode_changed = true;
static char ip_addr[100] = "Waiting ip...";
static char scrolling_text[100] = "Scrolling text looks OK...";

static const Mode_t kModeCycle[] = {
    MODE_CLOCK,
    MODE_CLOCK_WEATHER,
    MODE_SCROLL_TEXT,
    MODE_REMOTE_CONTROL,
    MODE_SOLAR,
    MODE_ANALOG_CLOCK,
    MODE_FIREFLIES_IDLE,
    MODE_CELLULAR_AUTOMATA,
    MODE_MATRIX_RAIN,
    MODE_RIPPLE,
    MODE_TUNNEL,
    MODE_BOUNCING_BALLS,
    MODE_TELEPORT,
    MODE_LISSAJOUS,
};

static bool mode_banner_active = false;
static bool mode_banner_drawn = false;
static bool mode_skip_banner_on_next_change = false;
static TickType_t mode_banner_expire_tick = 0;
static char mode_banner_text[32] = {0};
static Mode_t current_display_mode = MODE_REMOTE_CONTROL;
static bool mode_transition_pending = false;
static bool invert_display = false;

static bool ota_display_in_progress = false;
static bool ota_display_failed = false;
static bool ota_display_success = false;
static size_t ota_display_written = 0;
static size_t ota_display_total = 0;
static TickType_t ota_display_hold_until = 0;
static Mode_t ota_previous_mode = MODE_REMOTE_CONTROL;


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

#define SENSOR_POLL_INTERVAL_MS 30000
#define HTTP_CLIENT_RETRY_DELAY_MS 5000

static void handleModeSolar(void);
static void handleModeClock(bool first_run);
static void handleModeAnalogClock(bool first_run);
static void handleModeClockWeather(bool first_run);
static void handleModeScrollingText(bool first_run, char* text);
static void handle_preventive_maintenance(bool first_run);
static void handleModeOtaProgress(bool first_run);
static void redraw_flip_dot(uint8_t* framebuffer);
static esp_err_t fetch_home_assistant_sensor_state(const char* sensor_id, int32_t* sensor_value);
static esp_err_t fetch_home_assistant_weather_state(void);
static void sensor_cache_init(void);
static void home_assistant_poll_task(void* arg);
static bool sensor_cache_get_temperature(int32_t* value);
static bool sensor_cache_get_solar(uint32_t* value);
static bool sensor_cache_get_weather(float* temperature, float* humidity, float* pressure, char* condition, size_t condition_size);
static void get_time(struct tm* timeinfo);
static void start_mode_banner(Mode_t new_mode);
static const char* mode_to_string(Mode_t mode);
static bool mode_is_valid(Mode_t mode);
static bool mode_is_user_selectable(Mode_t mode);
static Mode_t normalize_mode(uint32_t stored_mode);
static Mode_t step_mode(int direction);
static void draw_mode_banner(void);
static void apply_mode_selection(Mode_t requested_mode, const char* extra_arg);
static bool parse_bool_string(const char* value, bool* out_value);
static void apply_invert_setting(bool invert, bool persist);
static const char* weather_condition_to_icon_bits(const char* condition);
static void ota_status_callback(flipdot_ota_status_t status, size_t bytes_written, size_t total_bytes, void *ctx);


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
        if (mode != MODE_REMOTE_CONTROL) {
            mode = MODE_REMOTE_CONTROL;
            mode_transition_pending = true;
        }
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
    const char* safe_arg = (extra_arg != NULL) ? extra_arg : "";

    if (new_mode == MODE_COMMAND_SET_INVERT) {
        bool requested_invert = false;
        if (!parse_bool_string(safe_arg, &requested_invert)) {
            ESP_LOGW(TAG, "Invalid invert parameter: '%s'", safe_arg);
            return;
        }

        if (invert_display != requested_invert) {
            apply_invert_setting(requested_invert, true);
            ESP_LOGI(TAG, "Display inversion %s", requested_invert ? "enabled" : "disabled");
            mode_changed = true;
            mode_transition_pending = false;
        }
        return;
    }

    if (new_mode == MODE_COMMAND_NEXT || new_mode == MODE_COMMAND_PREV) {
        int direction = (new_mode == MODE_COMMAND_NEXT) ? 1 : -1;
        Mode_t next_mode = step_mode(direction);
        apply_mode_selection(next_mode, NULL);
        start_mode_banner(next_mode);
        return;
    }

    Mode_t requested_mode = normalize_mode(new_mode);

    apply_mode_selection(requested_mode, safe_arg);
    start_mode_banner(requested_mode);
}

static void sensor_cache_init(void)
{
    memset(&sensor_cache, 0, sizeof(sensor_cache));
    sensor_cache.temperature_status = ESP_FAIL;
    sensor_cache.solar_status = ESP_FAIL;
    sensor_cache.weather_status = ESP_FAIL;
    sensor_cache.mutex = xSemaphoreCreateMutex();
    assert(sensor_cache.mutex != NULL);
}

static bool sensor_cache_get_temperature(int32_t* value)
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

static bool sensor_cache_get_weather(float* temperature, float* humidity, float* pressure, char* condition, size_t condition_size)
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

static const char* weather_condition_to_icon_bits(const char* condition)
{
    const char* fallback = cloud_bits;

    if (condition == NULL || condition[0] == '\0') {
        return fallback;
    }

    char normalized[48];
    size_t len = strlen(condition);
    if (len > sizeof(normalized) - 1) {
        len = sizeof(normalized) - 1;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)condition[i];
        c = (unsigned char)tolower(c);
        if (c == ' ' || c == '_') {
            c = '-';
        }
        normalized[i] = (char)c;
    }
    normalized[len] = '\0';

    struct tm timeinfo;
    get_time(&timeinfo);
    bool is_night = (timeinfo.tm_hour < 6 || timeinfo.tm_hour >= 20);

    if (strcmp(normalized, "clear-night") == 0) {
        return moon_bits;
    }

    if (strcmp(normalized, "sunny") == 0 || strcmp(normalized, "clear-day") == 0) {
        return sun_bits;
    }

    if (strcmp(normalized, "clear") == 0) {
        return is_night ? moon_bits : sun_bits;
    }

    if (strcmp(normalized, "partlycloudy") == 0 || strcmp(normalized, "partly-cloudy") == 0) {
        return is_night ? cloud_moon_bits : cloud_sun_bits;
    }

    if (strcmp(normalized, "cloudy") == 0 || strcmp(normalized, "overcast") == 0) {
        return clouds_bits;
    }

    if (strcmp(normalized, "windy-variant") == 0) {
        return is_night ? cloud_wind_moon_bits : cloud_wind_sun_bits;
    }

    if (strcmp(normalized, "windy") == 0) {
        return wind_bits;
    }

    if (strcmp(normalized, "fog") == 0 || strcmp(normalized, "mist") == 0) {
        return cloud_bits;
    }

    if (strcmp(normalized, "hail") == 0) {
        return rain_snow_bits;
    }

    if (strcmp(normalized, "lightning-rainy") == 0 || strcmp(normalized, "thunderstorm") == 0) {
        return rain_lightning_bits;
    }

    if (strcmp(normalized, "lightning") == 0) {
        return lightning_bits;
    }

    if (strcmp(normalized, "pouring") == 0) {
        return rain2_bits;
    }

    if (strcmp(normalized, "rainy") == 0) {
        return is_night ? rain1_moon_bits : rain1_sun_bits;
    }

    if (strcmp(normalized, "drizzle") == 0 || strcmp(normalized, "light-rain") == 0) {
        return rain0_bits;
    }

    if (strcmp(normalized, "snowy-rainy") == 0) {
        return rain_snow_bits;
    }

    if (strcmp(normalized, "snowy") == 0) {
        return is_night ? snow_moon_bits : snow_sun_bits;
    }

    if (strcmp(normalized, "exceptional") == 0) {
        return rain_lightning_bits;
    }

    if (strstr(normalized, "lightning") != NULL ||
        strstr(normalized, "thunder") != NULL ||
        strstr(normalized, "storm") != NULL) {
        return rain_lightning_bits;
    }

    if (strstr(normalized, "snow") != NULL) {
        if (strstr(normalized, "rain") != NULL) {
            return rain_snow_bits;
        }
        return is_night ? snow_moon_bits : snow_sun_bits;
    }

    if (strstr(normalized, "hail") != NULL) {
        return rain_snow_bits;
    }

    if (strstr(normalized, "rain") != NULL ||
        strstr(normalized, "drizzle") != NULL ||
        strstr(normalized, "shower") != NULL) {
        return is_night ? rain1_moon_bits : rain1_bits;
    }

    if (strstr(normalized, "wind") != NULL || strstr(normalized, "breeze") != NULL) {
        return wind_bits;
    }

    if (strstr(normalized, "fog") != NULL || strstr(normalized, "mist") != NULL) {
        return cloud_bits;
    }

    if (strstr(normalized, "partly") != NULL && strstr(normalized, "cloud") != NULL) {
        return is_night ? cloud_moon_bits : cloud_sun_bits;
    }

    if (strstr(normalized, "cloud") != NULL || strstr(normalized, "overcast") != NULL) {
        return clouds_bits;
    }

    if (is_night) {
        return moon_bits;
    }

    if (strstr(normalized, "sun") != NULL || strstr(normalized, "clear") != NULL) {
        return sun_bits;
    }

    return fallback;
}

static const char* mode_to_string(Mode_t current_mode)
{
    switch (current_mode) {
        case MODE_CLOCK:
            return "Clock D";
        case MODE_CLOCK_WEATHER:
            return "Clock+W";
        case MODE_SCROLL_TEXT:
            return "Scroll";
        case MODE_REMOTE_CONTROL:
            return "Remote";
        case MODE_SOLAR:
            return "Solar";
        case MODE_PREVENTIVE_MAINTENANCE_MODE:
            return "Maintenance";
        case MODE_ANALOG_CLOCK:
            return "Clock A";
        case MODE_FIREFLIES_IDLE:
            return "Fireflies";
        case MODE_CELLULAR_AUTOMATA:
            return "Cells";
        case MODE_MATRIX_RAIN:
            return "Matrix";
        case MODE_RIPPLE:
            return "Ripple";
        case MODE_TUNNEL:
            return "Tunnel";
        case MODE_BOUNCING_BALLS:
            return "Bounce";
        case MODE_TELEPORT:
            return "Teleport";
        case MODE_LISSAJOUS:
            return "Lissajous";
        case MODE_OTA_PROGRESS:
            return "Update";
        default:
            return "Mode";
    }
}

static bool mode_is_valid(Mode_t candidate)
{
    switch (candidate) {
        case MODE_CLOCK:
        case MODE_CLOCK_WEATHER:
        case MODE_SCROLL_TEXT:
        case MODE_REMOTE_CONTROL:
        case MODE_SOLAR:
        case MODE_PREVENTIVE_MAINTENANCE_MODE:
        case MODE_ANALOG_CLOCK:
        case MODE_FIREFLIES_IDLE:
        case MODE_CELLULAR_AUTOMATA:
        case MODE_MATRIX_RAIN:
        case MODE_RIPPLE:
        case MODE_TUNNEL:
        case MODE_BOUNCING_BALLS:
        case MODE_TELEPORT:
        case MODE_LISSAJOUS:
        case MODE_OTA_PROGRESS:
            return true;
        default:
            return false;
    }
}

static bool mode_is_user_selectable(Mode_t candidate)
{
    size_t count = sizeof(kModeCycle) / sizeof(kModeCycle[0]);
    for (size_t i = 0; i < count; i++) {
        if (kModeCycle[i] == candidate) {
            return true;
        }
    }
    return false;
}

static Mode_t normalize_mode(uint32_t stored_mode)
{
    Mode_t candidate = (Mode_t)stored_mode;
    if (!mode_is_valid(candidate)) {
        return MODE_REMOTE_CONTROL;
    }
    return candidate;
}

static Mode_t step_mode(int direction)
{
    const size_t count = sizeof(kModeCycle) / sizeof(kModeCycle[0]);
    Mode_t reference = mode;

    if (!mode_is_user_selectable(reference)) {
        if (mode_is_user_selectable(previous_mode)) {
            reference = previous_mode;
        } else {
            reference = kModeCycle[0];
        }
    }

    size_t index = 0;
    bool found = false;
    for (size_t i = 0; i < count; i++) {
        if (kModeCycle[i] == reference) {
            index = i;
            found = true;
            break;
        }
    }

    if (!found) {
        return kModeCycle[0];
    }

    int next_index = (int)index + direction;
    if (next_index < 0) {
        next_index += (int)count;
    }
    if (next_index >= (int)count) {
        next_index -= (int)count;
    }

    return kModeCycle[next_index];
}

static void draw_mode_banner(void)
{
    font_t* font = &font_3x6;
    uint8_t text_width = (uint8_t)framebuffer_get_string_width(mode_banner_text, font);
    uint8_t x = 0;
    if (FRAMEBUFFER_WIDTH > text_width) {
        x = (uint8_t)((FRAMEBUFFER_WIDTH - text_width) / 2);
    }
    uint8_t y = 0;
    if (FRAMEBUFFER_HEIGHT > font->font_height) {
        y = (uint8_t)((FRAMEBUFFER_HEIGHT - font->font_height) / 2);
    }

    uint8_t* framebuffer = framebuffer_clear();
    framebuffer = framebuffer_draw_string(mode_banner_text, x, y, font, false);
    flip_dot_driver_draw(framebuffer, FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT);
    mode_banner_drawn = true;
}

static void start_mode_banner(Mode_t new_mode)
{
    const char* name = mode_to_string(new_mode);
    snprintf(mode_banner_text, sizeof(mode_banner_text), "%s", name);
    mode_banner_text[sizeof(mode_banner_text) - 1] = '\0';
    mode_banner_expire_tick = xTaskGetTickCount() + pdMS_TO_TICKS(4000);
    mode_banner_active = true;
    mode_banner_drawn = false;
    mode_skip_banner_on_next_change = false;
}

static void apply_mode_selection(Mode_t requested_mode, const char* extra_arg)
{
    nvs_handle_t nvs_handle;

    mode = requested_mode;
    mode_changed = true;
    mode_transition_pending = true;

    ESP_ERROR_CHECK(nvs_open("storage", NVS_READWRITE, &nvs_handle));
    ESP_ERROR_CHECK(nvs_set_u32(nvs_handle, "mode", (uint32_t)requested_mode));

    if (requested_mode == MODE_SCROLL_TEXT && extra_arg != NULL && extra_arg[0] != '\0') {
        ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "scroll_text", extra_arg));
        strncpy(scrolling_text, extra_arg, sizeof(scrolling_text) - 1);
        scrolling_text[sizeof(scrolling_text) - 1] = '\0';
    }

    ESP_ERROR_CHECK(nvs_commit(nvs_handle));
    nvs_close(nvs_handle);
}

static bool parse_bool_string(const char* value, bool* out_value)
{
    if (value == NULL || out_value == NULL) {
        return false;
    }

    size_t len = strlen(value);
    if (len == 0) {
        return false;
    }

    if (len == 1) {
        if (value[0] == '1') {
            *out_value = true;
            return true;
        }
        if (value[0] == '0') {
            *out_value = false;
            return true;
        }
    }

    char normalized[8];
    if (len >= sizeof(normalized)) {
        len = sizeof(normalized) - 1;
    }
    for (size_t i = 0; i < len; i++) {
        normalized[i] = (char)tolower((unsigned char)value[i]);
    }
    normalized[len] = '\0';

    if (strcmp(normalized, "true") == 0 || strcmp(normalized, "on") == 0 || strcmp(normalized, "yes") == 0) {
        *out_value = true;
        return true;
    }

    if (strcmp(normalized, "false") == 0 || strcmp(normalized, "off") == 0 || strcmp(normalized, "no") == 0) {
        *out_value = false;
        return true;
    }

    return false;
}

static void ota_status_callback(flipdot_ota_status_t status, size_t bytes_written, size_t total_bytes, void *ctx)
{
    (void)ctx;

    switch (status) {
        case FLIPDOT_OTA_STATUS_START:
            ota_previous_mode = mode;
            ota_display_in_progress = true;
            ota_display_failed = false;
            ota_display_success = false;
            ota_display_written = 0;
            ota_display_total = total_bytes;
            ota_display_hold_until = 0;
            mode_banner_active = false;
            mode_skip_banner_on_next_change = true;
            mode_transition_pending = false;
            mode = MODE_OTA_PROGRESS;
            mode_changed = true;
            break;
        case FLIPDOT_OTA_STATUS_PROGRESS:
            ota_display_in_progress = true;
            ota_display_written = bytes_written;
            ota_display_total = total_bytes;
            mode_banner_active = false;
            mode_skip_banner_on_next_change = true;
            break;
        case FLIPDOT_OTA_STATUS_SUCCESS:
            ota_display_in_progress = false;
            ota_display_success = true;
            ota_display_failed = false;
            ota_display_written = bytes_written;
            ota_display_total = total_bytes;
            ota_display_hold_until = xTaskGetTickCount() + pdMS_TO_TICKS(1000);
            mode_banner_active = false;
            mode_skip_banner_on_next_change = true;
            mode_transition_pending = false;
            mode = MODE_OTA_PROGRESS;
            mode_changed = true;
            break;
        case FLIPDOT_OTA_STATUS_FAILED:
            ota_display_in_progress = false;
            ota_display_failed = true;
            ota_display_success = false;
            ota_display_written = bytes_written;
            ota_display_total = total_bytes;
            ota_display_hold_until = xTaskGetTickCount() + pdMS_TO_TICKS(5000);
            mode_banner_active = false;
            mode_skip_banner_on_next_change = true;
            mode_transition_pending = false;
            mode = MODE_OTA_PROGRESS;
            mode_changed = true;
            break;
        default:
            break;
    }
}

static void apply_invert_setting(bool invert, bool persist)
{
    invert_display = invert;
    flip_dot_driver_set_invert(invert);

    if (persist) {
        nvs_handle_t nvs_handle;
        ESP_ERROR_CHECK(nvs_open("storage", NVS_READWRITE, &nvs_handle));
        ESP_ERROR_CHECK(nvs_set_u8(nvs_handle, "invert", invert ? 1 : 0));
        ESP_ERROR_CHECK(nvs_commit(nvs_handle));
        nvs_close(nvs_handle);
    }
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

#ifdef CONFIG_HOME_ASSISTANT_SENSOR_ENTITY_ID
        if ((now - last_temp_poll >= poll_interval) || sensor_cache.temperature_status != ESP_OK) {
            int32_t value = 0;
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
static void initialise_mdns(void)
{
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set("flip"));
    ESP_ERROR_CHECK(mdns_instance_name_set("flip-instance"));

    //structure with TXT records
    mdns_txt_item_t serviceTxtData[2] = {
        {"board","esp32"},
        {"path", "/"}
    };

    ESP_ERROR_CHECK(mdns_service_add("FlipDotDisplay", "_http", "_tcp", 80, serviceTxtData, sizeof(serviceTxtData) / sizeof(serviceTxtData[0])));
    netbiosns_init();
    netbiosns_set_name("flip");
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

static void handleModeOtaProgress(bool first_run)
{
    (void)first_run;

    uint8_t* framebuffer = framebuffer_clear();
    font_t* font = &font_3x6;
    char line1[20];
    char line2[20];

    if (ota_display_in_progress) {
        strcpy(line1, "Updating");

        if (ota_display_total > 0) {
            uint32_t percent = (uint32_t)((ota_display_written * 100) / ota_display_total);
            if (percent > 100) {
                percent = 100;
            }
            snprintf(line2, sizeof(line2), "%3lu%%", (unsigned long)percent);
        } else {
            unsigned long kilobytes = (unsigned long)((ota_display_written + 512) / 1024);
            snprintf(line2, sizeof(line2), "%lu kB", kilobytes);
        }
    } else if (ota_display_success) {
        strcpy(line1, "Update");
        strcpy(line2, "Success");
    } else if (ota_display_failed) {
        strcpy(line1, "Update");
        strcpy(line2, "Failed");
    } else {
        ota_display_in_progress = false;
        mode = ota_previous_mode;
        mode_changed = true;
        mode_transition_pending = true;
        mode_skip_banner_on_next_change = true;
        ota_display_written = 0;
        ota_display_total = 0;
        return;
    }

    uint8_t width1 = (uint8_t)framebuffer_get_string_width(line1, font);
    uint8_t width2 = (uint8_t)framebuffer_get_string_width(line2, font);
    uint8_t x1 = (width1 < FRAMEBUFFER_WIDTH) ? (uint8_t)((FRAMEBUFFER_WIDTH - width1) / 2) : 0;
    uint8_t x2 = (width2 < FRAMEBUFFER_WIDTH) ? (uint8_t)((FRAMEBUFFER_WIDTH - width2) / 2) : 0;

    framebuffer = framebuffer_draw_string(line1, x1, 1, font, false);
    framebuffer = framebuffer_draw_string(line2, x2, FRAMEBUFFER_HEIGHT - font->font_height - 1, font, false);

    flip_dot_driver_draw(framebuffer, FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT);

    if (!ota_display_in_progress) {
        if (ota_display_failed) {
            if ((int32_t)(ota_display_hold_until - xTaskGetTickCount()) <= 0) {
                ota_display_failed = false;
                ota_display_written = 0;
                ota_display_total = 0;
                mode = ota_previous_mode;
                mode_changed = true;
                mode_transition_pending = true;
                mode_skip_banner_on_next_change = true;
            }
        } else if (ota_display_success) {
            if ((int32_t)(ota_display_hold_until - xTaskGetTickCount()) <= 0) {
                ota_display_success = false;
                ota_display_written = 0;
                ota_display_total = 0;
            }
        }
    }

    vTaskDelay(pdMS_TO_TICKS(200));
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

    int32_t temperature_inside = 0;
    bool temp_available = sensor_cache_get_temperature(&temperature_inside);

    //if (timeinfo.tm_sec % 2 == 0) {
        strftime(strftime_buf, sizeof(strftime_buf), "%H:%M", &timeinfo);
    //} else {
    //    strftime(strftime_buf, sizeof(strftime_buf), "%H %M", &timeinfo);
    //}
    framebuffer_clear();
    framebuffer = framebuffer_draw_string(strftime_buf, 0, 1, &font_3x6, false);

    strftime(strftime_buf, sizeof(strftime_buf), "%a %d", &timeinfo);
    framebuffer = framebuffer_draw_string(strftime_buf, 3, font_3x6.font_height + 2, &font_3x6, false);

    if (temp_available) {
        snprintf(strftime_buf, sizeof(strftime_buf), "%ld", (long)temperature_inside);
        uint8_t temp_width = (uint8_t)framebuffer_get_string_width(strftime_buf, &font_3x6);
        uint8_t temp_x = 0;
        if (FRAMEBUFFER_WIDTH > temp_width) {
            temp_x = (uint8_t)(FRAMEBUFFER_WIDTH - temp_width - 1);
        }
        framebuffer = framebuffer_draw_string(strftime_buf, temp_x, 1, &font_3x6, false);
        // Manually add a "celcius" character
        framebuffer = framebuffer_set_pixel_value(FRAMEBUFFER_WIDTH - 1, 0, 1);
        // Draw a line between the time and temperature
        framebuffer = framebuffer_draw_line(18, 0, 18, 6, 1);
    }

    flip_dot_driver_draw(framebuffer, FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT);
    vTaskDelay(pdMS_TO_TICKS(1000));
}

static void handleModeClockWeather(bool first_run)
{
    static bool show_clock = false;
    static TickType_t next_swap_tick = 0;
    static TickType_t swap_interval_ticks = 0;
    uint8_t* framebuffer;

    if (swap_interval_ticks == 0) {
        int minutes = CLOCK_WEATHER_SWAP_INTERVAL_MINUTES;
        if (minutes < 1) {
            minutes = 1;
        }
        swap_interval_ticks = pdMS_TO_TICKS(minutes * 60 * 1000);
        if (swap_interval_ticks == 0) {
            swap_interval_ticks = pdMS_TO_TICKS(60000);
        }
    }

    TickType_t now_ticks = xTaskGetTickCount();

    if (first_run) {
        show_clock = false;
        framebuffer_clear();
        next_swap_tick = now_ticks + swap_interval_ticks;
    }

    if ((int32_t)(now_ticks - next_swap_tick) >= 0) {
        show_clock = !show_clock;
        next_swap_tick = now_ticks + swap_interval_ticks;
        framebuffer_clear();
    }

    if (show_clock) {
        time_t now;
        struct tm timeinfo;
        char draw_buf[32];

        time(&now);
        localtime_r(&now, &timeinfo);

        framebuffer = framebuffer_clear();
        strftime(draw_buf, sizeof(draw_buf), "%H:%M", &timeinfo);
        framebuffer = framebuffer_draw_string(draw_buf, 5, 1, &font_3x6, false);

        strftime(draw_buf, sizeof(draw_buf), "%a %d", &timeinfo);
        framebuffer = framebuffer_draw_string(draw_buf, 3, font_3x6.font_height + 2, &font_3x6, false);

        flip_dot_driver_draw(framebuffer, FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT);
    } else {
        float outside_temperature = 0.0f;
        float humidity = 0.0f;
        float pressure = 0.0f;
        char condition[32] = {0};
        int32_t inside_temperature = 0;

        bool inside_available = sensor_cache_get_temperature(&inside_temperature);
        bool weather_available = sensor_cache_get_weather(&outside_temperature, &humidity, &pressure, condition, sizeof(condition));
        (void)humidity;
        (void)pressure;

        framebuffer = framebuffer_clear();

        const char* icon_bits = weather_condition_to_icon_bits(weather_available ? condition : NULL);
        if (icon_bits != NULL) {
            framebuffer = framebuffer_draw_xbm16x16_cropped(icon_bits, 0, 0, 1, 1);
        }

        font_t* temp_font = &font_3x5;
        const uint8_t right_area_x = 16;
        const uint8_t right_area_width = FRAMEBUFFER_WIDTH - right_area_x;
        const uint8_t inside_y = 1;
        const uint8_t outside_y = (FRAMEBUFFER_HEIGHT > temp_font->font_height)
                                      ? (FRAMEBUFFER_HEIGHT - temp_font->font_height - 1)
                                      : 0;

        char inside_line[8];
        char outside_line[8];

        if (inside_available) {
            long inside_int = (long)inside_temperature;
            snprintf(inside_line, sizeof(inside_line), "%ld", inside_int);
        } else {
            strncpy(inside_line, "--", sizeof(inside_line) - 1);
            inside_line[sizeof(inside_line) - 1] = '\0';
        }

        if (weather_available) {
            long outside_int = lroundf(outside_temperature);
            snprintf(outside_line, sizeof(outside_line), "%ld", outside_int);
        } else {
            strncpy(outside_line, "--", sizeof(outside_line) - 1);
            outside_line[sizeof(outside_line) - 1] = '\0';
        }

        uint8_t inside_width = (uint8_t)framebuffer_get_string_width(inside_line, temp_font);
        if (inside_width > right_area_width && strlen(inside_line) > 1) {
            memmove(inside_line, inside_line + 1, strlen(inside_line));
            inside_width = (uint8_t)framebuffer_get_string_width(inside_line, temp_font);
        }

        uint8_t outside_width = (uint8_t)framebuffer_get_string_width(outside_line, temp_font);
        if (outside_width > right_area_width && strlen(outside_line) > 1) {
            memmove(outside_line, outside_line + 1, strlen(outside_line));
            outside_width = (uint8_t)framebuffer_get_string_width(outside_line, temp_font);
        }

        uint8_t inside_x = FRAMEBUFFER_WIDTH > inside_width + 1
                               ? (uint8_t)(FRAMEBUFFER_WIDTH - inside_width - 2)
                               : right_area_x;
        if (inside_x < right_area_x) {
            inside_x = right_area_x;
        }

        uint8_t outside_x = FRAMEBUFFER_WIDTH > outside_width + 1
                                ? (uint8_t)(FRAMEBUFFER_WIDTH - outside_width - 2)
                                : right_area_x;
        if (outside_x < right_area_x) {
            outside_x = right_area_x;
        }

        framebuffer = framebuffer_draw_string(inside_line, inside_x, inside_y, temp_font, false);
        framebuffer = framebuffer_draw_string(outside_line, outside_x, outside_y, temp_font, false);

        if (inside_y < FRAMEBUFFER_HEIGHT) {
            framebuffer = framebuffer_set_pixel_value(FRAMEBUFFER_WIDTH - 1, inside_y, 1);
        }
        if (outside_y < FRAMEBUFFER_HEIGHT) {
            framebuffer = framebuffer_set_pixel_value(FRAMEBUFFER_WIDTH - 1, outside_y, 1);
        }

        flip_dot_driver_draw(framebuffer, FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT);
    }

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
        .timeout_ms = 5000,           // Reduced timeout from 10s to 5s
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
    
    int content_length =  esp_http_client_fetch_headers(client);
    int total_read_len = 0, read_len;
    if (total_read_len < content_length && content_length <= MAX_HTTP_RECV_BUFFER) {
        read_len = esp_http_client_read(client, buffer, content_length);
        if (read_len <= 0) {
            ESP_LOGE(TAG, "Error read data");
            err = ESP_FAIL;
        } else {
            buffer[read_len] = 0;
            ESP_LOGD(TAG, "read_len = %d", read_len);
        }
    } else {
        err = ESP_FAIL;
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
        .timeout_ms = 5000,           // Reduced timeout from 10s to 5s
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
    
    int content_length =  esp_http_client_fetch_headers(client);
    int total_read_len = 0, read_len;
    if (total_read_len < content_length && content_length <= MAX_HTTP_RECV_BUFFER) {
        read_len = esp_http_client_read(client, buffer, content_length);
        if (read_len <= 0) {
            ESP_LOGE(TAG, "Error read data");
            err = ESP_FAIL;
        } else {
            buffer[read_len] = 0;
            ESP_LOGD(TAG, "read_len = %d", read_len);
        }
    } else {
        err = ESP_FAIL;
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
            sensor_cache.weather_temperature = atof(temp_location);

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

static void get_time(struct tm* timeinfo) {
    time_t now;
    time(&now);
    localtime_r(&now, timeinfo);
}

static Mode_t get_mode_nvs(void) {
    nvs_handle_t nvs_handle;
    esp_err_t ret;

    ESP_ERROR_CHECK(nvs_open("storage", NVS_READWRITE, &nvs_handle));

    uint32_t stored_mode = MODE_REMOTE_CONTROL;
    ret = nvs_get_u32(nvs_handle, "mode", &stored_mode);

    nvs_close(nvs_handle);

    if (ret != ESP_OK) {
        return MODE_REMOTE_CONTROL;
    }

    return normalize_mode(stored_mode);
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
        mode = normalize_mode(stored_mode);
    } else {
        mode = MODE_REMOTE_CONTROL;
    }
    max_len = sizeof(scrolling_text);
    nvs_get_str(nvs_handle, "scroll_text", scrolling_text, &max_len);

    uint8_t stored_invert = 0;
    ret = nvs_get_u8(nvs_handle, "invert", &stored_invert);
    invert_display = (ret == ESP_OK) ? (stored_invert != 0) : false;

    nvs_close(nvs_handle);

    current_display_mode = mode;
    mode_transition_pending = true;

    webserver_init(&handle_websocket_event, &handle_mode_changed);
    flipdot_ota_set_status_callback(ota_status_callback, NULL);
    start_station();

    webserver_start();
    initialise_mdns();
    initialize_sntp();

    setenv("TZ", "CET-1CEST,M3.5.0/02,M10.5.0/03", 1);
    tzset();

    flip_dot_driver_init();
    flip_dot_driver_set_invert(invert_display);
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
        bool skip_banner = mode_skip_banner_on_next_change;
        mode_skip_banner_on_next_change = false;

        get_time(&timeinfo);
        if (timeinfo.tm_hour == MAINTENANCE_HOUR && timeinfo.tm_min == MAINTENANCE_MINUTE) {
            if (mode != MODE_PREVENTIVE_MAINTENANCE_MODE) {
                temp_mode_changed = true;
                mode = MODE_PREVENTIVE_MAINTENANCE_MODE;
                mode_transition_pending = true;
                ESP_LOGI(TAG, "Entering mainenatnce mode for one minute");
            }
        } else if ((timeinfo.tm_hour != MAINTENANCE_HOUR || timeinfo.tm_min != MAINTENANCE_MINUTE) && mode == MODE_PREVENTIVE_MAINTENANCE_MODE) {
            temp_mode_changed = true;
            mode = get_mode_nvs();
            mode_transition_pending = true;
            ESP_LOGI(TAG, "Leaving mainenatnce mode");
        }

        if (skip_banner) {
            temp_mode_changed = true;
            mode_transition_pending = false;
        }

        if (temp_mode_changed && !skip_banner && mode_transition_pending) {
            start_mode_banner(mode);
            mode_transition_pending = false;
        }

        if (mode_banner_active) {
            if (!mode_banner_drawn || temp_mode_changed) {
                draw_mode_banner();
            }

            TickType_t now_ticks = xTaskGetTickCount();
            if ((int32_t)(mode_banner_expire_tick - now_ticks) > 0) {
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }

            mode_banner_active = false;
            mode_skip_banner_on_next_change = true;
            mode_changed = true;
            continue;
        }

        ESP_LOGI(TAG, "Mode: %d", mode);

        switch (mode) {
            case MODE_CLOCK:
                handleModeClock(temp_mode_changed);
                break;
            case MODE_CLOCK_WEATHER:
                handleModeClockWeather(temp_mode_changed);
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
            case MODE_FIREFLIES_IDLE:
                handleModeFirefliesIdle(temp_mode_changed);
                break;
            case MODE_CELLULAR_AUTOMATA:
                handleModeCellularAutomata(temp_mode_changed);
                break;
            case MODE_MATRIX_RAIN:
                handleModeMatrixRain(temp_mode_changed);
                break;
            case MODE_RIPPLE:
                handleModeRipple(temp_mode_changed);
                break;
            case MODE_TUNNEL:
                handleModeTunnel(temp_mode_changed);
                break;
            case MODE_BOUNCING_BALLS:
                handleModeBouncingBalls(temp_mode_changed);
                break;
            case MODE_TELEPORT:
                handleModeTeleport(temp_mode_changed);
                break;
            case MODE_LISSAJOUS:
                handleModeLissajous(temp_mode_changed);
                break;
            case MODE_OTA_PROGRESS:
                handleModeOtaProgress(temp_mode_changed);
                break;
            case MODE_PREVENTIVE_MAINTENANCE_MODE:
                handle_preventive_maintenance(temp_mode_changed);
                break;
            default:
                break;
        }

        current_display_mode = mode;
    }
}
