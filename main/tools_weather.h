#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

#define TOOLS_WEATHER_CITY "杭州"

typedef struct {
    bool valid;
    bool updating;
    esp_err_t error;
    uint32_t updated_at;
    char summary[40];
} tools_weather_status_t;

esp_err_t tools_weather_init(void);
tools_weather_status_t tools_weather_status(void);
void tools_weather_request_refresh(void);
