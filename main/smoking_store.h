#pragma once

#include "esp_err.h"
#include "smoking_model.h"

typedef void (*smoking_store_status_cb_t)(esp_err_t result, void *user);

esp_err_t smoking_store_init(smoking_store_status_cb_t cb, void *user);
esp_err_t smoking_store_load(smoking_persist_state_t *state, bool *loaded);
esp_err_t smoking_store_save_async(const smoking_persist_state_t *state);

