#include "angle_input.h"
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "sdkconfig.h"

#define BUF_SIZE (1024)

#define UART_PORT_NUM   1

static angle_event_callback* pCallback;

static void uart_rx_task(void *arg)
{
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };
    int intr_alloc_flags = 0;


    ESP_ERROR_CHECK(uart_driver_install(UART_PORT_NUM, BUF_SIZE * 2, 0, 0, NULL, intr_alloc_flags));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT_NUM, UART_PIN_NO_CHANGE, 14, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    // Configure a temporary buffer for the incoming data
    uint8_t *data = (uint8_t *) malloc(BUF_SIZE);

    while (1) {
        // Read data from the UART
        int len = uart_read_bytes(UART_PORT_NUM, data, BUF_SIZE, 20 / portTICK_RATE_MS);
        if (len > 10) {
            data[len] = '\0';
            //printf("%s", data);
            int num_commas = 0;
            int azimuth = -1;
            int elevation = -1;
            for (int i = 0; i < len; i++) {
                if (data[i] == ',') {
                    num_commas++;
                    if (num_commas == 2) {
                        azimuth = atoi((char*)&data[i + 1]);
                    }
                    if (num_commas == 3) {
                        elevation = atoi((char*)&data[i + 1]);
                    }
                }
            }
            pCallback(azimuth, elevation);
        }
    }
}

void angle_input_init(angle_event_callback* cb)
{
    pCallback = cb;
    xTaskCreate(uart_rx_task, "uart_uart_rx_task", 2048, NULL, 10, NULL);
}