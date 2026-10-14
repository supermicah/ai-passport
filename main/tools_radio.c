#include "tools_radio.h"

#include "bsp_audio.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "tools_storage.h"
#include "tools_wifi.h"

#include "decoder/esp_audio_dec_default.h"
#include "simple_dec/esp_audio_simple_dec.h"
#include "simple_dec/esp_audio_simple_dec_default.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    char name[64];
    char desc[96];
    char url[192];
} radio_station_t;

static const char *TAG = "tools_radio";

#define RADIO_INPUT_BYTES 1024u
#define RADIO_OUTPUT_BYTES 8192u
#define RADIO_STOP_TIMEOUT_MS 1200u
#define RADIO_MAX_FAILED_ATTEMPTS 3u
#define RADIO_MIN_VOLUME 10u
#define RADIO_MAX_VOLUME 100u
#define RADIO_VOLUME_STEP 10u

static const radio_station_t RADIO_STATIONS[] = {
    {"杭州交通经济", "本地路况 · 网络直播",
     "http://lhttp.qtfm.cn/live/1133/64k.mp3"},
    {"杭州华语之声", "城市陪伴 · 网络直播",
     "http://lhttp.qtfm.cn/live/20505/64k.mp3"},
    {"杭州城市资讯", "潮流音乐 · 网络直播",
     "http://lhttp-hw.qtfm.cn/live/15318146/64k.mp3"},
    {"西湖之声", "杭州本地 · 网络直播",
     "http://lhttp.qtfm.cn/live/1163/64k.mp3"},
    {"浙江之声", "浙江新闻 · 网络直播",
     "http://lhttp.qtfm.cn/live/4518/64k.mp3"},
    {"中国之声", "新闻综合 · 网络直播",
     "http://lhttp.qingting.fm/live/15318317/64k.mp3"},
    {"第一财经", "财经资讯 · 网络直播",
     "http://lhttp.qingting.fm/live/276/64k.mp3"},
};

static TaskHandle_t s_radio_task;
static SemaphoreHandle_t s_state_lock;
static bool s_initialized;
static bool s_network_ready;
static bool s_wanted_playing;
static bool s_stream_active;
static size_t s_station_index;
static uint8_t s_volume = 55;
static uint32_t s_generation = 1;
static tools_radio_state_t s_state = TOOLS_RADIO_STOPPED;
static esp_err_t s_error = ESP_OK;
static char s_detail[40] = "已停止";

static size_t station_count(void)
{
    return sizeof(RADIO_STATIONS) / sizeof(RADIO_STATIONS[0]);
}

static uint8_t clamp_volume(uint8_t volume)
{
    if (volume < RADIO_MIN_VOLUME) return RADIO_MIN_VOLUME;
    if (volume > RADIO_MAX_VOLUME) return RADIO_MAX_VOLUME;
    return volume;
}

static void state_lock(void)
{
    if (s_state_lock) xSemaphoreTake(s_state_lock, portMAX_DELAY);
}

static void state_unlock(void)
{
    if (s_state_lock) xSemaphoreGive(s_state_lock);
}

static void set_state(tools_radio_state_t state, const char *detail, esp_err_t err)
{
    state_lock();
    s_state = state;
    s_error = err;
    snprintf(s_detail, sizeof(s_detail), "%s", detail ? detail : "");
    state_unlock();
}

static void bump_generation(void)
{
    state_lock();
    s_generation++;
    if (s_generation == 0) s_generation = 1;
    state_unlock();
}

static void save_u8(const char *key, uint8_t value)
{
    nvs_handle_t handle;
    if (nvs_open_from_partition(TOOLS_STORAGE_PARTITION, "radio",
                                NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_u8(handle, key, value);
    nvs_commit(handle);
    nvs_close(handle);
}

static uint8_t load_u8(const char *key, uint8_t fallback)
{
    nvs_handle_t handle;
    if (nvs_open_from_partition(TOOLS_STORAGE_PARTITION, "radio",
                                NVS_READONLY, &handle) != ESP_OK) {
        if (nvs_open("radio", NVS_READONLY, &handle) != ESP_OK) return fallback;
    }
    uint8_t value = fallback;
    if (nvs_get_u8(handle, key, &value) != ESP_OK) value = fallback;
    nvs_close(handle);
    return value;
}

static bool starts_with(const char *text, const char *prefix)
{
    return text && prefix && strncmp(text, prefix, strlen(prefix)) == 0;
}

static bool request_current(uint32_t generation, size_t station)
{
    state_lock();
    bool current = s_network_ready && s_wanted_playing &&
                   s_generation == generation && s_station_index == station;
    state_unlock();
    return current;
}

static size_t downmix_to_mono(uint8_t *pcm, size_t bytes, uint8_t channels)
{
    if (!pcm || channels == 0) return 0;
    if (channels == 1) return bytes & ~(size_t)1;

    int16_t *samples = (int16_t *)pcm;
    size_t frames = bytes / (sizeof(int16_t) * channels);
    for (size_t frame = 0; frame < frames; frame++) {
        int32_t mixed = 0;
        for (uint8_t ch = 0; ch < channels; ch++) {
            mixed += samples[frame * channels + ch];
        }
        samples[frame] = (int16_t)(mixed / channels);
    }
    return frames * sizeof(int16_t);
}

static esp_audio_simple_dec_handle_t open_mp3_decoder(void)
{
    esp_audio_simple_dec_cfg_t config = {
        .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,
        .dec_cfg = NULL,
        .cfg_size = 0,
        .use_frame_dec = false,
    };
    esp_audio_simple_dec_handle_t decoder = NULL;
    esp_audio_err_t err = esp_audio_simple_dec_open(&config, &decoder);
    if (err != ESP_AUDIO_ERR_OK) {
        ESP_LOGW(TAG, "MP3 decoder open failed: %d", err);
        return NULL;
    }
    return decoder;
}

static bool stream_station(size_t station, uint32_t generation)
{
    const radio_station_t preset = RADIO_STATIONS[station % station_count()];
    set_state(TOOLS_RADIO_CONNECTING, "正在连接", ESP_OK);

    esp_http_client_config_t config = {
        .url = preset.url,
        .timeout_ms = 7000,
        .buffer_size = 2048,
        .buffer_size_tx = 512,
        .user_agent = "AI-Passport-Tools-Radio/1.0",
        .keep_alive_enable = true,
        .disable_auto_redirect = false,
        .max_redirection_count = 4,
    };
    if (starts_with(preset.url, "https://")) {
        config.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        set_state(TOOLS_RADIO_ERROR, "内存不足", ESP_ERR_NO_MEM);
        return false;
    }
    esp_http_client_set_header(client, "Icy-MetaData", "0");
    esp_http_client_set_header(client, "Accept", "audio/mpeg,*/*");

    bool played_audio = false;
    esp_audio_simple_dec_handle_t decoder = NULL;
    uint8_t *input = NULL;
    uint8_t *output = NULL;

    do {
        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            set_state(TOOLS_RADIO_ERROR, "连接失败", err);
            break;
        }
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status < 200 || status >= 300) {
            ESP_LOGW(TAG, "HTTP status %d for %s", status, preset.url);
            set_state(TOOLS_RADIO_ERROR, "电台无响应", ESP_FAIL);
            break;
        }

        decoder = open_mp3_decoder();
        input = (uint8_t *)malloc(RADIO_INPUT_BYTES);
        output = (uint8_t *)malloc(RADIO_OUTPUT_BYTES);
        if (!decoder || !input || !output) {
            set_state(TOOLS_RADIO_ERROR, "内存不足", ESP_ERR_NO_MEM);
            break;
        }

        set_state(TOOLS_RADIO_BUFFERING, "正在缓冲", ESP_OK);
        bool format_ready = false;
        uint8_t source_channels = 1;
        int empty_reads = 0;

        while (request_current(generation, station)) {
            int received = esp_http_client_read(client, (char *)input, RADIO_INPUT_BYTES);
            if (received < 0) {
                set_state(TOOLS_RADIO_ERROR, "读取失败", ESP_FAIL);
                break;
            }
            if (received == 0) {
                if (++empty_reads > 3) break;
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            empty_reads = 0;

            esp_audio_simple_dec_raw_t raw = {
                .buffer = input,
                .len = (uint32_t)received,
                .eos = false,
                .consumed = 0,
                .frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE,
            };
            while (raw.len > 0 && request_current(generation, station)) {
                esp_audio_simple_dec_out_t frame = {
                    .buffer = output,
                    .len = RADIO_OUTPUT_BYTES,
                    .needed_size = 0,
                    .decoded_size = 0,
                };
                uint32_t before = raw.len;
                esp_audio_err_t result = esp_audio_simple_dec_process(decoder, &raw, &frame);
                if (result == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                    ESP_LOGW(TAG, "decoder needs %lu bytes",
                             (unsigned long)frame.needed_size);
                    raw.len = 0;
                    break;
                }
                if (result != ESP_AUDIO_ERR_OK) {
                    ESP_LOGW(TAG, "MP3 decode failed: %d", result);
                    raw.len = 0;
                    break;
                }
                if (raw.consumed > raw.len) raw.consumed = raw.len;
                raw.buffer += raw.consumed;
                raw.len -= raw.consumed;

                if (frame.decoded_size > 0) {
                    esp_audio_simple_dec_info_t info = {0};
                    if (!format_ready &&
                        esp_audio_simple_dec_get_info(decoder, &info) == ESP_AUDIO_ERR_OK) {
                        source_channels = info.channel > 2 ? 2 : info.channel;
                        if (source_channels == 0) source_channels = 1;
                        if (info.bits_per_sample != 16 ||
                            info.sample_rate < 8000 || info.sample_rate > 48000 ||
                            bsp_audio_set_format(info.sample_rate, 16, 1) != ESP_OK) {
                            ESP_LOGW(TAG, "unsupported stream: %luHz/%ubit/%uch",
                                     (unsigned long)info.sample_rate,
                                     info.bits_per_sample, source_channels);
                            set_state(TOOLS_RADIO_ERROR, "格式不支持", ESP_FAIL);
                            raw.len = 0;
                            break;
                        }
                        state_lock();
                        uint8_t volume = s_volume;
                        state_unlock();
                        bsp_audio_set_volume(volume);
                        format_ready = true;
                        ESP_LOGI(TAG, "playing %s at %luHz/%uch", preset.name,
                                 (unsigned long)info.sample_rate, source_channels);
                    }
                    if (format_ready) {
                        size_t mono_bytes =
                            downmix_to_mono(frame.buffer, frame.decoded_size,
                                            source_channels);
                        if (bsp_audio_write(frame.buffer, mono_bytes) == ESP_OK &&
                            !played_audio) {
                            played_audio = true;
                            set_state(TOOLS_RADIO_PLAYING, "正在播放", ESP_OK);
                        }
                    }
                }

                if (raw.len == before || raw.consumed == 0) break;
            }
        }
    } while (false);

    if (decoder) esp_audio_simple_dec_close(decoder);
    free(output);
    free(input);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return played_audio;
}

static void radio_task(void *arg)
{
    (void)arg;
    size_t last_station = SIZE_MAX;
    uint8_t failed_attempts = 0;

    for (;;) {
        state_lock();
        bool network_ready = s_network_ready;
        bool wanted = s_wanted_playing;
        size_t station = s_station_index;
        uint32_t generation = s_generation;
        state_unlock();

        if (!network_ready) {
            set_state(TOOLS_RADIO_STOPPED, "等待网络", ESP_OK);
            vTaskDelay(pdMS_TO_TICKS(300));
            continue;
        }
        if (!wanted) {
            set_state(TOOLS_RADIO_STOPPED, "已暂停", ESP_OK);
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        if (station != last_station) {
            last_station = station;
            failed_attempts = 0;
        }

        state_lock();
        s_stream_active = true;
        state_unlock();
        bool played = stream_station(station, generation);
        state_lock();
        s_stream_active = false;
        state_unlock();

        if (!request_current(generation, station)) continue;
        if (played) {
            failed_attempts = 0;
            set_state(TOOLS_RADIO_RECONNECTING, "正在重连", ESP_OK);
            vTaskDelay(pdMS_TO_TICKS(1200));
            continue;
        }

        failed_attempts++;
        if (failed_attempts >= RADIO_MAX_FAILED_ATTEMPTS && station_count() > 1) {
            set_state(TOOLS_RADIO_ERROR, "已自动换台", ESP_FAIL);
            vTaskDelay(pdMS_TO_TICKS(1200));
            tools_radio_next();
            failed_attempts = 0;
            continue;
        }
        set_state(TOOLS_RADIO_ERROR, "正在重试", ESP_FAIL);
        vTaskDelay(pdMS_TO_TICKS(2500));
    }
}

esp_err_t tools_radio_init(void)
{
    if (s_initialized) return ESP_OK;
    if (!s_state_lock) {
        s_state_lock = xSemaphoreCreateMutex();
        if (!s_state_lock) return ESP_ERR_NO_MEM;
    }

    s_station_index = load_u8("station", 0) % station_count();
    s_volume = clamp_volume(load_u8("volume", s_volume));
    save_u8("station", (uint8_t)s_station_index);
    save_u8("volume", s_volume);

    esp_err_t err = bsp_audio_init();
    if (err != ESP_OK) {
        set_state(TOOLS_RADIO_ERROR, "音频初始化失败", err);
        return err;
    }
    if (esp_audio_dec_register_default() != ESP_AUDIO_ERR_OK ||
        esp_audio_simple_dec_register_default() != ESP_AUDIO_ERR_OK) {
        set_state(TOOLS_RADIO_ERROR, "解码器初始化失败", ESP_FAIL);
        return ESP_FAIL;
    }
    bsp_audio_set_volume(s_volume);
    s_wanted_playing = false;
    set_state(TOOLS_RADIO_STOPPED, "已暂停", ESP_OK);

    if (xTaskCreate(radio_task, "tools_radio", 7168, NULL, 6, &s_radio_task) != pdPASS) {
        s_radio_task = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_initialized = true;
    return ESP_OK;
}

void tools_radio_set_network_ready(bool ready)
{
    if (!s_initialized) return;
    state_lock();
    if (s_network_ready != ready) {
        s_network_ready = ready;
        s_generation++;
        if (s_generation == 0) s_generation = 1;
    }
    state_unlock();
}

void tools_radio_toggle(void)
{
    if (tools_radio_init() != ESP_OK) return;
    state_lock();
    s_wanted_playing = !s_wanted_playing;
    bool wanted = s_wanted_playing;
    s_generation++;
    if (s_generation == 0) s_generation = 1;
    state_unlock();
    if (!wanted) set_state(TOOLS_RADIO_STOPPED, "已暂停", ESP_OK);
}

void tools_radio_stop(void)
{
    if (!s_initialized) return;
    state_lock();
    s_wanted_playing = false;
    s_generation++;
    if (s_generation == 0) s_generation = 1;
    state_unlock();

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(RADIO_STOP_TIMEOUT_MS);
    for (;;) {
        state_lock();
        bool active = s_stream_active;
        state_unlock();
        if (!active) break;
        if ((int32_t)(deadline - xTaskGetTickCount()) <= 0) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    set_state(TOOLS_RADIO_STOPPED, "已暂停", ESP_OK);
}

void tools_radio_next(void)
{
    if (tools_radio_init() != ESP_OK) return;
    state_lock();
    s_station_index = (s_station_index + 1u) % station_count();
    uint8_t station = (uint8_t)s_station_index;
    state_unlock();
    save_u8("station", station);
    bump_generation();
}

void tools_radio_prev(void)
{
    if (tools_radio_init() != ESP_OK) return;
    state_lock();
    s_station_index = (s_station_index + station_count() - 1u) % station_count();
    uint8_t station = (uint8_t)s_station_index;
    state_unlock();
    save_u8("station", station);
    bump_generation();
}

void tools_radio_volume_up(void)
{
    if (tools_radio_init() != ESP_OK) return;
    state_lock();
    uint8_t volume = s_volume >= RADIO_MAX_VOLUME - RADIO_VOLUME_STEP ?
        RADIO_MAX_VOLUME : (uint8_t)(s_volume + RADIO_VOLUME_STEP);
    s_volume = volume;
    state_unlock();
    bsp_audio_set_volume(volume);
    save_u8("volume", volume);
}

void tools_radio_volume_down(void)
{
    if (tools_radio_init() != ESP_OK) return;
    state_lock();
    uint8_t volume = s_volume <= RADIO_MIN_VOLUME + RADIO_VOLUME_STEP ?
        RADIO_MIN_VOLUME : (uint8_t)(s_volume - RADIO_VOLUME_STEP);
    s_volume = volume;
    state_unlock();
    bsp_audio_set_volume(volume);
    save_u8("volume", volume);
}

tools_radio_status_t tools_radio_status(void)
{
    tools_radio_status_t status = {0};
    state_lock();
    status.state = s_state;
    status.error = s_error;
    status.wanted_playing = s_wanted_playing;
    status.network_ready = s_network_ready;
    status.station_index = s_station_index;
    status.station_count = station_count();
    status.volume = s_volume;
    snprintf(status.detail, sizeof(status.detail), "%s", s_detail);
    state_unlock();

    const radio_station_t *station = &RADIO_STATIONS[status.station_index % station_count()];
    snprintf(status.station_name, sizeof(status.station_name), "%s", station->name);
    snprintf(status.station_desc, sizeof(status.station_desc), "%s", station->desc);
    return status;
}
