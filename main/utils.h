#ifndef UTILS_H
#define UTILS_H

#include <stdbool.h>
#include <time.h>

/**
 * @brief Parse a boolean string value
 * 
 * Accepts: "1", "0", "true", "false", "on", "off", "yes", "no" (case insensitive)
 * 
 * @param value The string to parse
 * @param out_value Pointer to store the parsed boolean value
 * @return true if parsing was successful, false otherwise
 */
bool parse_bool_string(const char* value, bool* out_value);

/**
 * @brief Get current time
 * 
 * @param timeinfo Pointer to tm struct to store the time
 */
void get_time(struct tm* timeinfo);

/**
 * @brief Convert weather condition string to icon bitmap
 * 
 * @param condition Weather condition string (e.g., "sunny", "rainy", "cloudy")
 * @return Pointer to bitmap data for the weather icon
 */
const char* weather_condition_to_icon_bits(const char* condition);

#endif // UTILS_H
