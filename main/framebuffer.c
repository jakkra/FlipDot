#include "framebuffer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <esp_err.h>
#include <string.h>


typedef struct scroll_text_data_t {
    on_framebuffer_updated* on_update_callback;
    char* scrolling_text;
    TaskHandle_t scrolling_task_handle;
    uint32_t scroll_interval;
    uint8_t x;
    uint8_t y;
    char* current_str_index;
    font_t* font;
} scroll_text_data_t;

static void drawChar(char c, uint8_t x, uint8_t y, font_t* font_container);
static void scroll_task(void* arg);

static uint8_t framebuffer[FRAMEBUFFER_HEIGHT][FRAMEBUFFER_WIDTH];

static scroll_text_data_t scroll_data;


uint8_t* framebuffer_init(void)
{
    memset(&scroll_data, 0, sizeof(scroll_text_data_t));
    memset(framebuffer, 0, FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT);
    return (uint8_t*)framebuffer;
}

uint8_t* framebuffer_clear(void)
{
    if (scroll_data.scrolling_task_handle != NULL) {
        vTaskDelete(scroll_data.scrolling_task_handle);
        scroll_data.scrolling_task_handle = NULL;
        free(scroll_data.scrolling_text);
        scroll_data.on_update_callback = NULL;
    }
    memset(framebuffer, 0, FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT);
    return (uint8_t*)framebuffer;
}


uint8_t* framebuffer_draw_string(char* str, uint8_t x, uint8_t y, font_t* font)
{
    uint8_t x_pos = x;
    uint8_t y_pos = y;
    char* str_pos = str;

    // TODO detect character width for individual characters by checking like below
    // {0x7c,0x20,0x10,0x0c,0x00,0x00,0x00} => width 4
    // {0x00,0x00,0x74,0x54,0x54,0x00,0x00} => width 3

    while (*str_pos) {
        drawChar(*str_pos++, x_pos, y_pos, font);
        x_pos += font->font_width;
        if (*str_pos && *str_pos != ':' && *(str_pos - 1) != ':') {
            x_pos++;
        }

        // Until dynamic character width just make the space character less wide manually
        if (*str_pos  && *(str_pos - 1) == ' ') {
            x_pos -= font->font_width;
            x_pos++;
        }
        if ((x_pos + font->font_width) > FRAMEBUFFER_WIDTH) {
            // Do not draw outside of the framebuffer. Just ignore rest of str.
            break;
        }
    }
    
    /*x_pos = x;
    y_pos = y + font.font_height;
    str_pos = str;
    while (*str_pos) {
        drawChar(*str_pos++, x_pos, y_pos, &font);
        x_pos += font.font_width;
        if (*str_pos && *str_pos != ':' && *(str_pos - 1) != ':') {
            x_pos++;
        }
    }*/
    return (uint8_t*)framebuffer;
}

esp_err_t framebuffer_scrolling_text(char* str, uint8_t x, uint8_t y, uint32_t scroll_interval_ms, font_t* font, on_framebuffer_updated* on_update)
{
    if (scroll_data.on_update_callback != NULL) {
        ESP_LOGE("FRAMEBUFFER", "Scrolling text already running, clear before use.");
        return ESP_FAIL;
    }
    scroll_data.x = FRAMEBUFFER_WIDTH - 1;
    scroll_data.y = y;
    scroll_data.scroll_interval = scroll_interval_ms;
    scroll_data.on_update_callback = on_update;
    scroll_data.scrolling_text = malloc(strlen(str));
    strcpy(scroll_data.scrolling_text, str);
    scroll_data.current_str_index = scroll_data.scrolling_text;
    scroll_data.font = font;
    assert(xTaskCreate(scroll_task, "scroll_task", 2048, NULL, 10, &scroll_data.scrolling_task_handle) == pdPASS);

    return ESP_OK;
}

static void scroll_task(void* arg)
{
    while (1) {
        scroll_data.current_str_index++;
        if (*scroll_data.current_str_index == '\0') {
            scroll_data.current_str_index = scroll_data.scrolling_text;
        }
        scroll_data.x--;
        memset(framebuffer, 0, FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT);
        framebuffer_draw_string(scroll_data.current_str_index, scroll_data.x, scroll_data.y, scroll_data.font);
        scroll_data.on_update_callback((uint8_t*)framebuffer);
        vTaskDelay(pdMS_TO_TICKS(scroll_data.scroll_interval));
    }
}


static void drawChar(char c, uint8_t x, uint8_t y, font_t* font_container) {
    uint8_t i, j;
    uint8_t width = font_container->font_width;
    uint8_t height = font_container->font_height;
    uint8_t offset = font_container->start_offset;

    // Convert the character to an index
    c = c & 0x7F;
    if (c < ' ') {
        c = 0;
    } else {
        c -= ' ';
    }

    uint8_t* chr = &font_container->font[c*width];

    for (j = 0; j < width; j++) {
        for (i = offset; i < height + offset; i++) {
            if (chr[j] & (1 << i)) {
                framebuffer[y + i - offset][x + j] = 1;
            }
        }
    }
}