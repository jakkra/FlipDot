#include <stdio.h>
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

#include "web_server.h"
#include "flip_dot_driver.h"
#include "angle_input.h"
#include "esp_sntp.h"

static char TAG[] = "FlipDot";

#define USE_STATION
#define ANGFLE_BUFFER_SIZE 10
#define OLD_ANGLE_LIMIT_MS 3000


static bool websocket_connected = false;
static int8_t angle_buffer[2][ANGFLE_BUFFER_SIZE];
static int angle_buffer_index = 0;

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    switch (event_id) {
        case WIFI_EVENT_AP_START:
            break;
        case WIFI_EVENT_AP_STOP:
            break;
        case WIFI_EVENT_AP_STACONNECTED:
        {
            wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
            ESP_LOGW(TAG, "station "MACSTR" join, AID=%d", MAC2STR(event->mac), event->aid);
            break;
        }
        case WIFI_EVENT_AP_STADISCONNECTED:
        {
            wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
            ESP_LOGW(TAG, "station "MACSTR" leave, AID=%d", MAC2STR(event->mac), event->aid);
            break;
        }
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "Failed to connect WiFi");
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
    }
}
#ifdef CONFIG_WIFI_MODE_AP
static void start_ap(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = CONFIG_AP_SSID,
            .ssid_len = strlen(CONFIG_AP_SSID),
            .password = CONFIG_AP_PASS,
            .max_connection = 1,
            .authmode = WIFI_AUTH_WPA2_PSK
        },
    };
    if (strlen(CONFIG_AP_PASS) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_softap finished. SSID:%s password:%s", CONFIG_AP_SSID, CONFIG_AP_PASS);
}
#endif

#ifdef CONFIG_WIFI_MODE_STATION
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

    ESP_LOGI(TAG, "WiFi Sta Started");
}
#endif

static void handle_websocket_event(websocket_event_t event, uint8_t* data, uint32_t len) {
    if (event == WEBSOCKET_EVENT_CONNECTED) {
        websocket_connected = true;
    } else if (event == WEBSOCKET_EVENT_DISCONNECTED) {
        websocket_connected = false;
    } else if (event == WEBSOCKET_EVENT_DATA) {
        printf("Got data length: %d\n", len);
        flip_dot_driver_draw(data, len);
    } else {
        assert(false); // Unhandled
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

uint32_t map(uint32_t x, uint32_t in_min, uint32_t in_max, uint32_t out_min, uint32_t out_max) {
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}


static uint8_t frameBuffer[14][28];
static uint32_t frameBufferTime[14][28];

int cmpfunc (const void * a, const void * b) {
   return ( *(int8_t*)a - *(int8_t*)b );
}

static void angle_callback(int8_t azimuth, int8_t elevation)
{
    uint32_t ms_now = esp_timer_get_time() / 1000;
    azimuth = azimuth * -1;
    if (azimuth > 45) {
        azimuth = 45;
    }
    if (azimuth < -45) {
        azimuth = -45;
    }

    if (elevation > 45) {
        elevation = 45;
    }
    if (elevation < -45) {
        elevation = -45;
    }

    angle_buffer[0][angle_buffer_index % ANGFLE_BUFFER_SIZE] = azimuth;
    angle_buffer[1][angle_buffer_index % ANGFLE_BUFFER_SIZE] = elevation;

    qsort(&angle_buffer[0], ANGFLE_BUFFER_SIZE, sizeof(int8_t), cmpfunc);
    qsort(&angle_buffer[1], ANGFLE_BUFFER_SIZE, sizeof(int8_t), cmpfunc);
    angle_buffer_index++;
    printf("%d, %d, %d\n", ms_now, angle_buffer[0][ANGFLE_BUFFER_SIZE / 2], angle_buffer[1][ANGFLE_BUFFER_SIZE / 2]);
    frameBuffer[map(angle_buffer[1][ANGFLE_BUFFER_SIZE / 2], -45, 45, 0, 13)][map(angle_buffer[0][ANGFLE_BUFFER_SIZE / 2], -45, 45, 0, 27)] = 1;
    frameBufferTime[map(angle_buffer[1][ANGFLE_BUFFER_SIZE / 2], -45, 45, 0, 13)][map(angle_buffer[0][ANGFLE_BUFFER_SIZE / 2], -45, 45, 0, 27)] = ms_now;
    flip_dot_driver_draw((uint8_t*)frameBuffer, 28 * 14);

    for (int i = 0; i < 14; i++) {
        for (int j = 0; j < 28; j++) {
            if (frameBufferTime[i][j] + OLD_ANGLE_LIMIT_MS < ms_now) {
                frameBuffer[i][j] = 0;
            }
        }
    }
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

#define BUF_SIZE (1024)

#define UART_PORT_NUM   1

static angle_event_callback* pCallback;

static void sntp_sync_time_thread(void *arg)
{
    time_t now;
    uint8_t framebuffer[14][28];
    char strftime_buf[64];
    struct tm timeinfo;
    int retry = 0;
    
    while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET) {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d)", retry);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }
    setenv("TZ", "CET-1CEST", 1);
    tzset();

    while(1) {
        memset(framebuffer, 0, sizeof(framebuffer));
        time(&now);
        localtime_r(&now, &timeinfo);
        strftime(strftime_buf, sizeof(strftime_buf), "%X", &timeinfo);
        ESP_LOGI(TAG, "The current date/time in Sweden is: %s", strftime_buf);

        flip_dot_driver_print_character(strftime_buf[0] -  '0', 0, framebuffer);
        flip_dot_driver_print_character(strftime_buf[1] -  '0', 4, framebuffer);
        flip_dot_driver_print_character(strftime_buf[3] -  '0', 8, framebuffer);
        flip_dot_driver_print_character(strftime_buf[4] -  '0', 12, framebuffer);
        flip_dot_driver_print_character(strftime_buf[6] -  '0', 16, framebuffer);
        flip_dot_driver_print_character(strftime_buf[7] -  '0', 20, framebuffer);

        flip_dot_driver_draw(framebuffer, sizeof(framebuffer));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void app_main() {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    webserver_init(&handle_websocket_event);
#ifdef CONFIG_WIFI_MODE_STATION
    start_station();
#endif
#ifdef CONFIG_WIFI_MODE_AP
    start_ap();
#endif
    webserver_start();
    initialise_mdns();
    initialize_sntp();

    flip_dot_driver_init();
    flip_dot_driver_all_off();
    ESP_LOGW(TAG, "Started and running\n");
    memset(angle_buffer, 0, sizeof(angle_buffer));
    memset(frameBuffer, 0, sizeof(frameBuffer));
    memset(frameBufferTime, 0, sizeof(frameBufferTime));
    //angle_input_init(angle_callback);

    //flip_dot_driver_test();
    xTaskCreate(sntp_sync_time_thread, "sntp_sync_time_thread", 2048, NULL, 10, NULL);
}


