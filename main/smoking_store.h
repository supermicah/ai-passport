#pragma once

#include "esp_err.h"
#include "smoking_model.h"

#include <stdint.h>

#define SMOKING_POWER_LOG_CAPACITY 96u

typedef struct {
    uint32_t time_sec;
    int16_t voltage_mv;
    int8_t soc;
    uint8_t screen;
    uint8_t flags;
    uint8_t reserved[3];
} smoking_power_sample_t;

typedef void (*smoking_store_status_cb_t)(esp_err_t result, void *user);

esp_err_t smoking_store_partition_init(void);
esp_err_t smoking_store_init(smoking_store_status_cb_t cb, void *user);
esp_err_t smoking_store_load(smoking_persist_state_t *state, bool *loaded);
esp_err_t smoking_store_save(const smoking_persist_state_t *state);
esp_err_t smoking_store_save_async(const smoking_persist_state_t *state);
esp_err_t smoking_store_load_clock(uint32_t *clock_sec, bool *loaded);
esp_err_t smoking_store_save_clock(uint32_t clock_sec);
esp_err_t smoking_store_append_power_sample(const smoking_power_sample_t *sample);

