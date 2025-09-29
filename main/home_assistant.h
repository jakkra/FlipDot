#ifndef HOME_ASSISTANT_H
#define HOME_ASSISTANT_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/**
 * @brief Initialize the Home Assistant sensor cache system
 * 
 * Must be called before using any other functions in this module.
 */
void home_assistant_cache_init(void);

/**
 * @brief Start the Home Assistant polling task
 * 
 * Creates a background task that periodically polls configured sensors
 * from Home Assistant and updates the cache.
 */
void home_assistant_start_polling(void);

/**
 * @brief Get the cached temperature value
 * 
 * @param value Pointer to store the temperature value (in configured unit)
 * @return true if a valid cached value is available, false otherwise
 */
bool home_assistant_get_temperature(int32_t* value);

/**
 * @brief Get the cached solar production value
 * 
 * @param value Pointer to store the solar production in watts
 * @return true if a valid cached value is available, false otherwise
 */
bool home_assistant_get_solar(uint32_t* value);

/**
 * @brief Get the cached weather data
 * 
 * @param temperature Pointer to store the temperature (in configured unit)
 * @param humidity Pointer to store the humidity percentage
 * @param pressure Pointer to store the pressure (in configured unit)
 * @param condition Buffer to store the weather condition string
 * @param condition_size Size of the condition buffer
 * @return true if valid cached weather data is available, false otherwise
 */
bool home_assistant_get_weather(float* temperature, float* humidity, float* pressure, 
                                  char* condition, size_t condition_size);

#endif // HOME_ASSISTANT_H
