#pragma once

#include <inttypes.h>

typedef struct font_t {
	unsigned char* font;
	uint8_t font_height;
	uint8_t font_width;
	uint8_t start_offset;
} font_t;

