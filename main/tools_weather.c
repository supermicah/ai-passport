// Lightweight weather fetcher for the tools app home dashboard.
#include "tools_weather.h"

#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tools_wifi.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *TAG = "tools_weather";

#define WEATHER_URL "http://wttr.in/Hangzhou?format=%t"
#define WEATHER_REFRESH_SEC (30u * 60u)
#define WEATHER_RETRY_SEC (5u * 60u)
#define WEATHER_STOP_TIMEOUT_MS 7000u
#define WEATHER_NOTIFY_REFRESH 0x01u
#define WEATHER_NOTIFY_STOP 0x02u

static TaskHandle_t s_weather_task;
static SemaphoreHandle_t s_weather_stopped;
static volatile bool s_refresh_requested = true;
static tools_weather_status_t s_status = {
    .valid = false,
    .updating = false,
    .error = ESP_OK,
    .updated_at = 0,
    .summary = TOOLS_WEATHER_CITY " --",
};

static uint32_t weather_now_sec(void)
{
    time_t raw = time(NULL);
    if (raw < 0) raw = 0;
    return (uint32_t)raw;
}

static bool weather_wifi_ready(void)
{
    tools_wifi_status_t wifi = tools_wifi_status();
    return wifi.state == TOOLS_WIFI_CONNECTED ||
           wifi.state == TOOLS_WIFI_TIME_SYNCED;
}

static void normalize_weather_text(char *text)
{
    size_t out = 0;
    for (size_t i = 0; text[i] && out < 16; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (ch == '\r' || ch == '\n' || ch == ' ') continue;
        if (ch == '+') continue;
        if (ch == '\xc2' && (unsigned char)text[i + 1] == '\xb0') {
            text[out++] = 'C';
            i++;
            if (text[i + 1] == 'C') i++;
            continue;
        }
        if (isprint(ch)) text[out++] = (char)ch;
    }
    text[out] = '\0';
}

static esp_err_t fetch_weather(char *summary, size_t summary_len)
{
    char body[32] = {0};
    esp_http_client_config_t config = {
        .url = WEATHER_URL,
        .timeout_ms = 5000,
        .buffer_size = 128,
        .buffer_size_tx = 128,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_ERR_NO_MEM;

    esp_err_t err = esp_http_client_open(client, 0);
    if (err == ESP_OK) {
        (void)esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        int read_len = esp_http_client_read_response(client, body, sizeof(body) - 1);
        if (status != 200 || read_len <= 0) {
            err = ESP_FAIL;
        } else {
            body[read_len] = '\0';
            normalize_weather_text(body);
            if (body[0] == '\0') {
                err = ESP_FAIL;
            } else {
                snprintf(summary, summary_len, TOOLS_WEATHER_CITY " %s", body);
            }
        }
    }
    esp_http_client_cleanup(client);
    return err;
}

static void weather_task(void *arg)
{
    (void)arg;
    uint32_t last_attempt = 0;
    for (;;) {
        uint32_t notify = 0;
        if (xTaskNotifyWait(0, UINT32_MAX, &notify,
                            pdMS_TO_TICKS(10000)) == pdTRUE) {
            if (notify & WEATHER_NOTIFY_STOP) break;
            if (notify & WEATHER_NOTIFY_REFRESH) s_refresh_requested = true;
        }
        uint32_t now = weather_now_sec();
        uint32_t retry = s_status.valid ? WEATHER_REFRESH_SEC : WEATHER_RETRY_SEC;
        bool due = s_refresh_requested || last_attempt == 0 ||
                   now - last_attempt >= retry;
        if (due && weather_wifi_ready()) {
            char summary[sizeof(s_status.summary)] = {0};
            s_refresh_requested = false;
            s_status.updating = true;
            last_attempt = now;
            esp_err_t err = fetch_weather(summary, sizeof(summary));
            if (err == ESP_OK) {
                strncpy(s_status.summary, summary, sizeof(s_status.summary) - 1);
                s_status.summary[sizeof(s_status.summary) - 1] = '\0';
                s_status.valid = true;
                s_status.updated_at = weather_now_sec();
                ESP_LOGI(TAG, "weather updated: %s", s_status.summary);
            } else {
                s_status.error = err;
                ESP_LOGW(TAG, "weather update failed: %s", esp_err_to_name(err));
            }
            s_status.updating = false;
        }
    }

    s_status.updating = false;
    s_weather_task = NULL;
    if (s_weather_stopped) xSemaphoreGive(s_weather_stopped);
    vTaskDelete(NULL);
}

esp_err_t tools_weather_init(void)
{
    if (s_weather_task) return ESP_OK;
    if (s_weather_stopped) {
        vSemaphoreDelete(s_weather_stopped);
        s_weather_stopped = NULL;
    }
    s_weather_stopped = xSemaphoreCreateBinary();
    if (!s_weather_stopped) return ESP_ERR_NO_MEM;
    s_refresh_requested = true;
    if (xTaskCreate(weather_task, "tools_weather", 4096, NULL, 3,
                    &s_weather_task) != pdPASS) {
        vSemaphoreDelete(s_weather_stopped);
        s_weather_stopped = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t tools_weather_stop(void)
{
    TaskHandle_t task = s_weather_task;
    if (!task) return ESP_OK;

    xTaskNotify(task, WEATHER_NOTIFY_STOP, eSetBits);
    if (!s_weather_stopped ||
        xSemaphoreTake(s_weather_stopped,
                       pdMS_TO_TICKS(WEATHER_STOP_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "weather task stop timed out");
        return ESP_ERR_TIMEOUT;
    }
    vSemaphoreDelete(s_weather_stopped);
    s_weather_stopped = NULL;
    return ESP_OK;
}

tools_weather_status_t tools_weather_status(void)
{
    return s_status;
}

void tools_weather_request_refresh(void)
{
    s_refresh_requested = true;
    if (s_weather_task) {
        xTaskNotify(s_weather_task, WEATHER_NOTIFY_REFRESH, eSetBits);
    }
}
