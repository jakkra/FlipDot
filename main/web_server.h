#pragma once

#include "stdint.h"
#include "esp_err.h"

typedef enum websocket_event_t
{
  WEBSOCKET_EVENT_CONNECTED,
  WEBSOCKET_EVENT_DISCONNECTED,
  WEBSOCKET_EVENT_DATA
} websocket_event_t;

typedef void(websocket_callback(websocket_event_t status, uint8_t* data, uint32_t len));
typedef void(mode_change_callback(uint32_t mode, char* extra_arg));


void webserver_init(websocket_callback* ws_cb, mode_change_callback mode_cb);
void webserver_start(void);
uint16_t web_server_controller_get_value(uint8_t channel);
esp_err_t webserver_ws_send(uint8_t* payload, uint32_t len);

