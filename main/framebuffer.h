#pragma once
#include <inttypes.h>
#include <esp_err.h>
#include "fonts/font.h"

#define FRAMEBUFFER_WIDTH   28
#define FRAMEBUFFER_HEIGHT  14

typedef void on_framebuffer_updated(uint8_t* framebuffer);


uint8_t* framebuffer_init(void);
uint8_t* framebuffer_clear(void);
uint8_t* framebuffer_draw_string(char* str, uint8_t x, uint8_t y, font_t* font);
esp_err_t framebuffer_scrolling_text(char* str, uint8_t x, uint8_t y, uint32_t scroll_interval_ms, font_t* font, on_framebuffer_updated* on_update);
