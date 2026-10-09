#include "smoking_store.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "tools_storage.h"

#include <stddef.h>
#include <string.h>

static const char *TAG = "smoking_store";

#define STORE_NAMESPACE "smoking"
#define STORE_PARTITION TOOLS_STORAGE_PARTITION
#define STORE_KEY "state"
#define STORE_CLOCK_KEY "clock"
#define STORE_POWER_KEY "power"
#define STORE_MAGIC 0x534D4B31u
#define STORE_POWER_MAGIC 0x50575231u
#define STORE_POWER_VERSION 1u
#define STORE_QUEUE_DEPTH 1

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t size;
    smoking_persist_state_t state;
    uint32_t crc32;
} smoking_store_blob_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t next;
    uint32_t count;
    smoking_power_sample_t samples[SMOKING_POWER_LOG_CAPACITY];
    uint32_t crc32;
} smoking_power_log_blob_t;

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

static uint32_t power_log_crc(const smoking_power_log_blob_t *blob)
{
    return crc32_update(0, (const uint8_t *)blob,
                        offsetof(smoking_power_log_blob_t, crc32));
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

static esp_err_t open_store(nvs_open_mode_t mode, nvs_handle_t *handle)
{
    return nvs_open_from_partition(STORE_PARTITION, STORE_NAMESPACE, mode, handle);
}

static esp_err_t open_legacy_store(nvs_open_mode_t mode, nvs_handle_t *handle)
{
    return nvs_open(STORE_NAMESPACE, mode, handle);
}

static esp_err_t write_blob_to(nvs_handle_t handle, const smoking_store_blob_t *blob)
{
    esp_err_t err = nvs_set_blob(handle, STORE_KEY, blob, sizeof(*blob));
    if (err == ESP_OK) err = nvs_commit(handle);
    return err;
}

static esp_err_t write_blob(const smoking_store_blob_t *blob)
{
    nvs_handle_t handle = 0;
    esp_err_t err = open_store(NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = write_blob_to(handle, blob);
    nvs_close(handle);
    return err;
}

static esp_err_t read_blob_from(nvs_handle_t handle, smoking_persist_state_t *state,
                                bool *loaded)
{
    smoking_store_blob_t blob;
    size_t size = sizeof(blob);
    esp_err_t err = nvs_get_blob(handle, STORE_KEY, &blob, &size);
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

static esp_err_t try_load_state(bool legacy, smoking_persist_state_t *state,
                                bool *loaded)
{
    if (loaded) *loaded = false;
    nvs_handle_t handle = 0;
    esp_err_t err = legacy ? open_legacy_store(NVS_READONLY, &handle) :
        open_store(NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    err = read_blob_from(handle, state, loaded);
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

esp_err_t smoking_store_partition_init(void)
{
    return nvs_flash_init_partition(STORE_PARTITION);
}

esp_err_t smoking_store_load(smoking_persist_state_t *state, bool *loaded)
{
    if (loaded) *loaded = false;

    bool primary_loaded = false;
    esp_err_t err = try_load_state(false, state, &primary_loaded);
    if (err != ESP_OK || primary_loaded) {
        if (loaded) *loaded = primary_loaded;
        return err;
    }

    smoking_persist_state_t legacy_state;
    bool legacy_loaded = false;
    err = try_load_state(true, &legacy_state, &legacy_loaded);
    if (err != ESP_OK || !legacy_loaded) return err;

    ESP_LOGI(TAG, "已从旧 NVS 迁移抽烟状态到 tools_data 分区");
    smoking_store_blob_t blob = make_blob(&legacy_state);
    esp_err_t save_err = write_blob(&blob);
    if (save_err != ESP_OK) {
        ESP_LOGW(TAG, "迁移抽烟状态失败: %s", esp_err_to_name(save_err));
        return save_err;
    }
    if (state) *state = legacy_state;
    if (loaded) *loaded = true;
    return ESP_OK;
}

esp_err_t smoking_store_load_clock(uint32_t *clock_sec, bool *loaded)
{
    if (loaded) *loaded = false;
    nvs_handle_t handle = 0;
    esp_err_t err = open_store(NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;

    uint32_t value = 0;
    err = nvs_get_u32(handle, STORE_CLOCK_KEY, &value);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = open_legacy_store(NVS_READONLY, &handle);
        if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
        if (err != ESP_OK) return err;
        err = nvs_get_u32(handle, STORE_CLOCK_KEY, &value);
        nvs_close(handle);
        if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    }
    if (err != ESP_OK) return err;

    if (clock_sec) *clock_sec = value;
    if (loaded) *loaded = true;
    return ESP_OK;
}

esp_err_t smoking_store_save_clock(uint32_t clock_sec)
{
    nvs_handle_t handle = 0;
    esp_err_t err = open_store(NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_u32(handle, STORE_CLOCK_KEY, clock_sec);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static esp_err_t load_power_log(nvs_handle_t handle, smoking_power_log_blob_t *log,
                                bool *loaded)
{
    if (loaded) *loaded = false;
    memset(log, 0, sizeof(*log));

    size_t size = sizeof(*log);
    esp_err_t err = nvs_get_blob(handle, STORE_POWER_KEY, log, &size);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    if (size != sizeof(*log) ||
        log->magic != STORE_POWER_MAGIC ||
        log->version != STORE_POWER_VERSION ||
        log->next >= SMOKING_POWER_LOG_CAPACITY ||
        log->count > SMOKING_POWER_LOG_CAPACITY ||
        log->crc32 != power_log_crc(log)) {
        memset(log, 0, sizeof(*log));
        ESP_LOGW(TAG, "电量记录版本、大小或 CRC 不匹配，重新开始记录");
        return ESP_ERR_INVALID_CRC;
    }

    if (loaded) *loaded = true;
    return ESP_OK;
}

esp_err_t smoking_store_append_power_sample(const smoking_power_sample_t *sample)
{
    if (!sample) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle = 0;
    esp_err_t err = open_store(NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    smoking_power_log_blob_t log;
    bool loaded = false;
    err = load_power_log(handle, &log, &loaded);
    if (err != ESP_OK && err != ESP_ERR_INVALID_CRC) {
        nvs_close(handle);
        return err;
    }
    if (!loaded) {
        memset(&log, 0, sizeof(log));
        log.magic = STORE_POWER_MAGIC;
        log.version = STORE_POWER_VERSION;
    }

    log.samples[log.next] = *sample;
    log.next = (log.next + 1u) % SMOKING_POWER_LOG_CAPACITY;
    if (log.count < SMOKING_POWER_LOG_CAPACITY) log.count++;
    log.crc32 = power_log_crc(&log);

    err = nvs_set_blob(handle, STORE_POWER_KEY, &log, sizeof(log));
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
