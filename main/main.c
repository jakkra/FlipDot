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

#include "web_server.h"
#include "flip_dot_driver.h"
#include "ota_update.h"
#include "esp_sntp.h"
#include "esp_netif_sntp.h"
#include "framebuffer.h"
#include "animated_modes.h"
#include "utils.h"
#include "home_assistant.h"
#include "fonts/font_3x5.h"
#include "fonts/font_3x6.h"
#include "fonts/font_pzim3x5.h"
#include "fonts/font_bmspa.h"
#include "fonts/font_homespun.h"

static char TAG[] = "FlipDot";

#define MAINTENANCE_HOUR    2
#define MAINTENANCE_MINUTE  30
#define MAINTENANCE_ITERATIONS  50

#define CLOCK_WEATHER_SWAP_INTERVAL_MINUTES 10

typedef enum Mode_t {
    MODE_CLOCK = 0,
    MODE_SCROLL_TEXT = 1,
    MODE_REMOTE_CONTROL = 2,
    MODE_SOLAR = 3,
    MODE_PREVENTIVE_MAINTENANCE_MODE = 6,
    MODE_ANALOG_CLOCK = 7,
    MODE_CLOCK_WEATHER = 8,
    MODE_CLOCK_TEMP = 9,
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

// Display state
typedef struct {
    Mode_t mode;
    Mode_t previous_mode;
    bool websocket_connected;
    bool mode_changed;
    char ip_addr[100];
    char scrolling_text[100];
    bool invert_display;
} display_state_t;

static display_state_t display_state = {
    .mode = MODE_REMOTE_CONTROL,
    .previous_mode = MODE_REMOTE_CONTROL,
    .websocket_connected = false,
    .mode_changed = true,
    .ip_addr = "Waiting ip...",
    .scrolling_text = "Scrolling text looks OK...",
    .invert_display = false,
};

static const Mode_t kModeCycle[] = {
    MODE_CLOCK,
    MODE_CLOCK_WEATHER,
    MODE_CLOCK_TEMP,
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

// Mode banner state
typedef struct {
    bool active;
    bool drawn;
    bool skip_on_next_change;
    TickType_t expire_tick;
    char text[32];
    Mode_t current_display_mode;
    bool transition_pending;
} mode_banner_state_t;

static mode_banner_state_t mode_banner = {
    .active = false,
    .drawn = false,
    .skip_on_next_change = false,
    .expire_tick = 0,
    .text = {0},
    .current_display_mode = MODE_REMOTE_CONTROL,
    .transition_pending = false,
};

// OTA display state
typedef struct {
    bool in_progress;
    bool failed;
    bool success;
    size_t written;
    size_t total;
    TickType_t hold_until;
    Mode_t previous_mode;
} ota_display_state_t;

static ota_display_state_t ota_display = {
    .in_progress = false,
    .failed = false,
    .success = false,
    .written = 0,
    .total = 0,
    .hold_until = 0,
    .previous_mode = MODE_REMOTE_CONTROL,
};

static void handleModeSolar(void);
static void handleModeClock(bool first_run);
static void handleModeAnalogClock(bool first_run);
static void handleModeClockWeather(bool first_run);
static void handleModeClockTemp(bool first_run);
static void handleModeScrollingText(bool first_run, char* text);
static void handle_preventive_maintenance(bool first_run);
static void handleModeOtaProgress(bool first_run);
static void redraw_flip_dot(uint8_t* framebuffer);
static void start_mode_banner(Mode_t new_mode);
static const char* mode_to_string(Mode_t mode);
static bool mode_is_valid(Mode_t mode);
static bool mode_is_user_selectable(Mode_t mode);
static Mode_t normalize_mode(uint32_t stored_mode);
static Mode_t step_mode(int direction);
static void draw_mode_banner(void);
static void apply_mode_selection(Mode_t requested_mode, const char* extra_arg);
static void apply_invert_setting(bool invert, bool persist);
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
        memset(display_state.ip_addr, 0, sizeof(display_state.ip_addr));
        snprintf(display_state.ip_addr, sizeof(display_state.ip_addr), IPSTR, IP2STR(&event->ip_info.ip));
        if (display_state.mode == MODE_REMOTE_CONTROL) {
            display_state.mode_changed = true; // Trigger re-draw ip addr on screen
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
        display_state.websocket_connected = true;
        // Change display_state.mode automatically when ws connects
        if (display_state.mode != MODE_REMOTE_CONTROL) {
            display_state.mode = MODE_REMOTE_CONTROL;
            mode_banner.transition_pending = true;
        }
        display_state.mode_changed = true;
        framebuffer_clear();
    } else if (event == WEBSOCKET_EVENT_DISCONNECTED) {
        display_state.websocket_connected = false;
        if (display_state.mode == MODE_REMOTE_CONTROL) {
            display_state.mode_changed = true; // Trigger re-draw of ip address
        }
    } else if (event == WEBSOCKET_EVENT_DATA) {
        if (display_state.mode == MODE_REMOTE_CONTROL) {
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

        if (display_state.invert_display != requested_invert) {
            apply_invert_setting(requested_invert, true);
            ESP_LOGI(TAG, "Display inversion %s", requested_invert ? "enabled" : "disabled");
            display_state.mode_changed = true;
            mode_banner.transition_pending = false;
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

static const char* mode_to_string(Mode_t current_mode)
{
    switch (current_mode) {
        case MODE_CLOCK:
            return "Clock D";
        case MODE_CLOCK_WEATHER:
            return "Clock+W";
        case MODE_CLOCK_TEMP:
            return "Clock+T";
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
        case MODE_CLOCK_TEMP:
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
    Mode_t reference = display_state.mode;

    if (!mode_is_user_selectable(reference)) {
        if (mode_is_user_selectable(display_state.previous_mode)) {
            reference = display_state.previous_mode;
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
    uint8_t text_width = (uint8_t)framebuffer_get_string_width(mode_banner.text, font);
    uint8_t x = 0;
    if (FRAMEBUFFER_WIDTH > text_width) {
        x = (uint8_t)((FRAMEBUFFER_WIDTH - text_width) / 2);
    }
    uint8_t y = 0;
    if (FRAMEBUFFER_HEIGHT > font->font_height) {
        y = (uint8_t)((FRAMEBUFFER_HEIGHT - font->font_height) / 2);
    }

    uint8_t* framebuffer = framebuffer_clear();
    framebuffer = framebuffer_draw_string(mode_banner.text, x, y, font, false);
    flip_dot_driver_draw(framebuffer, FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT);
    mode_banner.drawn = true;
}

static void start_mode_banner(Mode_t new_mode)
{
    const char* name = mode_to_string(new_mode);
    snprintf(mode_banner.text, sizeof(mode_banner.text), "%s", name);
    mode_banner.text[sizeof(mode_banner.text) - 1] = '\0';
    mode_banner.expire_tick = xTaskGetTickCount() + pdMS_TO_TICKS(3000);
    mode_banner.active = true;
    mode_banner.drawn = false;
    mode_banner.skip_on_next_change = false;
}

static void apply_mode_selection(Mode_t requested_mode, const char* extra_arg)
{
    nvs_handle_t nvs_handle;

    display_state.mode = requested_mode;
    display_state.mode_changed = true;
    mode_banner.transition_pending = true;

    ESP_ERROR_CHECK(nvs_open("storage", NVS_READWRITE, &nvs_handle));
    ESP_ERROR_CHECK(nvs_set_u32(nvs_handle, "mode", (uint32_t)requested_mode));

    if (requested_mode == MODE_SCROLL_TEXT && extra_arg != NULL && extra_arg[0] != '\0') {
        ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "scroll_text", extra_arg));
        strncpy(display_state.scrolling_text, extra_arg, sizeof(display_state.scrolling_text) - 1);
        display_state.scrolling_text[sizeof(display_state.scrolling_text) - 1] = '\0';
    }

    ESP_ERROR_CHECK(nvs_commit(nvs_handle));
    nvs_close(nvs_handle);
}

static void ota_status_callback(flipdot_ota_status_t status, size_t bytes_written, size_t total_bytes, void *ctx)
{
    (void)ctx;

    switch (status) {
        case FLIPDOT_OTA_STATUS_START:
            ota_display.previous_mode = display_state.mode;
            ota_display.in_progress = true;
            ota_display.failed = false;
            ota_display.success = false;
            ota_display.written = 0;
            ota_display.total = total_bytes;
            ota_display.hold_until = 0;
            mode_banner.active = false;
            mode_banner.skip_on_next_change = true;
            mode_banner.transition_pending = false;
            display_state.mode = MODE_OTA_PROGRESS;
            display_state.mode_changed = true;
            break;
        case FLIPDOT_OTA_STATUS_PROGRESS:
            ota_display.in_progress = true;
            ota_display.written = bytes_written;
            ota_display.total = total_bytes;
            mode_banner.active = false;
            mode_banner.skip_on_next_change = true;
            break;
        case FLIPDOT_OTA_STATUS_SUCCESS:
            ota_display.in_progress = false;
            ota_display.success = true;
            ota_display.failed = false;
            ota_display.written = bytes_written;
            ota_display.total = total_bytes;
            ota_display.hold_until = xTaskGetTickCount() + pdMS_TO_TICKS(1000);
            mode_banner.active = false;
            mode_banner.skip_on_next_change = true;
            mode_banner.transition_pending = false;
            display_state.mode = MODE_OTA_PROGRESS;
            display_state.mode_changed = true;
            break;
        case FLIPDOT_OTA_STATUS_FAILED:
            ota_display.in_progress = false;
            ota_display.failed = true;
            ota_display.success = false;
            ota_display.written = bytes_written;
            ota_display.total = total_bytes;
            ota_display.hold_until = xTaskGetTickCount() + pdMS_TO_TICKS(5000);
            mode_banner.active = false;
            mode_banner.skip_on_next_change = true;
            mode_banner.transition_pending = false;
            display_state.mode = MODE_OTA_PROGRESS;
            display_state.mode_changed = true;
            break;
        default:
            break;
    }
}

static void apply_invert_setting(bool invert, bool persist)
{
    display_state.invert_display = invert;
    flip_dot_driver_set_invert(invert);

    if (persist) {
        nvs_handle_t nvs_handle;
        ESP_ERROR_CHECK(nvs_open("storage", NVS_READWRITE, &nvs_handle));
        ESP_ERROR_CHECK(nvs_set_u8(nvs_handle, "invert", invert ? 1 : 0));
        ESP_ERROR_CHECK(nvs_commit(nvs_handle));
        nvs_close(nvs_handle);
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

    if (ota_display.in_progress) {
        strcpy(line1, "Updating");

        if (ota_display.total > 0) {
            uint32_t percent = (uint32_t)((ota_display.written * 100) / ota_display.total);
            if (percent > 100) {
                percent = 100;
            }
            snprintf(line2, sizeof(line2), "%3lu%%", (unsigned long)percent);
        } else {
            unsigned long kilobytes = (unsigned long)((ota_display.written + 512) / 1024);
            snprintf(line2, sizeof(line2), "%lu kB", kilobytes);
        }
    } else if (ota_display.success) {
        strcpy(line1, "Update");
        strcpy(line2, "Success");
    } else if (ota_display.failed) {
        strcpy(line1, "Update");
        strcpy(line2, "Failed");
    } else {
        ota_display.in_progress = false;
        display_state.mode = ota_display.previous_mode;
        display_state.mode_changed = true;
        mode_banner.transition_pending = true;
        mode_banner.skip_on_next_change = true;
        ota_display.written = 0;
        ota_display.total = 0;
        return;
    }

    uint8_t width1 = (uint8_t)framebuffer_get_string_width(line1, font);
    uint8_t width2 = (uint8_t)framebuffer_get_string_width(line2, font);
    uint8_t x1 = (width1 < FRAMEBUFFER_WIDTH) ? (uint8_t)((FRAMEBUFFER_WIDTH - width1) / 2) : 0;
    uint8_t x2 = (width2 < FRAMEBUFFER_WIDTH) ? (uint8_t)((FRAMEBUFFER_WIDTH - width2) / 2) : 0;

    framebuffer = framebuffer_draw_string(line1, x1, 1, font, false);
    framebuffer = framebuffer_draw_string(line2, x2, FRAMEBUFFER_HEIGHT - font->font_height - 1, font, false);

    flip_dot_driver_draw(framebuffer, FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT);

    if (!ota_display.in_progress) {
        if (ota_display.failed) {
            if ((int32_t)(ota_display.hold_until - xTaskGetTickCount()) <= 0) {
                ota_display.failed = false;
                ota_display.written = 0;
                ota_display.total = 0;
                display_state.mode = ota_display.previous_mode;
                display_state.mode_changed = true;
                mode_banner.transition_pending = true;
                mode_banner.skip_on_next_change = true;
            }
        } else if (ota_display.success) {
            if ((int32_t)(ota_display.hold_until - xTaskGetTickCount()) <= 0) {
                ota_display.success = false;
                ota_display.written = 0;
                ota_display.total = 0;
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

    bool solar_available = home_assistant_get_solar(&solar_production_watt);
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
    bool temp_available = home_assistant_get_temperature(&temperature_inside);

    strftime(strftime_buf, sizeof(strftime_buf), "%H:%M", &timeinfo);

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
    static bool mode_was_changed = false;
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
        mode_was_changed = true;
    }

    if ((int32_t)(now_ticks - next_swap_tick) >= 0) {
        show_clock = !show_clock;
        next_swap_tick = now_ticks + swap_interval_ticks;
        framebuffer_clear();
        mode_was_changed = true;
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
        if (mode_was_changed) {
            mode_was_changed = false;
            flip_dot_driver_draw_silent(framebuffer, FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT, 10000);
        }
    } else {
        float outside_temperature = 0.0f;
        float humidity = 0.0f;
        float pressure = 0.0f;
        char condition[32] = {0};
        int32_t inside_temperature = 0;

        bool inside_available = home_assistant_get_temperature(&inside_temperature);
        bool weather_available = home_assistant_get_weather(&outside_temperature, &humidity, &pressure, condition, sizeof(condition));
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

        if (mode_was_changed) {
            mode_was_changed = false;
            flip_dot_driver_draw_silent(framebuffer, FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT, 10000);
        }
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
}

static void handleModeClockTemp(bool first_run)
{
    time_t now;
    struct tm timeinfo;
    char draw_buf[32];
    uint8_t* framebuffer;
    static bool mode_was_changed = false;

    if (first_run) {
        framebuffer_clear();
        mode_was_changed = true;
    }

    time(&now);
    localtime_r(&now, &timeinfo);

    int32_t inside_temperature = 0;
    float outside_temperature = 0.0f;
    float humidity = 0.0f;
    float pressure = 0.0f;
    char condition[32] = {0};

    bool inside_available = home_assistant_get_temperature(&inside_temperature);
    bool outside_available = home_assistant_get_weather(&outside_temperature, &humidity, &pressure, condition, sizeof(condition));
    (void)humidity;
    (void)pressure;
    (void)condition;

    framebuffer = framebuffer_clear();

    // Time centered on top
    strftime(draw_buf, sizeof(draw_buf), "%H:%M", &timeinfo);
    uint8_t time_width = (uint8_t)framebuffer_get_string_width(draw_buf, &font_3x6);
    uint8_t time_x = (FRAMEBUFFER_WIDTH > time_width) ? (uint8_t)((FRAMEBUFFER_WIDTH - time_width) / 2) : 0;
    framebuffer = framebuffer_draw_string(draw_buf, time_x, 1, &font_3x6, false);

    // Inside and outside temperatures, centered in their respective halves
    font_t* temp_font = &font_3x5;
    // Place temps directly below the time, fitting within the 14px display height
    const uint8_t temp_y = font_3x6.font_height + 2;  // y=8, temps occupy y=8..12
    const uint8_t half = FRAMEBUFFER_WIDTH / 2;        // 14px per half

    char inside_line[8];
    char outside_line[8];

    if (inside_available) {
        snprintf(inside_line, sizeof(inside_line), "%ld", (long)inside_temperature);
    } else {
        strncpy(inside_line, "--", sizeof(inside_line) - 1);
        inside_line[sizeof(inside_line) - 1] = '\0';
    }

    if (outside_available) {
        snprintf(outside_line, sizeof(outside_line), "%ld", (long)lroundf(outside_temperature));
    } else {
        strncpy(outside_line, "--", sizeof(outside_line) - 1);
        outside_line[sizeof(outside_line) - 1] = '\0';
    }

    uint8_t inside_width = (uint8_t)framebuffer_get_string_width(inside_line, temp_font);
    uint8_t outside_width = (uint8_t)framebuffer_get_string_width(outside_line, temp_font);

    // Center each number in its half; place degree dot immediately after the digits
    uint8_t inside_x = (half > inside_width) ? (uint8_t)((half - inside_width) / 2) : 0;
    uint8_t outside_x = half + ((half > outside_width) ? (uint8_t)((half - outside_width) / 2) : 0);

    framebuffer = framebuffer_draw_string(inside_line, inside_x, temp_y, temp_font, false);
    framebuffer = framebuffer_draw_string(outside_line, outside_x, temp_y, temp_font, false);

    // Degree dot immediately after each number
    framebuffer = framebuffer_set_pixel_value(inside_x + inside_width + 1, temp_y, 1);
    framebuffer = framebuffer_set_pixel_value(outside_x + outside_width + 1, temp_y, 1);

    if (mode_was_changed) {
        mode_was_changed = false;
        flip_dot_driver_draw_silent(framebuffer, FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT, 10000);
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
    for (int iterations = 0; iterations < MAINTENANCE_ITERATIONS; iterations++) {
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
        display_state.mode = normalize_mode(stored_mode);
    } else {
        display_state.mode = MODE_REMOTE_CONTROL;
    }
    max_len = sizeof(display_state.scrolling_text);
    nvs_get_str(nvs_handle, "scroll_text", display_state.scrolling_text, &max_len);

    uint8_t stored_invert = 0;
    ret = nvs_get_u8(nvs_handle, "invert", &stored_invert);
    display_state.invert_display = (ret == ESP_OK) ? (stored_invert != 0) : false;

    nvs_close(nvs_handle);

    mode_banner.current_display_mode = display_state.mode;
    mode_banner.transition_pending = true;

    webserver_init(&handle_websocket_event, &handle_mode_changed);
    flipdot_ota_set_status_callback(ota_status_callback, NULL);
    start_station();

    webserver_start();
    initialise_mdns();
    initialize_sntp();

    setenv("TZ", "CET-1CEST,M3.5.0/02,M10.5.0/03", 1);
    tzset();

    flip_dot_driver_init();
    flip_dot_driver_set_invert(display_state.invert_display);
    // In case display has been off for a while
    // just flip all dots a few times to make sure none
    // are stuck.
    //handle_preventive_maintenance(true);

    ESP_LOGW(TAG, "Started and running\n");

    framebuffer_init();
    framebuffer_clear();

    home_assistant_cache_init();
    home_assistant_start_polling();

    // Convert 12 hours to ticks without overflowing inside pdMS_TO_TICKS
    const TickType_t one_hour_ticks = pdMS_TO_TICKS(60UL * 60UL * 1000UL);
    const TickType_t restart_after_ticks = one_hour_ticks * 12UL;

    while (true) {
        bool temp_mode_changed = display_state.mode_changed;
        display_state.mode_changed = false;
        bool skip_banner = mode_banner.skip_on_next_change;
        mode_banner.skip_on_next_change = false;

        if (xTaskGetTickCount() > restart_after_ticks) {
            ESP_LOGE(TAG, "Restarting");
            esp_restart();
        }


        get_time(&timeinfo);
        if (timeinfo.tm_hour == MAINTENANCE_HOUR && timeinfo.tm_min == MAINTENANCE_MINUTE) {
            if (display_state.mode != MODE_PREVENTIVE_MAINTENANCE_MODE) {
                temp_mode_changed = true;
                display_state.mode = MODE_PREVENTIVE_MAINTENANCE_MODE;
                mode_banner.transition_pending = true;
                ESP_LOGI(TAG, "Entering mainenatnce display_state.mode for one minute");
            }
        } else if ((timeinfo.tm_hour != MAINTENANCE_HOUR || timeinfo.tm_min != MAINTENANCE_MINUTE) && display_state.mode == MODE_PREVENTIVE_MAINTENANCE_MODE) {
            temp_mode_changed = true;
            display_state.mode = get_mode_nvs();
            mode_banner.transition_pending = true;
            ESP_LOGI(TAG, "Leaving mainenatnce mode");
        }

        if (skip_banner) {
            temp_mode_changed = true;
            mode_banner.transition_pending = false;
        }

        if (temp_mode_changed && !skip_banner && mode_banner.transition_pending) {
            start_mode_banner(display_state.mode);
            mode_banner.transition_pending = false;
        }

        if (mode_banner.active) {
            if (!mode_banner.drawn || temp_mode_changed) {
                draw_mode_banner();
            }

            TickType_t now_ticks = xTaskGetTickCount();
            if ((int32_t)(mode_banner.expire_tick - now_ticks) > 0) {
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }

            mode_banner.active = false;
            mode_banner.skip_on_next_change = true;
            display_state.mode_changed = true;
            continue;
        }

        ESP_LOGI(TAG, "Mode: %d", display_state.mode);

        switch (display_state.mode) {
            case MODE_CLOCK:
                handleModeClock(temp_mode_changed);
                break;
            case MODE_CLOCK_WEATHER:
                handleModeClockWeather(temp_mode_changed);
                break;
            case MODE_CLOCK_TEMP:
                handleModeClockTemp(temp_mode_changed);
                break;
            case MODE_SCROLL_TEXT:
                handleModeScrollingText(temp_mode_changed, display_state.scrolling_text);
                break;
            case MODE_REMOTE_CONTROL:
                if (temp_mode_changed && !display_state.websocket_connected) {
                    framebuffer_clear();
                    framebuffer = framebuffer_draw_string(display_state.ip_addr, 0, 0, &font_3x6, true);
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

        mode_banner.current_display_mode = display_state.mode;
    }
}
