#include <inttypes.h>
#include "flip_dot_driver.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_random.h"
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
static uint8_t previous_frame[392] = {0}; // Store previous frame for silent transitions
static bool previous_frame_valid = false;

// Helper function to send a complete frame
static void send_complete_frame(const uint8_t* data, uint32_t len);

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
    // Clear previous frame when doing all off
    memset(previous_frame, 0, sizeof(previous_frame));
    previous_frame_valid = true;
}

// Helper function to send a complete frame (reuses logic from flip_dot_driver_draw)
static void send_complete_frame(const uint8_t* data, uint32_t len)
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
    
    // Store current frame for future silent transitions
    uint32_t pixels_to_store = len < 392 ? len : 392;
    memcpy(previous_frame, data, pixels_to_store);
    if (pixels_to_store < 392) {
        memset(&previous_frame[pixels_to_store], 0, 392 - pixels_to_store);
    }
    previous_frame_valid = true;
}

void flip_dot_driver_draw_silent(uint8_t* data, uint32_t len, uint32_t max_duration_ms)
{
    // If no previous frame or no changes needed, just do normal draw
    if (!previous_frame_valid) {
        flip_dot_driver_draw(data, len);
        return;
    }
    
    uint32_t pixels_to_process = len < 392 ? len : 392;
    
    // Find all pixels that need to change
    uint16_t changed_pixels[392];
    uint32_t changed_count = 0;
    
    for (uint32_t i = 0; i < pixels_to_process; i++) {
        bool current_pixel = (data[i] != 0) ^ invert_pixels;
        bool previous_pixel = (previous_frame[i] != 0) ^ invert_pixels;
        
        if (current_pixel != previous_pixel) {
            changed_pixels[changed_count++] = i;
        }
    }
    
    // If few changes (< 5% of total pixels), update immediately
    if (changed_count < 20) {
        flip_dot_driver_draw(data, len);
        return;
    }
    
    ESP_LOGI(TAG, "Silent transition: %" PRIu32 " pixels changing over %" PRIu32 "ms", changed_count, max_duration_ms);
    
    // Calculate update parameters
    const uint32_t min_steps = 5;
    const uint32_t max_steps = 50;
    const uint32_t step_duration_ms = 100; // Minimum time between updates
    
    uint32_t total_steps = max_duration_ms / step_duration_ms;
    if (total_steps < min_steps) total_steps = min_steps;
    if (total_steps > max_steps) total_steps = max_steps;
    
    uint32_t pixels_per_step = (changed_count + total_steps - 1) / total_steps; // Round up
    if (pixels_per_step == 0) pixels_per_step = 1;
    
    // Create working buffer starting from previous frame
    uint8_t working_frame[392];
    memcpy(working_frame, previous_frame, sizeof(working_frame));
    
    // Randomize the order of pixel changes to avoid patterns
    for (uint32_t i = changed_count - 1; i > 0; i--) {
        uint32_t j = esp_random() % (i + 1);
        uint16_t temp = changed_pixels[i];
        changed_pixels[i] = changed_pixels[j];
        changed_pixels[j] = temp;
    }
    
    // Apply changes in steps
    uint32_t pixels_updated = 0;
    uint32_t step = 0;
    
    while (pixels_updated < changed_count && step < total_steps) {
        // Update pixels for this step
        uint32_t pixels_this_step = pixels_per_step;
        if (pixels_updated + pixels_this_step > changed_count) {
            pixels_this_step = changed_count - pixels_updated;
        }
        
        // Apply pixel changes to working frame
        for (uint32_t i = 0; i < pixels_this_step; i++) {
            uint16_t pixel_idx = changed_pixels[pixels_updated + i];
            working_frame[pixel_idx] = data[pixel_idx];
        }
        
        // Send the updated frame
        send_complete_frame(working_frame, pixels_to_process);
        
        pixels_updated += pixels_this_step;
        step++;
        
        // Wait before next update (except for the last step)
        if (pixels_updated < changed_count) {
            vTaskDelay(pdMS_TO_TICKS(step_duration_ms));
        }
    }
    
    // Store the final frame
    memcpy(previous_frame, data, pixels_to_process);
    if (pixels_to_process < 392) {
        memset(&previous_frame[pixels_to_process], 0, 392 - pixels_to_process);
    }
}
