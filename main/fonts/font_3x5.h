#pragma once
#include "font.h"

// Inspired by https://geoffg.net/Downloads/GLCD_Driver/glcd_library_1_0.h
const unsigned char variant_2_font_internal[0x41][3] ={
    {0x00,0x00,0x00}, // Space
    {0x00,0x5C,0x00}, // !
    {0x0C,0x00,0x0C}, // 
    {0x7C,0x28,0x7C}, // 
    {0x7C,0x44,0x7C}, // 0
    {0x24,0x10,0x48}, // 
    {0x28,0x54,0x08}, // 
    {0x00,0x0C,0x00}, // '
    {0x38,0x44,0x00}, // (
    {0x44,0x38,0x00}, // )
    {0x20,0x10,0x08}, // /
    {0x10,0x38,0x10}, // +
    {0x80,0x40,0x00}, // ,
    {0x10,0x10,0x10}, // -
    {0x00,0x40,0x00}, // .
    {0x20,0x10,0x08}, // /
    {0x38,0x44,0x38}, // 0
    {0x00,0x7C,0x00}, // 1
    {0x64,0x54,0x48}, // 2
    {0x44,0x54,0x28}, // 3
    {0x1C,0x10,0x7C}, // 4
    {0x4C,0x54,0x24}, // 5
    {0x38,0x54,0x20}, // 6
    {0x04,0x74,0x0C}, // 7
    {0x28,0x54,0x28}, // 8
    {0x08,0x54,0x38}, // 9
    {0x00,0x50,0x00}, // :
    {0x80,0x50,0x00}, // ;
    {0x10,0x28,0x44}, // <
    {0x28,0x28,0x28}, // =
    {0x44,0x28,0x10}, // >
    {0x04,0x54,0x08}, // ?
    {0x38,0x4C,0x5C}, // @
    {0x78,0x14,0x78}, // A
    {0x7C,0x54,0x28}, // B
    {0x38,0x44,0x44}, // C
    {0x7C,0x44,0x38}, // D
    {0x7C,0x54,0x44}, // E
    {0x7C,0x14,0x04}, // F
    {0x38,0x44,0x34}, // G
    {0x7C,0x10,0x7C}, // H
    {0x00,0x7C,0x00}, // I
    {0x20,0x40,0x3C}, // J
    {0x7C,0x10,0x6C}, // K
    {0x7C,0x40,0x40}, // L
    {0x7C,0x08,0x7C}, // M
    {0x7C,0x04,0x7C}, // N
    {0x7C,0x44,0x7C}, // O
    {0x7C,0x14,0x08}, // P
    {0x38,0x44,0x78}, // Q
    {0x7C,0x14,0x68}, // R
    {0x48,0x54,0x24}, // S
    {0x04,0x7C,0x04}, // T
    {0x7C,0x40,0x7C}, // U
    {0x3C,0x40,0x3C}, // V
    {0x7C,0x20,0x7C}, // W
    {0x6C,0x10,0x6C}, // X
    {0x1C,0x60,0x1C}, // Y
    {0x64,0x54,0x4C}, // Z
    {0x7C,0x44,0x00}, // [
    {0x08,0x10,0x20}, //    
    {0x44,0x7C,0x00}, // ]
    {0x08,0x04,0x08}, // ^
    {0x80,0x80,0x80}, // _
    {0x04,0x08,0x00}  // `
};

struct font_t font_3x5 = {
	.font = (unsigned char*)variant_2_font_internal,
	.font_height = 5,
	.font_width = 3,
	.start_offset = 2
};

