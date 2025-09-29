#include "utils.h"
#include <string.h>
#include <ctype.h>

// Weather icons
#include "weather/cloud.xbm"
#include "weather/clouds.xbm"
#include "weather/cloud_moon.xbm"
#include "weather/cloud_sun.xbm"
#include "weather/cloud_wind.xbm"
#include "weather/cloud_wind_moon.xbm"
#include "weather/cloud_wind_sun.xbm"
#include "weather/lightning.xbm"
#include "weather/moon.xbm"
#include "weather/rain0.xbm"
#include "weather/rain1.xbm"
#include "weather/rain1_moon.xbm"
#include "weather/rain1_sun.xbm"
#include "weather/rain2.xbm"
#include "weather/rain_lightning.xbm"
#include "weather/rain_snow.xbm"
#include "weather/snow_moon.xbm"
#include "weather/snow_sun.xbm"
#include "weather/sun.xbm"
#include "weather/wind.xbm"

bool parse_bool_string(const char* value, bool* out_value)
{
    if (value == NULL || out_value == NULL) {
        return false;
    }

    size_t len = strlen(value);
    if (len == 0) {
        return false;
    }

    if (len == 1) {
        if (value[0] == '1') {
            *out_value = true;
            return true;
        }
        if (value[0] == '0') {
            *out_value = false;
            return true;
        }
    }

    char normalized[8];
    if (len >= sizeof(normalized)) {
        len = sizeof(normalized) - 1;
    }
    for (size_t i = 0; i < len; i++) {
        normalized[i] = (char)tolower((unsigned char)value[i]);
    }
    normalized[len] = '\0';

    if (strcmp(normalized, "true") == 0 || strcmp(normalized, "on") == 0 || strcmp(normalized, "yes") == 0) {
        *out_value = true;
        return true;
    }

    if (strcmp(normalized, "false") == 0 || strcmp(normalized, "off") == 0 || strcmp(normalized, "no") == 0) {
        *out_value = false;
        return true;
    }

    return false;
}

void get_time(struct tm* timeinfo) {
    time_t now;
    time(&now);
    localtime_r(&now, timeinfo);
}

const char* weather_condition_to_icon_bits(const char* condition)
{
    const char* fallback = cloud_bits;

    if (condition == NULL || condition[0] == '\0') {
        return fallback;
    }

    char normalized[48];
    size_t len = strlen(condition);
    if (len > sizeof(normalized) - 1) {
        len = sizeof(normalized) - 1;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)condition[i];
        c = (unsigned char)tolower(c);
        if (c == ' ' || c == '_') {
            c = '-';
        }
        normalized[i] = (char)c;
    }
    normalized[len] = '\0';

    struct tm timeinfo;
    get_time(&timeinfo);
    bool is_night = (timeinfo.tm_hour < 6 || timeinfo.tm_hour >= 20);

    if (strcmp(normalized, "clear-night") == 0) {
        return moon_bits;
    }

    if (strcmp(normalized, "sunny") == 0 || strcmp(normalized, "clear-day") == 0) {
        return sun_bits;
    }

    if (strcmp(normalized, "clear") == 0) {
        return is_night ? moon_bits : sun_bits;
    }

    if (strcmp(normalized, "partlycloudy") == 0 || strcmp(normalized, "partly-cloudy") == 0) {
        return is_night ? cloud_moon_bits : cloud_sun_bits;
    }

    if (strcmp(normalized, "cloudy") == 0 || strcmp(normalized, "overcast") == 0) {
        return clouds_bits;
    }

    if (strcmp(normalized, "windy-variant") == 0) {
        return is_night ? cloud_wind_moon_bits : cloud_wind_sun_bits;
    }

    if (strcmp(normalized, "windy") == 0) {
        return wind_bits;
    }

    if (strcmp(normalized, "fog") == 0 || strcmp(normalized, "mist") == 0) {
        return cloud_bits;
    }

    if (strcmp(normalized, "hail") == 0) {
        return rain_snow_bits;
    }

    if (strcmp(normalized, "lightning-rainy") == 0 || strcmp(normalized, "thunderstorm") == 0) {
        return rain_lightning_bits;
    }

    if (strcmp(normalized, "lightning") == 0) {
        return lightning_bits;
    }

    if (strcmp(normalized, "pouring") == 0) {
        return rain2_bits;
    }

    if (strcmp(normalized, "rainy") == 0) {
        return is_night ? rain1_moon_bits : rain1_sun_bits;
    }

    if (strcmp(normalized, "drizzle") == 0 || strcmp(normalized, "light-rain") == 0) {
        return rain0_bits;
    }

    if (strcmp(normalized, "snowy-rainy") == 0) {
        return rain_snow_bits;
    }

    if (strcmp(normalized, "snowy") == 0) {
        return is_night ? snow_moon_bits : snow_sun_bits;
    }

    if (strcmp(normalized, "exceptional") == 0) {
        return rain_lightning_bits;
    }

    if (strstr(normalized, "lightning") != NULL ||
        strstr(normalized, "thunder") != NULL ||
        strstr(normalized, "storm") != NULL) {
        return rain_lightning_bits;
    }

    if (strstr(normalized, "snow") != NULL) {
        if (strstr(normalized, "rain") != NULL) {
            return rain_snow_bits;
        }
        return is_night ? snow_moon_bits : snow_sun_bits;
    }

    if (strstr(normalized, "hail") != NULL) {
        return rain_snow_bits;
    }

    if (strstr(normalized, "rain") != NULL ||
        strstr(normalized, "drizzle") != NULL ||
        strstr(normalized, "shower") != NULL) {
        return is_night ? rain1_moon_bits : rain1_bits;
    }

    if (strstr(normalized, "wind") != NULL || strstr(normalized, "breeze") != NULL) {
        return wind_bits;
    }

    if (strstr(normalized, "fog") != NULL || strstr(normalized, "mist") != NULL) {
        return cloud_bits;
    }

    if (strstr(normalized, "partly") != NULL && strstr(normalized, "cloud") != NULL) {
        return is_night ? cloud_moon_bits : cloud_sun_bits;
    }

    if (strstr(normalized, "cloud") != NULL || strstr(normalized, "overcast") != NULL) {
        return clouds_bits;
    }

    if (is_night) {
        return moon_bits;
    }

    return sun_bits;
}
