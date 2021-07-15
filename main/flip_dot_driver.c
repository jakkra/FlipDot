#include <inttypes.h>
#include "flip_dot_driver.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include <string.h>

#define TAG "FLIP_DOT_DRIVER"

// Note: Some pins on target chip cannot be assigned for UART communication.
// Please refer to documentation for selected board and target to configure pins using Kconfig.
#define ECHO_TEST_TXD   (CONFIG_RS485_UART_TXD)
#define ECHO_TEST_RXD   (CONFIG_RS485_UART_RXD)

// RTS for RS485 Half-Duplex Mode manages DE/~RE
#define ECHO_TEST_RTS   (CONFIG_RS485_UART_RTS)

// CTS is not used in RS485 Half-Duplex Mode
#define ECHO_TEST_CTS   (UART_PIN_NO_CHANGE)

#define BUF_SIZE        (127)
#define BAUD_RATE       (CONFIG_RS485_UART_BAUD_RATE)

// Read packet timeout
#define PACKET_READ_TICS        (100 / portTICK_RATE_MS)
#define ECHO_TASK_STACK_SIZE    (2048)
#define ECHO_TASK_PRIO          (10)
#define ECHO_UART_PORT          (CONFIG_RS485_UART_PORT_NUM)

// Timeout threshold for UART = number of symbols (~10 tics) with unchanged state on receive pin
#define ECHO_READ_TOUT          (3) // 3.5T * 8 = 28 ticks, TOUT=3 -> ~24..33 ticks

#define DATA_LENGTH             32

uint8_t all_bright[]= {0x80, 0x83, 0xFF, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x8F};
uint8_t all_dark[]= {0x80, 0x83, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x8F};
uint8_t test[]= {0x80, 0x83, 0xFF, 0x00, 0x7F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x8F};

static const int uart_num = ECHO_UART_PORT;



static void send_to_flip_dot(const int port, uint8_t* data, uint8_t length)
{
    //ESP_LOG_BUFFER_HEXDUMP(TAG, data, length, ESP_LOG_WARN);
    if (uart_write_bytes(port, (void*)data, length) != length) {
        ESP_LOGE(TAG, "Send data critical failure.");
        abort();
    }
}

void flip_dot_driver_init(void)
{
    uart_config_t uart_config = {
        .baud_rate = BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 122,
        .source_clk = UART_SCLK_APB,
    };

    // Set UART log level
    esp_log_level_set(TAG, ESP_LOG_DEBUG);
    printf("BAUD:%d, TX PIN: %d, len: %d\n", BAUD_RATE, ECHO_TEST_TXD, sizeof(all_dark));
    ESP_LOGI(TAG, "Start RS485 application test and configure UART.");

    // Install UART driver (we don't need an event queue here)
    // In this example we don't even use a buffer for sending data.
    ESP_ERROR_CHECK(uart_driver_install(uart_num, BUF_SIZE * 2, 0, 0, NULL, 0));

    // Configure UART parameters
    ESP_ERROR_CHECK(uart_param_config(uart_num, &uart_config));

    ESP_LOGI(TAG, "UART set pins, mode and install driver.");

    // Set UART pins as per KConfig settings
    ESP_ERROR_CHECK(uart_set_pin(uart_num, ECHO_TEST_TXD, UART_PIN_NO_CHANGE , UART_PIN_NO_CHANGE , UART_PIN_NO_CHANGE ));

    // Set RS485 half duplex mode
    ESP_ERROR_CHECK(uart_set_mode(uart_num, UART_MODE_UART ));

    // Set read timeout of UART TOUT feature
    ESP_ERROR_CHECK(uart_set_rx_timeout(uart_num, ECHO_READ_TOUT));
}

void flip_dot_driver_all_on(void)
{
    send_to_flip_dot(uart_num, all_bright, sizeof(all_bright));
}

void flip_dot_driver_all_off(void)
{
    send_to_flip_dot(uart_num, all_dark, sizeof(all_dark));
}

void flip_dot_driver_draw(uint8_t* data, uint32_t len)
{
    uint8_t display1[28];
    memset(display1, 0, sizeof(display1));
    uint8_t display2[28];
    memset(display2, 0, sizeof(display2));
    int row = 0;
    int col = 0;
    uint8_t addr1 = 0x15;
    uint8_t addr2 = 0x17;

    uint8_t buffer[DATA_LENGTH];
    memset(buffer, 0, sizeof(buffer));
    buffer[0] = 0x80;
    buffer[1] = 0x83;
    buffer[2] = addr1;
    buffer[DATA_LENGTH - 1] = 0x8F;
    /*
    for (int i = 0; i < len; i++) {
        if (i > 0 && i % 28 == 0) {
            row++;
            col = 0;
            printf("\n");
        }
        printf("%d ", data[i]);
        col++;
    }
    printf("\n\n");
    */
    row = 0;
    col = 0;
    for (int i = 0; i < len; i++) {
        if (i > 0 && i % 28 == 0) {
            row++;
            col = 0;
        }
        if (data[i] != 0) {
            if (row < 7) {
                display1[col] |= 1 << row;
            } else {
                display2[col] |= 1 << (row - 7);
            }
        }
        col++;
    }
    memcpy(&buffer[3], display1, sizeof(display1));
    send_to_flip_dot(uart_num, buffer, sizeof(buffer));
    buffer[2] = addr2;
    memcpy(&buffer[3], display2, sizeof(display2));
    send_to_flip_dot(uart_num, buffer, sizeof(buffer));
}
