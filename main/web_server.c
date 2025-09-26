#include "web_server.h"
#include "flip_dot_driver.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_netif.h"
#include <esp_system.h>
#include <esp_event.h>
#include "lwip/err.h"
#include "lwip/sys.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "string.h"
#include <stdint.h>
#include <ctype.h>

#define WS_SERVER_PORT          80
#define MAX_WS_INCOMING_SIZE    28*14 // TODO don't hardcode
#define MAX_WS_CONNECTIONS      5
#define MAX_HTTP_RSP_LEN        128
#define MAX_HTTP_REQ_LEN        128
#define INVALID_FD              -1
#define MAX_TX_BUF_SIZE         512

typedef struct web_server {
    httpd_handle_t                  handle;
    int                             sockfd;
    bool                            running;
    uint8_t                         tx_buf[MAX_TX_BUF_SIZE];
    uint16_t                        tx_buf_len;
    websocket_callback*             ws_callback;
    mode_change_callback*           mode_callback;
    bool                            client_connected;
    esp_timer_handle_t              failsafe_timer;
    bool                            tx_in_progress;
} web_server;

static esp_err_t on_client_connected(httpd_handle_t hd, int sockfd);
static void on_client_disconnect(httpd_handle_t hd, int sockfd);
static void failsafe_timer_callback(void* arg);
static esp_err_t ws_handler(httpd_req_t *req);
static esp_err_t mode_change_handler(httpd_req_t *req);
static esp_err_t alert_handler(httpd_req_t *req);
static esp_err_t mode_next_handler(httpd_req_t *req);
static esp_err_t mode_prev_handler(httpd_req_t *req);
static esp_err_t invert_handler(httpd_req_t *req);
static void async_send(void *arg);
static bool parse_bool_param(const char* value, bool* out_value);

static const httpd_uri_t ws = {
    .uri        = "/ws",
    .method     = HTTP_GET,
    .handler    = ws_handler,
    .user_ctx   = NULL,
    .is_websocket = true
};

static const httpd_uri_t mode_get = {
    .uri       = "/mode",
    .method    = HTTP_GET,
    .handler   = mode_change_handler,
};

static const httpd_uri_t mode_next_get = {
    .uri       = "/mode/next",
    .method    = HTTP_GET,
    .handler   = mode_next_handler,
};

static const httpd_uri_t mode_prev_get = {
    .uri       = "/mode/prev",
    .method    = HTTP_GET,
    .handler   = mode_prev_handler,
};

static const httpd_uri_t alert_get = {
    .uri       = "/alert",
    .method    = HTTP_GET,
    .handler   = alert_handler,
};

static const httpd_uri_t invert_get = {
    .uri       = "/display/invert",
    .method    = HTTP_GET,
    .handler   = invert_handler,
};

static const char *TAG = "ws_server";

static web_server server;


void webserver_init(websocket_callback* ws_cb, mode_change_callback mode_cb)
{
    memset(&server, 0, sizeof(web_server));
    server.running = false;
    ESP_LOGI(TAG, "webserver_init");
    server.handle = NULL;
    server.client_connected = false;
    server.ws_callback = ws_cb;
    server.mode_callback = mode_cb;
}

void webserver_start(void)
{
    assert(!server.running);
    ESP_LOGI(TAG, "webserver_start");
    esp_err_t err;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    config.server_port = WS_SERVER_PORT;
    config.close_fn = on_client_disconnect;
    config.open_fn = NULL; // Not for the WS connection but for the HTTP. So can't be used for WS connected unfortunately.
    config.max_open_sockets = MAX_WS_CONNECTIONS;
    err = httpd_start(&server.handle, &config);
    assert(err == ESP_OK);

    err = httpd_register_uri_handler(server.handle, &ws);
    assert(err == ESP_OK);
    err = httpd_register_uri_handler(server.handle, &mode_get);
    assert(err == ESP_OK);
    err = httpd_register_uri_handler(server.handle, &mode_next_get);
    assert(err == ESP_OK);
    err = httpd_register_uri_handler(server.handle, &mode_prev_get);
    assert(err == ESP_OK);
    err = httpd_register_uri_handler(server.handle, &alert_get);
    assert(err == ESP_OK);
    err = httpd_register_uri_handler(server.handle, &invert_get);
    assert(err == ESP_OK);

    const esp_timer_create_args_t failsafe_timer_args = {
            .callback = &failsafe_timer_callback,
            .arg = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "failsafe-timer"
    };
    ESP_ERROR_CHECK(esp_timer_create(&failsafe_timer_args, &server.failsafe_timer));

    server.running = true;
    ESP_LOGI(TAG, "Web Server started on port %d, server handle %p", config.server_port, server.handle);    
}

esp_err_t webserver_ws_send(uint8_t* payload, uint32_t len) {
    esp_err_t err;
    assert(len <= MAX_TX_BUF_SIZE);

    if (server.tx_in_progress) {
        return ESP_FAIL;
    }
    server.tx_in_progress = true;
    server.tx_buf_len = len;
    memset(server.tx_buf, 0, MAX_TX_BUF_SIZE);
    memcpy(server.tx_buf, payload, len);
    err = httpd_queue_work(server.handle, async_send, NULL);
    if (err != ESP_OK) {
        server.tx_in_progress = false;
    }
    return err;
}

static void async_send(void *arg)
{
    esp_err_t err;
    httpd_ws_frame_t packet;

    memset(&packet, 0, sizeof(httpd_ws_frame_t));
    packet.payload = server.tx_buf;
    packet.len = server.tx_buf_len;
    packet.type = HTTPD_WS_TYPE_TEXT;
    packet.final = true;

    err = httpd_ws_send_frame_async(server.handle, server.sockfd, &packet);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "httpd_ws_send_frame_async failed: %d", err);
    }
    server.tx_in_progress = false;
}

static esp_err_t on_client_connected(httpd_handle_t hd, int sockfd)
{
    server.client_connected = true;
    server.handle = hd;
    server.sockfd = sockfd;
    server.ws_callback(WEBSOCKET_EVENT_CONNECTED, NULL, 0);
    //ESP_ERROR_CHECK(esp_timer_start_once(server.failsafe_timer, 5000 * 1000));
    return ESP_OK;
}

static void on_client_disconnect(httpd_handle_t hd, int sockfd)
{
    if (server.sockfd != sockfd) {
        return;
    }

    ESP_LOGI(TAG, "WS Client disconnected");
    server.client_connected = false;
    esp_timer_stop(server.failsafe_timer);
    server.ws_callback(WEBSOCKET_EVENT_DISCONNECTED, NULL, 0);
}

static void failsafe_timer_callback(void* arg)
{
    ESP_LOGE(TAG, "No data on WS in 1s, reset values to default");
    httpd_sess_trigger_close(server.handle, server.sockfd);
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    assert(server.handle == req->handle);
    uint8_t buf[MAX_WS_INCOMING_SIZE] = { 0 };
    httpd_ws_frame_t packet;
    
    memset(&packet, 0, sizeof(httpd_ws_frame_t));
    esp_err_t ret = httpd_ws_recv_frame(req, &packet, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_recv_frame failed with %d", ret);
        return ret;
    }

    if (packet.len > MAX_WS_INCOMING_SIZE) {
        ESP_LOGE(TAG, "Incoming WS frame too large: %u", packet.len);
        return ESP_ERR_INVALID_SIZE;
    }

    packet.payload = buf;
    ret = httpd_ws_recv_frame(req, &packet, packet.len);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_recv_frame payload read failed with %d", ret);
        return ret;
    }

    if (packet.type == HTTPD_WS_TYPE_BINARY) {
        if (packet.len == MAX_WS_INCOMING_SIZE) {
            server.ws_callback(WEBSOCKET_EVENT_DATA, packet.payload, packet.len);
            if (server.client_connected) {
                esp_timer_stop(server.failsafe_timer);
                //ESP_ERROR_CHECK(esp_timer_start_once(server.failsafe_timer, 5000 * 1000));
            } else {
                on_client_connected(req->handle, httpd_req_to_sockfd(req));
            }
        } else {
            ESP_LOGI(TAG, "Invalid binary length");
        }
    }
   
    return ESP_OK;
}

static esp_err_t alert_handler(httpd_req_t *req)
{
    char message[MAX_HTTP_REQ_LEN] = {0};
    size_t query_len = httpd_req_get_url_query_len(req);

    if (query_len > 0) {
        char query[MAX_HTTP_REQ_LEN] = {0};
        if (query_len >= sizeof(query)) {
            query_len = sizeof(query) - 1;
        }
        if (httpd_req_get_url_query_str(req, query, query_len + 1) == ESP_OK) {
            if (httpd_query_key_value(query, "msg", message, sizeof(message)) != ESP_OK) {
                message[0] = '\0';
            }
        }
    }

    for (char* c = message; *c != '\0'; ++c) {
        if (*c == '+') {
            *c = ' ';
        }
    }

    if (message[0] == '\0') {
        strncpy(message, "Alert", sizeof(message) - 1);
        message[sizeof(message) - 1] = '\0';
    }

    server.mode_callback(MODE_COMMAND_ALERT, message);

    httpd_resp_set_type(req, "application/json");
    const char resp[] = "{\"status\":\"accepted\"}";
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

static bool parse_bool_param(const char* value, bool* out_value)
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

    char lowered[8];
    if (len >= sizeof(lowered)) {
        len = sizeof(lowered) - 1;
    }
    for (size_t i = 0; i < len; i++) {
        lowered[i] = (char)tolower((unsigned char)value[i]);
    }
    lowered[len] = '\0';

    if (strcmp(lowered, "true") == 0 || strcmp(lowered, "on") == 0 || strcmp(lowered, "yes") == 0) {
        *out_value = true;
        return true;
    }
    if (strcmp(lowered, "false") == 0 || strcmp(lowered, "off") == 0 || strcmp(lowered, "no") == 0) {
        *out_value = false;
        return true;
    }

    return false;
}

static esp_err_t mode_next_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    server.mode_callback(MODE_COMMAND_NEXT, NULL);
    const char resp[] = "{\"status\":\"accepted\"}";
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

static esp_err_t mode_prev_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    server.mode_callback(MODE_COMMAND_PREV, NULL);
    const char resp[] = "{\"status\":\"accepted\"}";
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

static esp_err_t invert_handler(httpd_req_t *req)
{
    char enabled_param[8] = {0};
    bool has_value = false;
    bool requested_state = false;

    size_t query_len = httpd_req_get_url_query_len(req);
    if (query_len > 0) {
        char query[MAX_HTTP_REQ_LEN] = {0};
        if (query_len >= sizeof(query)) {
            query_len = sizeof(query) - 1;
        }
        if (httpd_req_get_url_query_str(req, query, query_len + 1) == ESP_OK) {
            if (httpd_query_key_value(query, "enabled", enabled_param, sizeof(enabled_param)) == ESP_OK ||
                httpd_query_key_value(query, "value", enabled_param, sizeof(enabled_param)) == ESP_OK) {
                has_value = true;
                if (!parse_bool_param(enabled_param, &requested_state)) {
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid enabled value");
                    return ESP_OK;
                }
                enabled_param[0] = requested_state ? '1' : '0';
                enabled_param[1] = '\0';
            }
        }
    }

    if (has_value) {
        server.mode_callback(MODE_COMMAND_SET_INVERT, enabled_param);
    } else {
        bool new_state = !flip_dot_driver_get_invert();
        enabled_param[0] = new_state ? '1' : '0';
        enabled_param[1] = '\0';
        server.mode_callback(MODE_COMMAND_SET_INVERT, enabled_param);
    }

    bool current_state = flip_dot_driver_get_invert();
    httpd_resp_set_type(req, "application/json");
    char resp[48];
    snprintf(resp, sizeof(resp), "{\"inverted\":%s}", current_state ? "true" : "false");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

static esp_err_t mode_change_handler(httpd_req_t *req)
{
    char*  buf;
    size_t buf_len;
    esp_err_t status = ESP_FAIL;
    char param[4];
    uint32_t mode = -1;
    char resp[MAX_HTTP_RSP_LEN];
    char text[MAX_HTTP_REQ_LEN];
    memset(text, 0, sizeof(text));
    memset(resp, 0, sizeof(resp));

    buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        buf = malloc(buf_len);
        assert(buf != NULL);
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            ESP_LOGI(TAG, "Found URL query => %s", buf);
            if (httpd_query_key_value(buf, "mode", param, sizeof(param)) == ESP_OK) {
                errno = 0;
                mode = strtol(param, NULL, 10);
                if (errno == 0) {
                    status = ESP_OK;
                }
            }
            if (httpd_query_key_value(buf, "text", text, sizeof(text)) == ESP_OK) {
                // Replace '%20' with a space as the text is sent as a query parameter
                int len = strlen(text);
                int numSpaces = 0;
                for (int i = 0; i < len; i++) {
                    if (text[i] == '%' && text[i + 1] == '2' && text[i + 2] == '0') {
                        text[i] = ' ';
                        memmove(&text[i + 1], &text[i + 3], len - i - 2 * numSpaces);
                        numSpaces++;
                    }
                }
            }
        }
        free(buf);
    }

    if (status == ESP_OK) {
        snprintf(resp, sizeof(resp), "{\"mode\": \"%ld\"}", mode);
        httpd_resp_send(req, resp, strlen(resp));
        server.mode_callback(mode, text);
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid params");
    }

    return ESP_OK;
}
