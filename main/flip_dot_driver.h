#pragma once

#include <stdbool.h>

void flip_dot_driver_init(void);
void flip_dot_driver_all_on(void);
void flip_dot_driver_all_off(void);
void flip_dot_driver_draw(uint8_t* data, uint32_t len);
void flip_dot_driver_set_invert(bool invert);
bool flip_dot_driver_get_invert(void);
void flip_dot_driver_print_character(uint8_t character, uint8_t offset, uint8_t framebuffer[14][28]);
