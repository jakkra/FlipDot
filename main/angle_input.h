#pragma once

#include <inttypes.h>
typedef void(angle_event_callback(int8_t azimuth, int8_t elevation));

void angle_input_init(angle_event_callback* cb);