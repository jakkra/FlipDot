#include <inttypes.h>
#include "flip_dot_driver.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include <string.h>
#include <stdbool.h>
#include "sdkconfig.h"

#define TAG "FLIP_DOT_DRIVER"

#define BUF_SIZE        (127)
#define BAUD_RATE       (CONFIG_RS485_UART_BAUD_RATE)


#define DATA_LENGTH             32

uint8_t all_bright[]= {0x80, 0x83, 0xFF, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x8F};
uint8_t all_dark[]= {0x80, 0x83, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x8F};
uint8_t test[]= {0x80, 0x83, 0xFF, 0x00, 0x7F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x8F};

static const int uart_num = CONFIG_RS485_UART_PORT_NUM;

static bool invert_pixels = false;

#if CONFIG_FLIP_DOT_DEBUG_UART_OUTPUT
static void log_display_frame(const uint8_t display1[28], const uint8_t display2[28])
{
    static uint32_t frame_counter = 0;
    const uint32_t frame_id = frame_counter++;

    static const char hex_digits[] = "0123456789ABCDEF";
    char hex_buffer[56 * 2 + 1] = {0};

    for (int i = 0; i < 56; ++i) {
        const uint8_t value = (i < 28) ? display1[i] : display2[i - 28];
        const int idx = i * 2;
        hex_buffer[idx] = hex_digits[value >> 4];
        hex_buffer[idx + 1] = hex_digits[value & 0x0F];
    }

    ESP_LOGI(TAG, "DISPLAY_FRAME:%" PRIu32 ":%s", frame_id, hex_buffer);
}
#endif

void flip_dot_driver_set_invert(bool invert)
{
    invert_pixels = invert;
}

bool flip_dot_driver_get_invert(void)
{
    return invert_pixels;
}


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

    esp_log_level_set(TAG, ESP_LOG_DEBUG);
    printf("BAUD:%d, TX PIN: %d, len: %d\n", BAUD_RATE, CONFIG_RS485_UART_TXD, sizeof(all_dark));
    ESP_LOGI(TAG, "Start RS485 application test and configure UART.");

    ESP_ERROR_CHECK(uart_driver_install(uart_num, BUF_SIZE * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(uart_num, &uart_config));

    ESP_LOGI(TAG, "UART set pins, mode and install driver.");
    ESP_ERROR_CHECK(uart_set_pin(uart_num, CONFIG_RS485_UART_TXD, UART_PIN_NO_CHANGE , UART_PIN_NO_CHANGE , UART_PIN_NO_CHANGE ));
    ESP_ERROR_CHECK(uart_set_mode(uart_num, UART_MODE_UART ));
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
    uint8_t display1[28] = {0};
    uint8_t display2[28] = {0};
    
    uint8_t buffer[DATA_LENGTH] = {0x80, 0x83, 0x15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x8F};
    
    // Process pixels in chunks of 28 (one row)
    uint32_t pixels_to_process = len < 392 ? len : 392; // 28*14 max pixels
    
    for (uint32_t i = 0; i < pixels_to_process; i++) {
        uint8_t row = i / 28;
        uint8_t col = i % 28;
        
        bool pixel_on = (data[i] != 0) ^ invert_pixels;
        
        if (pixel_on) {
            if (row < 7) {
                display1[col] |= 1 << row;
            } else {
                display2[col] |= 1 << (row - 7);
            }
        }
    }
    
    // Send display1
    memcpy(&buffer[3], display1, 28);
#if CONFIG_FLIP_DOT_DEBUG_UART_OUTPUT
    log_display_frame(display1, display2);
#endif
    send_to_flip_dot(uart_num, buffer, DATA_LENGTH);

    // Send display2
    buffer[2] = 0x17;
    memcpy(&buffer[3], display2, 28);
    send_to_flip_dot(uart_num, buffer, DATA_LENGTH);
}
