#include "framebuffer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <esp_err.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <float.h>
#include <sys/param.h>

typedef struct scroll_text_data_t {
    on_framebuffer_updated* on_update_callback;
    char* scrolling_text;
    TaskHandle_t scrolling_task_handle;
    uint32_t scroll_interval;
    int16_t x;
    int16_t y;
    font_t* font;
    int16_t pixel_offset;
    uint16_t text_width;
} scroll_text_data_t;

static int16_t drawChar(char c, int16_t x, int16_t y, font_t* font_container);
static void getWidthOfCharacter(char c, font_t* font_container, uint8_t* left_offset, uint8_t* right_offset, uint8_t* true_width);
static void scroll_task(void* arg);
static uint16_t get_string_width(const char* str, font_t* font_container);

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
        scroll_data.scrolling_text = NULL;
        scroll_data.pixel_offset = 0;
        scroll_data.text_width = 0;
    }
    memset(framebuffer, 0, FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT);
    return (uint8_t*)framebuffer;
}

uint8_t* framebuffer_draw_string(char* str, int16_t x, int16_t y, font_t* font, bool wrap_newline)
{
    int16_t x_pos = x;
    int16_t y_pos = y;
    char* str_pos = str;
    int16_t char_width;

    while (*str_pos) {
        if (wrap_newline && x_pos >= FRAMEBUFFER_WIDTH) {
            y_pos += font->font_height + 1;
            x_pos = x;
            continue;
        }
        if (!wrap_newline && x_pos >= FRAMEBUFFER_WIDTH) {
            break;
        }
        char_width = drawChar(*str_pos++, x_pos, y_pos, font);
        if (char_width >= 0) {
            x_pos += char_width;
            x_pos++; // Distance between characters => 1
        }
    }
    
    return (uint8_t*)framebuffer;
}

uint8_t* framebuffer_draw_bitmap(uint8_t width, uint8_t height, const uint8_t bitmap[height][width], uint8_t x, uint8_t y, bool invert)
{
    for (uint8_t i = 0; i < height; i++) {
        for (uint8_t j = 0; j < width; j++) {
            if (x + j < FRAMEBUFFER_WIDTH && y + i < FRAMEBUFFER_HEIGHT) {
                framebuffer[y + i][x + j] = invert ? !bitmap[i][j] : bitmap[i][j];
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
    scroll_data.x = x;
    scroll_data.y = y;
    scroll_data.scroll_interval = scroll_interval_ms;
    scroll_data.on_update_callback = on_update;
    scroll_data.scrolling_text = malloc(strlen(str) + 1);
    strcpy(scroll_data.scrolling_text, str);
    scroll_data.font = font;
    scroll_data.pixel_offset = FRAMEBUFFER_WIDTH;
    scroll_data.text_width = get_string_width(scroll_data.scrolling_text, font);
    assert(xTaskCreate(scroll_task, "scroll_task", 2048, NULL, 10, &scroll_data.scrolling_task_handle) == pdPASS);

    return ESP_OK;
}

uint8_t* framebuffer_set_pixel_value(uint8_t x, uint8_t y, uint8_t val) {
    framebuffer[y][x] = val;
    return (uint8_t*)framebuffer;
}

static void compute_endpoint_on_framebuffer(int cx, int cy, double angle_deg, int* out_x, int* out_y)
{
    double rad = (angle_deg - 90.0) * (M_PI / 180.0);
    double dx = cos(rad);
    double dy = sin(rad);

    if (fabs(dx) < 1e-9 && fabs(dy) < 1e-9) {
        *out_x = cx;
        *out_y = cy;
        return;
    }

    double t_x = DBL_MAX;
    double t_y = DBL_MAX;

    if (fabs(dx) > 1e-9) {
        if (dx > 0.0) {
            t_x = (FRAMEBUFFER_WIDTH - 1 - cx) / dx;
        } else {
            t_x = (0 - cx) / dx;
        }
    }
    if (fabs(dy) > 1e-9) {
        if (dy > 0.0) {
            t_y = (FRAMEBUFFER_HEIGHT - 1 - cy) / dy;
        } else {
            t_y = (0 - cy) / dy;
        }
    }

    double t = fmin(t_x, t_y);
    if (!isfinite(t) || t < 0.0) {
        t = 0.0;
    }

    int x = (int)round((double)cx + dx * t);
    int y = (int)round((double)cy + dy * t);

    if (x < 0) {
        x = 0;
    } else if (x >= FRAMEBUFFER_WIDTH) {
        x = FRAMEBUFFER_WIDTH - 1;
    }
    if (y < 0) {
        y = 0;
    } else if (y >= FRAMEBUFFER_HEIGHT) {
        y = FRAMEBUFFER_HEIGHT - 1;
    }

    *out_x = x;
    *out_y = y;
}

uint8_t* framebuffer_draw_line(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1, uint8_t val)
{
    int x = x0;
    int y = y0;
    const int target_x = x1;
    const int target_y = y1;
    int dx = abs(target_x - x);
    int sx = x < target_x ? 1 : -1;
    int dy = -abs(target_y - y);
    int sy = y < target_y ? 1 : -1;
    int err = dx + dy;

    while (1) {
        if ((unsigned)x < FRAMEBUFFER_WIDTH && (unsigned)y < FRAMEBUFFER_HEIGHT) {
            framebuffer[y][x] = val;
        }
        if (x == target_x && y == target_y) {
            break;
        }
        int e2 = err << 1;
        if (e2 >= dy) {
            err += dy;
            x += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y += sy;
        }
    }

    return (uint8_t*)framebuffer;
}

uint8_t* framebuffer_draw_analog_clock(uint8_t hour, uint8_t minute, uint8_t second, bool draw_bezel)
{
    const int cx = 13;
    const int cy = 7;
    if (draw_bezel) {
        for (int hour_mark = 0; hour_mark < 12; hour_mark++) {
            int marker_x = cx;
            int marker_y = cy;
            compute_endpoint_on_framebuffer(cx, cy, (double)hour_mark * 30.0, &marker_x, &marker_y);
            framebuffer_set_pixel_value((uint8_t)marker_x, (uint8_t)marker_y, 1);
        }
    }

    uint8_t hour12 = hour % 12;
    double hour_angle_deg = 30.0 * (double)hour12 + 0.5 * (double)minute + (double)second / 120.0;
    double minute_angle_deg = 6.0 * (double)minute + 0.1 * (double)second;

    int hour_x = cx;
    int hour_y = cy;
    int minute_x = cx;
    int minute_y = cy;

    compute_endpoint_on_framebuffer(cx, cy, hour_angle_deg, &hour_x, &hour_y);
    compute_endpoint_on_framebuffer(cx, cy, minute_angle_deg, &minute_x, &minute_y);

    double hour_dx = (double)hour_x - (double)cx;
    double hour_dy = (double)hour_y - (double)cy;
    const double hour_scale = 0.65;
    hour_x = cx + (int)round(hour_dx * hour_scale);
    hour_y = cy + (int)round(hour_dy * hour_scale);

    double minute_dx = (double)minute_x - (double)cx;
    double minute_dy = (double)minute_y - (double)cy;
    double minute_len = sqrt(minute_dx * minute_dx + minute_dy * minute_dy);
    if (minute_len > 1.0) {
        double minute_scale = 0.9 * (minute_len - 1.0) / minute_len;
        minute_x = cx + (int)round(minute_dx * minute_scale);
        minute_y = cy + (int)round(minute_dy * minute_scale);
    }

    framebuffer_draw_line((uint8_t)cx, (uint8_t)cy, (uint8_t)hour_x, (uint8_t)hour_y, 1);
    framebuffer_draw_line((uint8_t)cx, (uint8_t)cy, (uint8_t)minute_x, (uint8_t)minute_y, 1);
    framebuffer_set_pixel_value((uint8_t)cx, (uint8_t)cy, 1);

    return (uint8_t*)framebuffer;
}

static void scroll_task(void* arg)
{
    (void)arg;
    int16_t y_pos = scroll_data.y;

    while (1) {
        memset(framebuffer, 0, FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT);

        int16_t x_pos = scroll_data.pixel_offset + scroll_data.x;
        const char* text = scroll_data.scrolling_text;
        while (*text) {
            int16_t width = drawChar(*text++, x_pos, y_pos, scroll_data.font);
            if (width >= 0) {
                x_pos += width;
                x_pos++; // character spacing
            } else {
                break;
            }
        }

        scroll_data.on_update_callback((uint8_t*)framebuffer);

        if (scroll_data.text_width == 0) {
            vTaskDelay(pdMS_TO_TICKS(scroll_data.scroll_interval));
            continue;
        }

        scroll_data.pixel_offset--;
        if (scroll_data.pixel_offset < -(int16_t)scroll_data.text_width) {
            scroll_data.pixel_offset = FRAMEBUFFER_WIDTH;
        }

        vTaskDelay(pdMS_TO_TICKS(scroll_data.scroll_interval));
    }
}

static int16_t drawChar(char c, int16_t x, int16_t y, font_t* font_container) {
    uint8_t i, j;
    uint8_t width = font_container->font_width;
    uint8_t height = font_container->font_height;
    uint8_t offset = font_container->start_offset;
    uint8_t left_offset, right_offset, true_width;

    getWidthOfCharacter(c, font_container, &left_offset, &right_offset, &true_width);
    int16_t advance = true_width;

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
            int16_t target_x = x + j - left_offset;
            int16_t target_y = y + i - offset;

            if (target_x < 0 || target_x >= FRAMEBUFFER_WIDTH) {
                continue;
            }
            if (target_y < 0 || target_y >= FRAMEBUFFER_HEIGHT) {
                continue;
            }
            if (chr[j] & (1 << i)) {
                framebuffer[target_y][target_x] = 1;
            }
        }
    }

    return advance;
}

static uint16_t get_string_width(const char* str, font_t* font_container)
{
    uint16_t width_total = 0;
    const char* pos = str;
    uint8_t left_offset, right_offset, true_width;

    while (*pos) {
        getWidthOfCharacter(*pos++, font_container, &left_offset, &right_offset, &true_width);
        width_total += true_width;
        width_total++; // spacing between characters
    }

    if (width_total > 0) {
        width_total--; // remove trailing space
    }

    return width_total;
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
