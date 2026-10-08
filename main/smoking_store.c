#include "smoking_store.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"

#include <stddef.h>
#include <string.h>

static const char *TAG = "smoking_store";

#define STORE_NAMESPACE "smoking"
#define STORE_KEY "state"
#define STORE_CLOCK_KEY "clock"
#define STORE_MAGIC 0x534D4B31u
#define STORE_QUEUE_DEPTH 1

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    smoking_persist_state_t state;
    uint32_t crc32;
} smoking_store_blob_t;

static QueueHandle_t s_save_queue;
static smoking_store_status_cb_t s_status_cb;
static void *s_status_user;

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return ~crc;
}

static uint32_t blob_crc(const smoking_store_blob_t *blob)
{
    return crc32_update(0, (const uint8_t *)blob,
                        offsetof(smoking_store_blob_t, crc32));
}

static smoking_store_blob_t make_blob(const smoking_persist_state_t *state)
{
    smoking_store_blob_t blob = {
        .magic = STORE_MAGIC,
        .version = SMOKING_MODEL_VERSION,
        .size = sizeof(smoking_persist_state_t),
        .state = *state,
    };
    blob.state = smoking_model_persist_snapshot(&(smoking_model_t){
        .persisted = *state,
        .pending_cigarettes = 1,
    });
    blob.crc32 = blob_crc(&blob);
    return blob;
}

static esp_err_t write_blob(const smoking_store_blob_t *blob)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(STORE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, STORE_KEY, blob, sizeof(*blob));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static void save_task(void *arg)
{
    (void)arg;
    smoking_store_blob_t blob;
    for (;;) {
        if (xQueueReceive(s_save_queue, &blob, portMAX_DELAY) != pdTRUE) continue;
        smoking_store_blob_t latest;
        while (xQueueReceive(s_save_queue, &latest, 0) == pdTRUE) {
            blob = latest;
        }
        esp_err_t err = write_blob(&blob);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "保存 NVS 失败: %s", esp_err_to_name(err));
        }
        if (s_status_cb) s_status_cb(err, s_status_user);
    }
}

esp_err_t smoking_store_init(smoking_store_status_cb_t cb, void *user)
{
    s_status_cb = cb;
    s_status_user = user;
    if (s_save_queue) return ESP_OK;

    s_save_queue = xQueueCreate(STORE_QUEUE_DEPTH, sizeof(smoking_store_blob_t));
    if (!s_save_queue) return ESP_ERR_NO_MEM;
    if (xTaskCreate(save_task, "smoke_store", 4096, NULL, 3, NULL) != pdPASS) {
        vQueueDelete(s_save_queue);
        s_save_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t smoking_store_load(smoking_persist_state_t *state, bool *loaded)
{
    if (loaded) *loaded = false;
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(STORE_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;

    smoking_store_blob_t blob;
    size_t size = sizeof(blob);
    err = nvs_get_blob(handle, STORE_KEY, &blob, &size);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    if (size != sizeof(blob) || blob.magic != STORE_MAGIC ||
        blob.version != SMOKING_MODEL_VERSION ||
        blob.size != sizeof(smoking_persist_state_t) ||
        blob.crc32 != blob_crc(&blob)) {
        ESP_LOGW(TAG, "NVS 状态版本、大小或 CRC 不匹配，使用默认首页状态");
        return ESP_ERR_INVALID_CRC;
    }

    if (state) *state = blob.state;
    if (loaded) *loaded = true;
    return ESP_OK;
}

esp_err_t smoking_store_load_clock(uint32_t *clock_sec, bool *loaded)
{
    if (loaded) *loaded = false;
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(STORE_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;

    uint32_t value = 0;
    err = nvs_get_u32(handle, STORE_CLOCK_KEY, &value);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;

    if (clock_sec) *clock_sec = value;
    if (loaded) *loaded = true;
    return ESP_OK;
}

esp_err_t smoking_store_save_clock(uint32_t clock_sec)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(STORE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_u32(handle, STORE_CLOCK_KEY, clock_sec);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t smoking_store_save(const smoking_persist_state_t *state)
{
    if (!state) return ESP_ERR_INVALID_ARG;
    smoking_store_blob_t blob = make_blob(state);
    esp_err_t err = write_blob(&blob);
    if (s_status_cb) s_status_cb(err, s_status_user);
    return err;
}

esp_err_t smoking_store_save_async(const smoking_persist_state_t *state)
{
    if (!s_save_queue || !state) return ESP_ERR_INVALID_STATE;
    smoking_store_blob_t blob = make_blob(state);
    return xQueueOverwrite(s_save_queue, &blob) == pdTRUE ? ESP_OK : ESP_FAIL;
}
