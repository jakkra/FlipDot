#include "web_server.h"
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
static void async_send(void *arg);

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

    const esp_timer_create_args_t failsafe_timer_args = {
            .callback = &failsafe_timer_callback,
            .arg = NULL,
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
    packet.payload = buf;

    esp_err_t ret = httpd_ws_recv_frame(req, &packet, MAX_WS_INCOMING_SIZE);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_recv_frame failed with %d", ret);
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

static esp_err_t mode_change_handler(httpd_req_t *req)
{
    char*  buf;
    size_t buf_len;
    char param[4];
    char resp[MAX_HTTP_RSP_LEN];
    uint32_t mode = -1;
    esp_err_t status = ESP_FAIL;
    
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
        }
        free(buf);
    }

    if (status == ESP_OK) {
        snprintf(resp, sizeof(resp), "{\"mode\": \"%d\"}", mode);
        httpd_resp_send(req, resp, strlen(resp));
        server.mode_callback(mode);
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid params");
    }

    return ESP_OK;
}
