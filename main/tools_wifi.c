// Wi-Fi STA + BLUFI provisioning for the tools app.
#include "tools_wifi.h"

#include "demo_radio.h"
#include "esp_blufi.h"
#include "esp_blufi_api.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_hs.h"
#include "lwip/ip4_addr.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "nvs.h"
#include "tools_storage.h"
#include "tools_wifi_security.h"

#include <string.h>
#include <sys/time.h>

static const char *TAG = "tools_wifi";

#define BLUFI_AP_LIST_COUNT 16
#define TOOLS_WIFI_SNTP_SERVER "ntp.aliyun.com"
#define WIFI_STORE_NAMESPACE "wifi"
#define WIFI_STORE_KEY "sta"
#define WIFI_STORE_MAGIC 0x54574649u

static esp_netif_t *s_sta_netif;
static esp_event_handler_instance_t s_wifi_handler;
static esp_event_handler_instance_t s_ip_handler;
static wifi_config_t s_sta_config;
static volatile tools_wifi_state_t s_state = TOOLS_WIFI_OFF;
static volatile esp_err_t s_error;
static char s_ip[16];
static bool s_wifi_initialized;
static bool s_wifi_started;
static bool s_wifi_handler_registered;
static bool s_ip_handler_registered;
static bool s_host_initialized;
static bool s_host_running;
static bool s_gatt_initialized;
static bool s_btc_initialized;
static bool s_profile_initialized;
static bool s_ble_connected;
static bool s_wifi_connecting;
static bool s_wifi_got_ip;
static bool s_reconnect_after_disconnect;
static bool s_sntp_started;
static volatile bool s_time_synced;
static volatile bool s_time_sync_pending;
static SemaphoreHandle_t s_host_stopped;

typedef struct {
    uint32_t magic;
    uint32_t size;
    wifi_config_t config;
} tools_wifi_store_blob_t;

static void copy_ssid(char out[33])
{
    memcpy(out, s_sta_config.sta.ssid, 32);
    out[32] = '\0';
}

static esp_err_t save_credentials_to_tools_data(void)
{
    if (s_sta_config.sta.ssid[0] == '\0') return ESP_OK;

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open_from_partition(TOOLS_STORAGE_PARTITION,
                                            WIFI_STORE_NAMESPACE,
                                            NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    tools_wifi_store_blob_t blob = {
        .magic = WIFI_STORE_MAGIC,
        .size = sizeof(wifi_config_t),
        .config = s_sta_config,
    };
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

    tools_wifi_store_blob_t blob;
    size_t size = sizeof(blob);
    err = nvs_get_blob(handle, WIFI_STORE_KEY, &blob, &size);
    nvs_close(handle);
    if (err != ESP_OK || size != sizeof(blob) ||
        blob.magic != WIFI_STORE_MAGIC ||
        blob.size != sizeof(wifi_config_t) ||
        blob.config.sta.ssid[0] == '\0') {
        return false;
    }

    *config = blob.config;
    return true;
}

static void set_state_for_idle(void)
{
    if (s_wifi_got_ip) {
        s_state = s_time_synced ? TOOLS_WIFI_TIME_SYNCED : TOOLS_WIFI_CONNECTED;
    } else if (s_ble_connected) {
        s_state = TOOLS_WIFI_PHONE_CONNECTED;
    } else if (s_profile_initialized) {
        s_state = TOOLS_WIFI_BLE_READY;
    } else if (s_sta_config.sta.ssid[0] == '\0') {
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

static void send_wifi_report(esp_blufi_sta_conn_state_t state)
{
    wifi_mode_t mode = WIFI_MODE_STA;
    esp_wifi_get_mode(&mode);
    esp_blufi_extra_info_t info = { 0 };
    size_t ssid_len = strnlen((const char *)s_sta_config.sta.ssid,
                              sizeof(s_sta_config.sta.ssid));
    if (ssid_len > 0) {
        info.sta_ssid = s_sta_config.sta.ssid;
        info.sta_ssid_len = ssid_len;
    }
    esp_blufi_send_wifi_conn_report(mode, state, 0, &info);
}

static void send_wifi_list(void)
{
    uint16_t count = BLUFI_AP_LIST_COUNT;
    wifi_ap_record_t records[BLUFI_AP_LIST_COUNT] = { 0 };
    esp_blufi_ap_record_t list[BLUFI_AP_LIST_COUNT] = { 0 };
    esp_err_t err = esp_wifi_scan_get_ap_records(&count, records);
    if (err != ESP_OK) {
        esp_blufi_send_error_info(ESP_BLUFI_WIFI_SCAN_FAIL);
        return;
    }
    for (uint16_t i = 0; i < count; i++) {
        list[i].rssi = records[i].rssi;
        memcpy(list[i].ssid, records[i].ssid, sizeof(list[i].ssid));
    }
    if (s_ble_connected) esp_blufi_send_wifi_list(count, list);
}

static void request_wifi_connect(void)
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
    esp_err_t save_err = save_credentials_to_tools_data();
    if (save_err != ESP_OK) {
        ESP_LOGW(TAG, "保存 Wi-Fi 配置失败: %s", esp_err_to_name(save_err));
    }

    bool was_connected = s_wifi_got_ip;
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
        s_state = TOOLS_WIFI_FAILED;
    }
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == WIFI_EVENT_STA_START) {
        s_wifi_started = true;
        esp_wifi_get_config(WIFI_IF_STA, &s_sta_config);
        if (s_sta_config.sta.ssid[0] != '\0') {
            ESP_LOGI(TAG, "Wi-Fi credentials found, connecting");
            request_wifi_connect();
        } else {
            set_state_for_idle();
        }
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *event = data;
        ESP_LOGW(TAG, "Wi-Fi disconnected, reason=%u", event->reason);
        if (s_reconnect_after_disconnect) {
            s_reconnect_after_disconnect = false;
            esp_wifi_connect();
            return;
        }
        if (s_state == TOOLS_WIFI_CONNECTING && s_ble_connected) {
            send_wifi_report(ESP_BLUFI_STA_CONN_FAIL);
        }
        s_wifi_connecting = false;
        s_wifi_got_ip = false;
        s_ip[0] = '\0';
        set_state_for_idle();
    } else if (id == WIFI_EVENT_SCAN_DONE) {
        send_wifi_list();
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
    start_or_restart_sntp();
    if (s_ble_connected) send_wifi_report(ESP_BLUFI_STA_CONN_SUCCESS);
}

static void blufi_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE reset: %d", reason);
    s_error = ESP_FAIL;
    s_state = TOOLS_WIFI_FAILED;
}

static void blufi_sync(void)
{
    int rc = esp_blufi_profile_init();
    if (rc == 0) {
        s_profile_initialized = true;
    } else {
        s_error = ESP_FAIL;
        s_state = TOOLS_WIFI_FAILED;
    }
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    if (s_host_stopped) xSemaphoreGive(s_host_stopped);
    nimble_port_freertos_deinit();
}

static void blufi_event(esp_blufi_cb_event_t event, esp_blufi_cb_param_t *param)
{
    switch (event) {
    case ESP_BLUFI_EVENT_INIT_FINISH:
        esp_blufi_adv_start_with_name(TOOLS_WIFI_DEVICE_NAME);
        set_state_for_idle();
        break;
    case ESP_BLUFI_EVENT_BLE_CONNECT:
        s_ble_connected = true;
        esp_blufi_adv_stop();
        if (tools_wifi_security_init() != 0) {
            s_error = ESP_ERR_NO_MEM;
            s_state = TOOLS_WIFI_FAILED;
        } else if (!s_wifi_got_ip) {
            s_state = TOOLS_WIFI_PHONE_CONNECTED;
        }
        break;
    case ESP_BLUFI_EVENT_BLE_DISCONNECT:
        s_ble_connected = false;
        tools_wifi_security_deinit();
        esp_blufi_adv_start_with_name(TOOLS_WIFI_DEVICE_NAME);
        set_state_for_idle();
        break;
    case ESP_BLUFI_EVENT_SET_WIFI_OPMODE:
        esp_wifi_set_mode(WIFI_MODE_STA);
        break;
    case ESP_BLUFI_EVENT_RECV_STA_BSSID:
        memcpy(s_sta_config.sta.bssid, param->sta_bssid.bssid, 6);
        s_sta_config.sta.bssid_set = true;
        esp_wifi_set_config(WIFI_IF_STA, &s_sta_config);
        save_credentials_to_tools_data();
        break;
    case ESP_BLUFI_EVENT_RECV_STA_SSID:
        if (param->sta_ssid.ssid_len >= sizeof(s_sta_config.sta.ssid)) {
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
            break;
        }
        memset(s_sta_config.sta.ssid, 0, sizeof(s_sta_config.sta.ssid));
        memset(s_sta_config.sta.password, 0, sizeof(s_sta_config.sta.password));
        memset(s_sta_config.sta.bssid, 0, sizeof(s_sta_config.sta.bssid));
        s_sta_config.sta.bssid_set = false;
        s_sta_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
        memcpy(s_sta_config.sta.ssid, param->sta_ssid.ssid,
               param->sta_ssid.ssid_len);
        esp_wifi_set_config(WIFI_IF_STA, &s_sta_config);
        save_credentials_to_tools_data();
        break;
    case ESP_BLUFI_EVENT_RECV_STA_PASSWD:
        if (param->sta_passwd.passwd_len >= sizeof(s_sta_config.sta.password)) {
            esp_blufi_send_error_info(ESP_BLUFI_DATA_FORMAT_ERROR);
            break;
        }
        memset(s_sta_config.sta.password, 0, sizeof(s_sta_config.sta.password));
        memcpy(s_sta_config.sta.password, param->sta_passwd.passwd,
               param->sta_passwd.passwd_len);
        esp_wifi_set_config(WIFI_IF_STA, &s_sta_config);
        save_credentials_to_tools_data();
        break;
    case ESP_BLUFI_EVENT_REQ_CONNECT_TO_AP:
        request_wifi_connect();
        break;
    case ESP_BLUFI_EVENT_REQ_DISCONNECT_FROM_AP:
        esp_wifi_disconnect();
        break;
    case ESP_BLUFI_EVENT_GET_WIFI_STATUS:
        if (s_wifi_got_ip) {
            send_wifi_report(ESP_BLUFI_STA_CONN_SUCCESS);
        } else if (s_wifi_connecting) {
            send_wifi_report(ESP_BLUFI_STA_CONNECTING);
        } else {
            send_wifi_report(ESP_BLUFI_STA_CONN_FAIL);
        }
        break;
    case ESP_BLUFI_EVENT_GET_WIFI_LIST: {
        wifi_scan_config_t config = { 0 };
        if (esp_wifi_scan_start(&config, false) != ESP_OK) {
            esp_blufi_send_error_info(ESP_BLUFI_WIFI_SCAN_FAIL);
        }
        break;
    }
    case ESP_BLUFI_EVENT_RECV_SLAVE_DISCONNECT_BLE:
        esp_blufi_disconnect();
        break;
    case ESP_BLUFI_EVENT_DEAUTHENTICATE_STA:
        esp_wifi_disconnect();
        break;
    case ESP_BLUFI_EVENT_REPORT_ERROR:
        esp_blufi_send_error_info(param->report_error.state);
        break;
    default:
        break;
    }
}

static esp_blufi_callbacks_t s_callbacks = {
    .event_cb = blufi_event,
    .negotiate_data_handler = tools_wifi_negotiate,
    .encrypt_func = tools_wifi_encrypt,
    .decrypt_func = tools_wifi_decrypt,
    .checksum_func = tools_wifi_checksum,
};

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
    err = esp_wifi_set_storage(WIFI_STORAGE_FLASH);
    if (err == ESP_OK) err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) err = esp_wifi_start();
    if (err == ESP_OK) {
        s_wifi_started = true;
        esp_wifi_get_config(WIFI_IF_STA, &s_sta_config);
        if (s_sta_config.sta.ssid[0] == '\0' &&
            load_tools_data_credentials(&s_sta_config)) {
            ESP_LOGI(TAG, "Wi-Fi credentials restored from tools_data");
            esp_wifi_set_config(WIFI_IF_STA, &s_sta_config);
        }
        set_state_for_idle();
    }
    return err;
}

static esp_err_t blufi_start(void)
{
    if (s_host_initialized) return ESP_OK;

    esp_err_t err = esp_blufi_register_callbacks(&s_callbacks);
    if (err != ESP_OK) return err;
    err = nimble_port_init();
    if (err != ESP_OK) return err;
    s_host_initialized = true;
    s_host_stopped = xSemaphoreCreateBinary();
    if (!s_host_stopped) return ESP_ERR_NO_MEM;
    ble_hs_cfg.reset_cb = blufi_reset;
    ble_hs_cfg.sync_cb = blufi_sync;
    ble_hs_cfg.gatts_register_cb = esp_blufi_gatt_svr_register_cb;
    int rc = esp_blufi_gatt_svr_init();
    if (rc != 0) return ESP_FAIL;
    s_gatt_initialized = true;
    rc = ble_svc_gap_device_name_set(TOOLS_WIFI_DEVICE_NAME);
    if (rc != 0) return ESP_FAIL;
    esp_blufi_btc_init();
    s_btc_initialized = true;
    err = esp_nimble_enable(host_task);
    if (err == ESP_OK) s_host_running = true;
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
    if (err == ESP_OK) err = blufi_start();
    if (err != ESP_OK) {
        s_error = err;
        s_state = TOOLS_WIFI_FAILED;
    }
    return err;
}

void tools_wifi_stop_provisioning(void)
{
    s_ble_connected = false;
    tools_wifi_security_deinit();
    if (s_host_initialized) {
        if (s_profile_initialized) esp_blufi_adv_stop();
        if (s_gatt_initialized) {
            esp_blufi_gatt_svr_deinit();
            s_gatt_initialized = false;
        }
        bool host_stopped = !s_host_running;
        if (s_host_running) {
            int rc = nimble_port_stop();
            if (rc == 0) {
                xSemaphoreTake(s_host_stopped, portMAX_DELAY);
                host_stopped = true;
            } else {
                ESP_LOGE(TAG, "nimble_port_stop failed: %d", rc);
            }
        }
        if (host_stopped) nimble_port_deinit();
        s_host_running = false;
        if (s_profile_initialized) {
            esp_blufi_profile_deinit();
            s_profile_initialized = false;
        }
        if (s_btc_initialized) {
            esp_blufi_btc_deinit();
            s_btc_initialized = false;
        }
        s_host_initialized = false;
    }
    if (s_host_stopped) {
        vSemaphoreDelete(s_host_stopped);
        s_host_stopped = NULL;
    }
    set_state_for_idle();
}

esp_err_t tools_wifi_stop_all(void)
{
    tools_wifi_stop_provisioning();

    if (s_sntp_started) {
        esp_sntp_stop();
        s_sntp_started = false;
    }
    if (s_wifi_started) {
        (void)esp_wifi_scan_stop();
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
    s_ble_connected = false;
    s_reconnect_after_disconnect = false;
    s_time_synced = false;
    s_time_sync_pending = false;
    s_ip[0] = '\0';
    s_state = TOOLS_WIFI_OFF;
    s_error = ESP_OK;
    return ESP_OK;
}

esp_err_t tools_wifi_reconnect(void)
{
    esp_err_t err = tools_wifi_init();
    if (err == ESP_OK) request_wifi_connect();
    return err;
}

esp_err_t tools_wifi_clear_credentials(void)
{
    esp_err_t err = tools_wifi_init();
    wifi_config_t empty = { 0 };
    s_reconnect_after_disconnect = false;
    if (s_wifi_started) esp_wifi_disconnect();
    if (err == ESP_OK) err = esp_wifi_set_config(WIFI_IF_STA, &empty);
    esp_err_t clear_err = clear_tools_data_credentials();
    if (clear_err != ESP_OK) {
        ESP_LOGW(TAG, "清除小工具 Wi-Fi 配置失败: %s", esp_err_to_name(clear_err));
    }
    memset(&s_sta_config, 0, sizeof(s_sta_config));
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
        .provisioning = s_host_initialized,
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
