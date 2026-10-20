// Wi-Fi STA + SoftAP web provisioning for the tools app.
#include "tools_wifi.h"

#include "demo_radio.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "lwip/ip4_addr.h"
#include "nvs.h"
#include "tools_storage.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static const char *TAG = "tools_wifi";

#define TOOLS_WIFI_SNTP_SERVER "ntp.aliyun.com"
#define WIFI_STORE_NAMESPACE "wifi"
#define WIFI_STORE_KEY "sta"
#define WIFI_STORE_MAGIC_LEGACY 0x54574649u
#define WIFI_STORE_MAGIC_MULTI 0x5457464Du
#define WIFI_MAX_CREDENTIALS 5
#define WIFI_SCAN_RESULT_COUNT 12
#define WIFI_SETUP_MAX_BODY 512

static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static esp_event_handler_instance_t s_wifi_handler;
static esp_event_handler_instance_t s_ip_handler;
static httpd_handle_t s_httpd;
static wifi_config_t s_sta_config;
static volatile tools_wifi_state_t s_state = TOOLS_WIFI_OFF;
static volatile esp_err_t s_error;
static char s_ip[16];
static bool s_wifi_initialized;
static bool s_wifi_started;
static bool s_wifi_handler_registered;
static bool s_ip_handler_registered;
static bool s_phone_connected;
static bool s_wifi_connecting;
static bool s_wifi_got_ip;
static bool s_reconnect_after_disconnect;
static bool s_provisioning;
static bool s_save_on_connect;
static bool s_scan_running;
static bool s_pending_connect_after_scan;
static bool s_pending_save_on_connect;
static bool s_sntp_started;
static volatile bool s_time_synced;
static volatile bool s_time_sync_pending;
static wifi_config_t s_saved_configs[WIFI_MAX_CREDENTIALS];
static size_t s_saved_count;
static wifi_ap_record_t s_scan_records[WIFI_SCAN_RESULT_COUNT];
static uint16_t s_scan_count;

typedef struct {
    uint32_t magic;
    uint32_t size;
    wifi_config_t config;
} tools_wifi_legacy_store_blob_t;

typedef struct {
    uint32_t magic;
    uint32_t size;
    uint32_t count;
    wifi_config_t configs[WIFI_MAX_CREDENTIALS];
} tools_wifi_store_blob_t;

static const char INDEX_HTML_HEAD[] =
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>AI Passport Wi-Fi Setup</title>"
    "<style>body{font-family:-apple-system,BlinkMacSystemFont,sans-serif;margin:24px;background:#07120f;color:#eef1e8}"
    "main{max-width:420px;margin:auto}label{display:block;margin:16px 0 6px;color:#a8c2b2}"
    "input,select,button{box-sizing:border-box;width:100%;font-size:18px;padding:12px;border-radius:8px;border:1px solid #2b473b}"
    "input,select{background:#10211b;color:#eef1e8}button{margin-top:18px;background:#20e47c;color:#07120f;font-weight:700}"
    "p{line-height:1.5;color:#a8c2b2}.muted{font-size:14px;color:#6f887b}</style></head><body><main>"
    "<h1>AI Passport Wi-Fi Setup</h1>"
    "<p>Connect the device to a 2.4 GHz Wi-Fi network. Keep this phone connected to AI-Passport-Setup while submitting.</p>"
    "<form method=\"post\" action=\"/save\">"
    "<label for=\"ssid\">Nearby Wi-Fi</label><select id=\"ssid\" name=\"ssid\">"
    "<option value=\"\">Manual Wi-Fi name</option>";

static const char INDEX_HTML_TAIL[] =
    "</select><p class=\"muted\">Reload this page to refresh the scanned list.</p>"
    "<label for=\"custom_ssid\">Manual Wi-Fi name</label><input id=\"custom_ssid\" name=\"custom_ssid\" maxlength=\"31\">"
    "<label for=\"pass\">Password</label><input id=\"pass\" name=\"pass\" maxlength=\"63\" type=\"password\">"
    "<button type=\"submit\">Save and connect</button></form></main></body></html>";

static const char CONNECTING_HTML[] =
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>Connecting</title></head><body>"
    "<h1>Connecting...</h1><p>AI Passport is testing the new Wi-Fi settings. "
    "The setup hotspot will close after the device gets an IP address.</p>"
    "<p>If it does not connect, rejoin AI-Passport-Setup and try again.</p></body></html>";

static void copy_ssid(char out[33])
{
    memcpy(out, s_sta_config.sta.ssid, 32);
    out[32] = '\0';
}

static bool same_ssid(const uint8_t left[32], const uint8_t right[32])
{
    size_t left_len = strnlen((const char *)left, 32);
    size_t right_len = strnlen((const char *)right, 32);
    return left_len == right_len && memcmp(left, right, left_len) == 0;
}

static bool ssid_is_empty(const uint8_t ssid[32])
{
    return ssid[0] == '\0';
}

static void upsert_saved_credential(const wifi_config_t *config)
{
    if (ssid_is_empty(config->sta.ssid)) return;

    wifi_config_t next[WIFI_MAX_CREDENTIALS] = { 0 };
    size_t used = 0;
    next[used++] = *config;
    for (size_t i = 0; i < s_saved_count && used < WIFI_MAX_CREDENTIALS; i++) {
        if (!same_ssid(s_saved_configs[i].sta.ssid, config->sta.ssid)) {
            next[used++] = s_saved_configs[i];
        }
    }
    memcpy(s_saved_configs, next, sizeof(s_saved_configs));
    s_saved_count = used;
}

static esp_err_t save_credentials_to_tools_data(void)
{
    if (s_sta_config.sta.ssid[0] == '\0') return ESP_OK;
    upsert_saved_credential(&s_sta_config);

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open_from_partition(TOOLS_STORAGE_PARTITION,
                                            WIFI_STORE_NAMESPACE,
                                            NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    tools_wifi_store_blob_t blob = {
        .magic = WIFI_STORE_MAGIC_MULTI,
        .size = sizeof(tools_wifi_store_blob_t),
        .count = s_saved_count,
    };
    memcpy(blob.configs, s_saved_configs, sizeof(blob.configs));
    err = nvs_set_blob(handle, WIFI_STORE_KEY, &blob, sizeof(blob));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static esp_err_t clear_tools_data_credentials(void)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open_from_partition(TOOLS_STORAGE_PARTITION,
                                            WIFI_STORE_NAMESPACE,
                                            NVS_READWRITE, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;

    err = nvs_erase_key(handle, WIFI_STORE_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static bool load_tools_data_credentials(wifi_config_t *config)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open_from_partition(TOOLS_STORAGE_PARTITION,
                                            WIFI_STORE_NAMESPACE,
                                            NVS_READONLY, &handle);
    if (err != ESP_OK) return false;

    size_t size = 0;
    err = nvs_get_blob(handle, WIFI_STORE_KEY, NULL, &size);
    if (err != ESP_OK) {
        nvs_close(handle);
        return false;
    }

    tools_wifi_store_blob_t blob = { 0 };
    if (size == sizeof(blob)) {
        err = nvs_get_blob(handle, WIFI_STORE_KEY, &blob, &size);
        nvs_close(handle);
        if (err != ESP_OK || blob.magic != WIFI_STORE_MAGIC_MULTI ||
            blob.size != sizeof(tools_wifi_store_blob_t) ||
            blob.count == 0 || blob.count > WIFI_MAX_CREDENTIALS ||
            blob.configs[0].sta.ssid[0] == '\0') {
            return false;
        }
        memcpy(s_saved_configs, blob.configs, sizeof(s_saved_configs));
        s_saved_count = blob.count;
        *config = s_saved_configs[0];
        return true;
    }

    tools_wifi_legacy_store_blob_t legacy = { 0 };
    size = sizeof(legacy);
    err = nvs_get_blob(handle, WIFI_STORE_KEY, &legacy, &size);
    nvs_close(handle);
    if (err != ESP_OK || size != sizeof(legacy) ||
        legacy.magic != WIFI_STORE_MAGIC_LEGACY ||
        legacy.size != sizeof(wifi_config_t) ||
        legacy.config.sta.ssid[0] == '\0') {
        return false;
    }

    memset(s_saved_configs, 0, sizeof(s_saved_configs));
    s_saved_configs[0] = legacy.config;
    s_saved_count = 1;
    *config = legacy.config;
    return true;
}

static void set_state_for_idle(void)
{
    if (s_wifi_got_ip) {
        s_state = s_time_synced ? TOOLS_WIFI_TIME_SYNCED : TOOLS_WIFI_CONNECTED;
    } else if (s_wifi_connecting) {
        s_state = TOOLS_WIFI_CONNECTING;
    } else if (s_phone_connected) {
        s_state = TOOLS_WIFI_PHONE_CONNECTED;
    } else if (s_provisioning) {
        s_state = TOOLS_WIFI_SETUP_READY;
    } else if (s_saved_count == 0 && s_sta_config.sta.ssid[0] == '\0') {
        s_state = TOOLS_WIFI_NO_CREDENTIALS;
    } else {
        s_state = TOOLS_WIFI_CONNECTING;
    }
}

static void time_sync_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "SNTP time synced: %lld", (long long)tv->tv_sec);
    s_time_synced = true;
    s_time_sync_pending = true;
    if (s_wifi_got_ip) s_state = TOOLS_WIFI_TIME_SYNCED;
}

static void start_or_restart_sntp(void)
{
    if (s_sntp_started) {
        esp_sntp_restart();
        return;
    }
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, TOOLS_WIFI_SNTP_SERVER);
    esp_sntp_set_time_sync_notification_cb(time_sync_cb);
    esp_sntp_init();
    s_sntp_started = true;
    ESP_LOGI(TAG, "SNTP started: %s", TOOLS_WIFI_SNTP_SERVER);
}

static void stop_http_server(void)
{
    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
}

static void stop_softap_keep_sta(void)
{
    stop_http_server();
    s_phone_connected = false;
    s_provisioning = false;
    if (s_wifi_started) {
        esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "SoftAP stop mode switch failed: %s", esp_err_to_name(err));
        }
    }
    if (s_ap_netif) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
    }
    set_state_for_idle();
}

static void connect_current_config(bool save_on_connect)
{
    if (!s_wifi_started || s_sta_config.sta.ssid[0] == '\0') {
        set_state_for_idle();
        return;
    }
    if (s_wifi_connecting) return;

    s_sta_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    s_sta_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    s_sta_config.sta.failure_retry_cnt = 3;
    esp_wifi_set_config(WIFI_IF_STA, &s_sta_config);

    bool was_connected = s_wifi_got_ip;
    s_save_on_connect = save_on_connect;
    s_wifi_connecting = true;
    s_wifi_got_ip = false;
    s_time_synced = false;
    s_state = TOOLS_WIFI_CONNECTING;
    if (was_connected) {
        s_reconnect_after_disconnect = true;
        if (esp_wifi_disconnect() == ESP_OK) return;
        s_reconnect_after_disconnect = false;
    }
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        s_error = err;
        s_wifi_connecting = false;
        s_save_on_connect = false;
        s_state = TOOLS_WIFI_FAILED;
    }
}

static bool scan_record_matches_config(const wifi_ap_record_t *record,
                                       const wifi_config_t *config)
{
    return same_ssid(record->ssid, config->sta.ssid);
}

static bool select_scanned_saved_config(void)
{
    if (s_saved_count == 0) return false;
    int best_index = -1;
    int best_rssi = -128;
    for (uint16_t r = 0; r < s_scan_count; r++) {
        if (ssid_is_empty(s_scan_records[r].ssid)) continue;
        for (size_t i = 0; i < s_saved_count; i++) {
            if (scan_record_matches_config(&s_scan_records[r], &s_saved_configs[i]) &&
                s_scan_records[r].rssi > best_rssi) {
                best_index = (int)i;
                best_rssi = s_scan_records[r].rssi;
            }
        }
    }
    if (best_index < 0) return false;
    s_sta_config = s_saved_configs[best_index];
    ESP_LOGI(TAG, "selected saved Wi-Fi from scan, index=%d rssi=%d",
             best_index, best_rssi);
    return true;
}

static void start_wifi_scan(bool connect_after_scan, bool save_on_connect)
{
    if (!s_wifi_started) return;
    if (s_scan_running) {
        s_pending_connect_after_scan |= connect_after_scan;
        s_pending_save_on_connect |= save_on_connect;
        return;
    }
    wifi_scan_config_t config = {
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    esp_err_t err = esp_wifi_scan_start(&config, false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi scan start failed: %s", esp_err_to_name(err));
        if (connect_after_scan) {
            if (s_saved_count > 0) s_sta_config = s_saved_configs[0];
            connect_current_config(save_on_connect);
        }
        return;
    }
    s_scan_running = true;
    s_pending_connect_after_scan |= connect_after_scan;
    s_pending_save_on_connect |= save_on_connect;
}

static void request_wifi_connect(bool save_on_connect)
{
    if (save_on_connect) {
        connect_current_config(true);
        return;
    }
    if (s_saved_count == 0 && s_sta_config.sta.ssid[0] != '\0') {
        connect_current_config(false);
        return;
    }
    if (s_saved_count == 0) {
        set_state_for_idle();
        return;
    }
    s_state = TOOLS_WIFI_CONNECTING;
    start_wifi_scan(true, false);
}

static int hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

static bool url_decode_field(const char *in, char *out, size_t out_len)
{
    size_t used = 0;
    while (*in) {
        char ch = *in++;
        if (ch == '+') {
            ch = ' ';
        } else if (ch == '%') {
            int hi = hex_value(in[0]);
            int lo = hex_value(in[1]);
            if (hi < 0 || lo < 0) return false;
            ch = (char)((hi << 4) | lo);
            in += 2;
        }
        if (used + 1 >= out_len) return false;
        out[used++] = ch;
    }
    out[used] = '\0';
    return true;
}

static bool form_field(const char *body, const char *name, char *out, size_t out_len)
{
    size_t name_len = strlen(name);
    const char *p = body;
    while (*p) {
        const char *end = strchr(p, '&');
        size_t item_len = end ? (size_t)(end - p) : strlen(p);
        if (item_len > name_len + 1 && memcmp(p, name, name_len) == 0 &&
            p[name_len] == '=') {
            char encoded[128];
            size_t value_len = item_len - name_len - 1u;
            if (value_len >= sizeof(encoded)) return false;
            memcpy(encoded, p + name_len + 1, value_len);
            encoded[value_len] = '\0';
            return url_decode_field(encoded, out, out_len);
        }
        if (!end) break;
        p = end + 1;
    }
    out[0] = '\0';
    return false;
}

static char *append_text(char *out, char *end, const char *text)
{
    while (*text && out < end) *out++ = *text++;
    return out;
}

static char *append_html_escaped(char *out, char *end, const char *text)
{
    while (*text && out < end) {
        const char *escaped = NULL;
        switch (*text) {
        case '&':
            escaped = "&amp;";
            break;
        case '<':
            escaped = "&lt;";
            break;
        case '>':
            escaped = "&gt;";
            break;
        case '"':
            escaped = "&quot;";
            break;
        default:
            *out++ = *text++;
            continue;
        }
        out = append_text(out, end, escaped);
        text++;
    }
    return out;
}

static esp_err_t index_handler(httpd_req_t *req)
{
    start_wifi_scan(false, false);
    char *html = malloc(4096);
    if (!html) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
        return ESP_FAIL;
    }
    char *out = html;
    char *end = html + 4095;
    out = append_text(out, end, INDEX_HTML_HEAD);
    for (uint16_t i = 0; i < s_scan_count; i++) {
        char ssid[33];
        memcpy(ssid, s_scan_records[i].ssid, 32);
        ssid[32] = '\0';
        if (ssid[0] == '\0') continue;
        out = append_text(out, end, "<option value=\"");
        out = append_html_escaped(out, end, ssid);
        out = append_text(out, end, "\">");
        out = append_html_escaped(out, end, ssid);
        out = append_text(out, end, " (");
        char rssi[12];
        snprintf(rssi, sizeof(rssi), "%d", s_scan_records[i].rssi);
        out = append_text(out, end, rssi);
        out = append_text(out, end, " dBm)</option>");
    }
    out = append_text(out, end, INDEX_HTML_TAIL);
    *out = '\0';
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    esp_err_t err = httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    free(html);
    return err;
}

static esp_err_t save_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > WIFI_SETUP_MAX_BODY) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form size");
        return ESP_FAIL;
    }

    char body[WIFI_SETUP_MAX_BODY + 1];
    int received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Receive failed");
            return ESP_FAIL;
        }
        received += ret;
    }
    body[received] = '\0';

    char ssid[33];
    char custom_ssid[33];
    char password[65];
    (void)form_field(body, "ssid", ssid, sizeof(ssid));
    (void)form_field(body, "custom_ssid", custom_ssid, sizeof(custom_ssid));
    if (custom_ssid[0] != '\0') {
        strlcpy(ssid, custom_ssid, sizeof(ssid));
    }
    if (ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing Wi-Fi name");
        return ESP_FAIL;
    }
    (void)form_field(body, "pass", password, sizeof(password));

    wifi_config_t next = { 0 };
    strlcpy((char *)next.sta.ssid, ssid, sizeof(next.sta.ssid));
    strlcpy((char *)next.sta.password, password, sizeof(next.sta.password));
    next.sta.threshold.authmode = WIFI_AUTH_OPEN;
    next.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    next.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    next.sta.failure_retry_cnt = 3;
    s_sta_config = next;
    request_wifi_connect(true);

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, CONNECTING_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t start_http_server(void)
{
    if (s_httpd) return ESP_OK;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 4;
    config.backlog_conn = 2;
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 10;
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_httpd, &config);
    if (err != ESP_OK) return err;

    httpd_uri_t index = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_handler,
        .user_ctx = NULL,
    };
    httpd_uri_t save = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = save_handler,
        .user_ctx = NULL,
    };
    err = httpd_register_uri_handler(s_httpd, &index);
    if (err == ESP_OK) err = httpd_register_uri_handler(s_httpd, &save);
    if (err != ESP_OK) {
        stop_http_server();
    }
    return err;
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == WIFI_EVENT_STA_START) {
        s_wifi_started = true;
        esp_wifi_get_config(WIFI_IF_STA, &s_sta_config);
        if (s_sta_config.sta.ssid[0] != '\0') {
            ESP_LOGI(TAG, "Wi-Fi credentials found");
        }
        set_state_for_idle();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = data;
        ESP_LOGW(TAG, "Wi-Fi disconnected, reason=%u", event->reason);
        if (s_reconnect_after_disconnect) {
            s_reconnect_after_disconnect = false;
            esp_wifi_connect();
            return;
        }
        bool provisioning_attempt = s_save_on_connect;
        s_wifi_connecting = false;
        s_wifi_got_ip = false;
        s_save_on_connect = false;
        s_ip[0] = '\0';
        if (provisioning_attempt) {
            s_state = TOOLS_WIFI_FAILED;
        } else {
            set_state_for_idle();
        }
    } else if (id == WIFI_EVENT_AP_STACONNECTED) {
        s_phone_connected = true;
        set_state_for_idle();
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        s_phone_connected = false;
        set_state_for_idle();
    } else if (id == WIFI_EVENT_SCAN_DONE) {
        s_scan_running = false;
        uint16_t count = WIFI_SCAN_RESULT_COUNT;
        memset(s_scan_records, 0, sizeof(s_scan_records));
        esp_err_t err = esp_wifi_scan_get_ap_records(&count, s_scan_records);
        if (err == ESP_OK) {
            s_scan_count = count;
        } else {
            s_scan_count = 0;
            ESP_LOGW(TAG, "Wi-Fi scan read failed: %s", esp_err_to_name(err));
        }
        if (s_pending_connect_after_scan) {
            bool save_on_connect = s_pending_save_on_connect;
            s_pending_connect_after_scan = false;
            s_pending_save_on_connect = false;
            if (!select_scanned_saved_config() && s_saved_count > 0) {
                s_sta_config = s_saved_configs[0];
            }
            connect_current_config(save_on_connect);
        }
    }
}

static void ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id != IP_EVENT_STA_GOT_IP) return;
    ip_event_got_ip_t *event = data;
    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&event->ip_info.ip));
    s_wifi_connecting = false;
    s_wifi_got_ip = true;
    s_state = s_time_synced ? TOOLS_WIFI_TIME_SYNCED : TOOLS_WIFI_CONNECTED;
    if (s_save_on_connect) {
        esp_err_t save_err = save_credentials_to_tools_data();
        if (save_err != ESP_OK) {
            ESP_LOGW(TAG, "保存 Wi-Fi 配置失败: %s", esp_err_to_name(save_err));
        }
        s_save_on_connect = false;
    }
    start_or_restart_sntp();
    if (s_provisioning) stop_softap_keep_sta();
}

static esp_err_t wifi_start(void)
{
    if (s_wifi_initialized) return ESP_OK;
    s_state = TOOLS_WIFI_STARTING;
    esp_err_t err = demo_radio_nvs_prepare();
    if (err != ESP_OK) return err;
    err = demo_radio_network_prepare();
    if (err != ESP_OK) return err;

    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (!s_sta_netif) return ESP_ERR_NO_MEM;
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&config);
    if (err != ESP_OK) return err;
    s_wifi_initialized = true;
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              wifi_event, NULL, &s_wifi_handler);
    if (err != ESP_OK) return err;
    s_wifi_handler_registered = true;
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              ip_event, NULL, &s_ip_handler);
    if (err != ESP_OK) return err;
    s_ip_handler_registered = true;
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err == ESP_OK) err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err == ESP_OK) {
        s_wifi_started = true;
        if (load_tools_data_credentials(&s_sta_config)) {
            ESP_LOGI(TAG, "Wi-Fi credentials restored from tools_data");
            esp_wifi_set_config(WIFI_IF_STA, &s_sta_config);
        } else {
            esp_wifi_get_config(WIFI_IF_STA, &s_sta_config);
        }
        set_state_for_idle();
    }
    return err;
}

esp_err_t tools_wifi_init(void)
{
    esp_err_t err = wifi_start();
    if (err != ESP_OK) {
        s_error = err;
        s_state = TOOLS_WIFI_FAILED;
    }
    return err;
}

esp_err_t tools_wifi_start_provisioning(void)
{
    esp_err_t err = tools_wifi_init();
    if (err != ESP_OK) {
        s_error = err;
        s_state = TOOLS_WIFI_FAILED;
        return err;
    }
    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (!s_ap_netif) {
            s_error = ESP_ERR_NO_MEM;
            s_state = TOOLS_WIFI_FAILED;
            return ESP_ERR_NO_MEM;
        }
    }

    wifi_config_t ap_config = { 0 };
    strlcpy((char *)ap_config.ap.ssid, TOOLS_WIFI_SETUP_SSID,
            sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen(TOOLS_WIFI_SETUP_SSID);
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 1;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;

    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (err == ESP_OK) err = start_http_server();
    if (err != ESP_OK) {
        s_error = err;
        s_state = TOOLS_WIFI_FAILED;
        stop_http_server();
        return err;
    }

    s_provisioning = true;
    s_error = ESP_OK;
    start_wifi_scan(false, false);
    set_state_for_idle();
    return ESP_OK;
}

void tools_wifi_stop_provisioning(void)
{
    stop_softap_keep_sta();
}

esp_err_t tools_wifi_stop_all(void)
{
    tools_wifi_stop_provisioning();

    if (s_sntp_started) {
        esp_sntp_stop();
        s_sntp_started = false;
    }
    if (s_wifi_started) {
        (void)esp_wifi_disconnect();
        esp_err_t err = esp_wifi_stop();
        if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
            ESP_LOGW(TAG, "Wi-Fi stop failed: %s", esp_err_to_name(err));
        }
        s_wifi_started = false;
    }
    if (s_wifi_handler_registered) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                              s_wifi_handler);
        s_wifi_handler_registered = false;
    }
    if (s_ip_handler_registered) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                              s_ip_handler);
        s_ip_handler_registered = false;
    }
    if (s_wifi_initialized) {
        esp_err_t err = esp_wifi_deinit();
        if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
            ESP_LOGW(TAG, "Wi-Fi deinit failed: %s", esp_err_to_name(err));
        }
        s_wifi_initialized = false;
    }
    if (s_sta_netif) {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
    }

    s_wifi_connecting = false;
    s_wifi_got_ip = false;
    s_phone_connected = false;
    s_reconnect_after_disconnect = false;
    s_provisioning = false;
    s_save_on_connect = false;
    s_scan_running = false;
    s_pending_connect_after_scan = false;
    s_pending_save_on_connect = false;
    s_time_synced = false;
    s_time_sync_pending = false;
    s_scan_count = 0;
    s_ip[0] = '\0';
    s_state = TOOLS_WIFI_OFF;
    s_error = ESP_OK;
    return ESP_OK;
}

esp_err_t tools_wifi_reconnect(void)
{
    esp_err_t err = tools_wifi_init();
    if (err == ESP_OK) request_wifi_connect(false);
    return err;
}

esp_err_t tools_wifi_clear_credentials(void)
{
    esp_err_t err = tools_wifi_init();
    wifi_config_t empty = { 0 };
    s_reconnect_after_disconnect = false;
    s_save_on_connect = false;
    if (s_wifi_started) esp_wifi_disconnect();
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_STA, &empty);
    esp_err_t clear_err = clear_tools_data_credentials();
    if (clear_err != ESP_OK) {
        ESP_LOGW(TAG, "清除小工具 Wi-Fi 配置失败: %s", esp_err_to_name(clear_err));
    }
    memset(&s_sta_config, 0, sizeof(s_sta_config));
    memset(s_saved_configs, 0, sizeof(s_saved_configs));
    s_saved_count = 0;
    s_wifi_connecting = false;
    s_wifi_got_ip = false;
    s_time_synced = false;
    s_ip[0] = '\0';
    set_state_for_idle();
    return err;
}

tools_wifi_status_t tools_wifi_status(void)
{
    tools_wifi_status_t status = {
        .state = s_state,
        .error = s_error,
        .provisioning = s_provisioning,
        .time_synced = s_time_synced,
    };
    copy_ssid(status.ssid);
    strncpy(status.ip, s_ip, sizeof(status.ip) - 1);
    return status;
}

bool tools_wifi_take_time_synced(void)
{
    if (!s_time_sync_pending) return false;
    s_time_sync_pending = false;
    return true;
}
