#include "framebuffer.h"
#include "fonts/font_pzim3x5.h"
#include "fonts/font_3x6.h"
#include "fonts/font_3x5.h"
#include "fonts/bmspa_font.h"
#include "fonts/homespun_font.h"
#include "esp_log.h"
#include <string.h>


static void drawChar(char c, uint8_t x, uint8_t y, font_t* font_container);

static uint8_t framebuffer[FRAMEBUFFER_HEIGHT][FRAMEBUFFER_WIDTH];


uint8_t* framebuffer_init(void)
{
    memset(framebuffer, 0, FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT);
    return framebuffer;
}

uint8_t* framebuffer_clear(void)
{
    memset(framebuffer, 0, FRAMEBUFFER_WIDTH * FRAMEBUFFER_HEIGHT);
    return framebuffer;
}


uint8_t* framebuffer_draw_string(char* str, uint8_t x, uint8_t y)
{
    uint8_t x_pos = x;
    uint8_t y_pos = y;
    char* str_pos = str;
    memset(framebuffer, 0, sizeof(framebuffer));

    // TODO detect character width for individual characters by checking like below
    // {0x7c,0x20,0x10,0x0c,0x00,0x00,0x00} => width 4
    // {0x00,0x00,0x74,0x54,0x54,0x00,0x00} => width 3

    while (*str_pos) {
        drawChar(*str_pos++, x_pos, y_pos, &font_3x6);
        x_pos += font_3x6.font_width;
        if (*str_pos && *str_pos != ':' && *(str_pos - 1) != ':') {
            x_pos++;
        }
    }
    x_pos = x;
    y_pos = y + font_3x6.font_height;
    str_pos = str;
    while (*str_pos) {
        drawChar(*str_pos++, x_pos, y_pos, &font_3x6);
        x_pos += font_3x6.font_width;
        if (*str_pos && *str_pos != ':' && *(str_pos - 1) != ':') {
            x_pos++;
        }
    }
    return (uint8_t*)framebuffer;
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