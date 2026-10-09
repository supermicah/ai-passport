#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    TOOLS_RADIO_STOPPED = 0,
    TOOLS_RADIO_CONNECTING,
    TOOLS_RADIO_BUFFERING,
    TOOLS_RADIO_PLAYING,
    TOOLS_RADIO_RECONNECTING,
    TOOLS_RADIO_ERROR,
} tools_radio_state_t;

typedef struct {
    tools_radio_state_t state;
    esp_err_t error;
    bool wanted_playing;
    bool network_ready;
    size_t station_index;
    size_t station_count;
    uint8_t volume;
    char station_name[64];
    char station_desc[96];
    char detail[40];
} tools_radio_status_t;

esp_err_t tools_radio_init(void);
void tools_radio_set_network_ready(bool ready);
void tools_radio_toggle(void);
void tools_radio_stop(void);
void tools_radio_next(void);
void tools_radio_prev(void);
void tools_radio_volume_up(void);
void tools_radio_volume_down(void);
tools_radio_status_t tools_radio_status(void);
