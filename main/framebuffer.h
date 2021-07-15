#pragma once
#include <inttypes.h>

#define FRAMEBUFFER_WIDTH   28
#define FRAMEBUFFER_HEIGHT  14

uint8_t* framebuffer_init(void);
uint8_t* framebuffer_clear(void);
uint8_t* framebuffer_draw_string(char* str, uint8_t x, uint8_t y);
