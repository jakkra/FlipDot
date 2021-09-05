#include "framebuffer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <esp_err.h>
#include <string.h>
#include <sys/param.h>

typedef struct scroll_text_data_t {
    on_framebuffer_updated* on_update_callback;
    char* scrolling_text;
    TaskHandle_t scrolling_task_handle;
    uint32_t scroll_interval;
    uint8_t x;
    uint8_t y;
    char* current_str_index;
    font_t* font;
    int index;
} scroll_text_data_t;

static uint8_t drawChar(char c, uint8_t x, uint8_t y, font_t* font_container);
static void getWidthOfCharacter(char c, font_t* font_container, uint8_t* left_offset, uint8_t* right_offset, uint8_t* true_width);
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

uint8_t* framebuffer_draw_string(char* str, uint8_t x, uint8_t y, font_t* font, bool wrap_newline)
{
    uint8_t x_pos = x;
    uint8_t y_pos = y;
    char* str_pos = str;
    int8_t char_width;

    while (*str_pos) {
        char_width = drawChar(*str_pos++, x_pos, y_pos, font);
        if (char_width >= 0) {
            x_pos +=  char_width;
            x_pos++; // Distance between characters => 1
        } else {
            if (wrap_newline) {
                y_pos += font->font_height + 1;
                x_pos = x;
                str_pos--; // re-draw current char on new location
            } else {
                break; // Does not fit
            }
        }
    }
    
    return (uint8_t*)framebuffer;
}

esp_err_t framebuffer_scrolling_text(char* str, uint8_t x, uint8_t y, uint32_t scroll_interval_ms, font_t* font, on_framebuffer_updated* on_update)
{
    if (scroll_data.on_update_callback != NULL) {
        ESP_LOGE("FRAMEBUFFER", "Scrolling text already running, clear before use.");
        return ESP_FAIL;
    }
    scroll_data.x = 0;
    scroll_data.y = y;
    scroll_data.scroll_interval = scroll_interval_ms;
    scroll_data.on_update_callback = on_update;
    scroll_data.scrolling_text = malloc(strlen(str) + 1);
    strcpy(scroll_data.scrolling_text, str);
    scroll_data.current_str_index = scroll_data.scrolling_text;
    scroll_data.font = font;
    assert(xTaskCreate(scroll_task, "scroll_task", 2048, NULL, 10, &scroll_data.scrolling_task_handle) == pdPASS);

    return ESP_OK;
}


uint8_t* framebuffer_set_pixel_value(uint8_t x, uint8_t y, uint8_t val) {
    framebuffer[y][x] = val;
    return (uint8_t*)framebuffer;
}

static void scroll_task(void* arg)
{
    uint8_t x_pos = scroll_data.x;
    uint8_t y_pos = scroll_data.y ;
    int8_t char_width = 0;

    while (1) {
        scroll_data.index++;
        if (scroll_data.scrolling_text[scroll_data.index] == '\0') {
            scroll_data.index = 0;
        }
        memset(framebuffer, 0, FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT);
        char_width = 0;
        x_pos = scroll_data.x;
        int i = 0;
        while (char_width >= 0 && i < strlen(scroll_data.scrolling_text)) {
            char_width = drawChar(scroll_data.scrolling_text[(scroll_data.index + i) % strlen(scroll_data.scrolling_text)], x_pos, y_pos, scroll_data.font);
            if (char_width >= 0) {
                x_pos +=  char_width;
                x_pos++; // Distance between characters => 1
            } else {
                break; // Does not fit
            }
            i++;
        }
        scroll_data.on_update_callback((uint8_t*)framebuffer);
        vTaskDelay(pdMS_TO_TICKS(scroll_data.scroll_interval));
    }
}

static uint8_t drawChar(char c, uint8_t x, uint8_t y, font_t* font_container) {
    uint8_t i, j;
    uint8_t width = font_container->font_width;
    uint8_t height = font_container->font_height;
    uint8_t offset = font_container->start_offset;
    uint8_t left_offset, right_offset, true_width;

    getWidthOfCharacter(c, font_container, &left_offset, &right_offset, &true_width);
    //printf(" %d - %c - %d => %d\n", left_offset, c, right_offset, true_width);

    if ((x + true_width) > FRAMEBUFFER_WIDTH) {
        // Do not draw outside of the framebuffer. Just ignore it
        return -1;
    }
    
    // Convert the character to an index
    c = c & 0x7F;
    if (c < ' ') {
        c = 0;
    } else {
        c -= ' ';
    }

    uint8_t* chr = &font_container->font[c*width];

    for (j = left_offset; j < (width - right_offset); j++) {
        for (i = offset; i < height + offset; i++) {
            if (chr[j] & (1 << i)) {
                framebuffer[y + i - offset][x + j - left_offset] = 1;
            }
        }
    }

    return true_width;
}

static void getWidthOfCharacter(char c, font_t* font_container, uint8_t* left_offset, uint8_t* right_offset, uint8_t* true_width) {
    uint8_t width = font_container->font_width;

    // Convert the character to an index
    c = c & 0x7F;
    if (c < ' ') {
        c = 0;
    } else {
        c -= ' ';
    }

    uint8_t* chr = &font_container->font[c*width];
    uint8_t last_row_empty = 0xFF;

    for (int i = 0; i < width; i++) {
        if (chr[i]) {
            last_row_empty = MIN(last_row_empty, i);
        }
    }
    *left_offset = last_row_empty;

    last_row_empty = 0xFF;
    for (int i = width - 1; i >= 0; i--) {
        if (chr[i]) {
            last_row_empty = MIN(last_row_empty, width - i - 1);
        }
    }

    *right_offset = last_row_empty;
    if (*left_offset == 0xFF && *right_offset == 0xFF) {
        *true_width = 1; // Empty character
    } else {
        *true_width = width - *left_offset - *right_offset;
    }
}