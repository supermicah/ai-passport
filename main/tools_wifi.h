#pragma once

#include "esp_err.h"

#include <stdbool.h>

#define TOOLS_WIFI_DEVICE_NAME "BLUFI_FoloPassport"

typedef enum {
    TOOLS_WIFI_OFF = 0,
    TOOLS_WIFI_STARTING,
    TOOLS_WIFI_NO_CREDENTIALS,
    TOOLS_WIFI_BLE_READY,
    TOOLS_WIFI_PHONE_CONNECTED,
    TOOLS_WIFI_CONNECTING,
    TOOLS_WIFI_CONNECTED,
    TOOLS_WIFI_TIME_SYNCED,
    TOOLS_WIFI_FAILED,
} tools_wifi_state_t;

typedef struct {
    tools_wifi_state_t state;
    esp_err_t error;
    bool provisioning;
    bool time_synced;
    char ssid[33];
    char ip[16];
} tools_wifi_status_t;

esp_err_t tools_wifi_init(void);
esp_err_t tools_wifi_start_provisioning(void);
void tools_wifi_stop_provisioning(void);
esp_err_t tools_wifi_reconnect(void);
esp_err_t tools_wifi_clear_credentials(void);
tools_wifi_status_t tools_wifi_status(void);
bool tools_wifi_take_time_synced(void);
