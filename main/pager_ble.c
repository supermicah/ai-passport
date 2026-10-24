// BLE HID 外设：触屏点击和媒体键报告。
// GAP 与 HID 初始化流程参考 ESP-IDF examples/bluetooth/esp_hid_device（Unlicense OR CC0-1.0）。
#include "pager_ble.h"

#include <string.h>
#include "esp_bt.h"
#include "esp_hidd.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"

static const char *TAG = "pager_ble";

#define APPEARANCE_HID      0x03C0
#define HID_SERVICE_UUID    0x1812

static const uint8_t report_map[] = {
    // Touch screen, report ID 1: one finger, absolute X/Y in 0..32767.
    0x05, 0x0D, 0x09, 0x04, 0xA1, 0x01,
    0x85, PAGER_RPT_TOUCH,
    0x09, 0x22, 0xA1, 0x02,
    0x09, 0x42, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x01, 0x81, 0x02,
    0x75, 0x07, 0x95, 0x01, 0x81, 0x03,
    0x09, 0x51, 0x75, 0x08, 0x95, 0x01,
    0x15, 0x00, 0x25, 0x7F, 0x81, 0x02,
    0x05, 0x01, 0x26, 0xFF, 0x7F,
    0x75, 0x10, 0x95, 0x01,
    0x09, 0x30, 0x81, 0x02,
    0x09, 0x31, 0x81, 0x02,
    0xC0, 0xC0,

    // Consumer control, report ID 2: one 16-bit usage.
    0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01,
    0x85, PAGER_RPT_CONSUMER,
    0x15, 0x00, 0x26, 0xFF, 0x03, 0x19, 0x00, 0x2A, 0xFF, 0x03,
    0x75, 0x10, 0x95, 0x01, 0x81, 0x00,
    0xC0,
};

static esp_hid_raw_report_map_t report_maps[] = {{.data = report_map, .len = sizeof(report_map)}};

static esp_hid_device_config_t hid_config = {
    .vendor_id = 0x16C0,
    .product_id = 0x05DF,
    .version = 0x0100,
    .manufacturer_name = "Passport",
    .serial_number = "0001",
    .report_maps = report_maps,
    .report_maps_len = 1,
};

static esp_hidd_dev_t *s_dev;
static struct ble_hs_adv_fields s_adv_fields;
static ble_uuid16_t s_hid_uuid = BLE_UUID16_INIT(HID_SERVICE_UUID);
static pager_ble_state_cb_t s_state_cb;
static volatile pager_ble_state_t s_state = PAGER_BLE_ADVERTISING;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static char s_peer_name[PAGER_PEER_NAME_MAX];  // 只在 NimBLE 主机任务里读写

static void set_state(pager_ble_state_t state)
{
    s_state = state;
    if (s_state_cb) s_state_cb(state, s_peer_name);
}

static int gap_event(struct ble_gap_event *event, void *arg);

// 截到完整的 UTF-8 字符为止，避免把一个汉字从中间切开。
static void copy_utf8(char *dst, size_t cap, const uint8_t *src, size_t len)
{
    if (len >= cap) {
        len = cap - 1;
        while (len > 0 && (src[len] & 0xC0) == 0x80) --len;
    }
    memcpy(dst, src, len);
    dst[len] = 0;
}

static int on_peer_name(uint16_t conn, const struct ble_gatt_error *error,
                        struct ble_gatt_attr *attr, void *arg)
{
    (void)arg;
    if (error->status == 0 && attr && attr->om) {
        uint8_t buf[PAGER_PEER_NAME_MAX];
        uint16_t len = 0;
        if (ble_hs_mbuf_to_flat(attr->om, buf, sizeof(buf), &len) == 0 && conn == s_conn) {
            copy_utf8(s_peer_name, sizeof(s_peer_name), buf, len);
            ESP_LOGI(TAG, "Peer name: %s", s_peer_name);
            set_state(s_state);  // 带上名字重新通知界面
        }
    } else if (error->status != BLE_HS_EDONE) {
        ESP_LOGW(TAG, "Peer name not readable (status=%d)", error->status);
    }
    return 0;
}

// 读对方 GAP 服务里的设备名（0x2A00）。Mac、Windows、安卓都会提供。
static void read_peer_name(uint16_t conn)
{
    int rc = ble_gattc_read_by_uuid(conn, 1, 0xFFFF, BLE_UUID16_DECLARE(BLE_SVC_GAP_CHR_UUID16_DEVICE_NAME),
                                    on_peer_name, NULL);
    if (rc != 0) ESP_LOGW(TAG, "read peer name rc=%d", rc);
}

static void adv_start(void)
{
    if (ble_gap_adv_active()) return;
    int rc = ble_gap_adv_set_fields(&s_adv_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv fields rc=%d", rc);
        return;
    }
    struct ble_gap_adv_params params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min = BLE_GAP_ADV_ITVL_MS(30),
        .itvl_max = BLE_GAP_ADV_ITVL_MS(50),
    };
    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv start rc=%d", rc);
        return;
    }
    ESP_LOGI(TAG, "Advertising as %s", hid_config.device_name);
    set_state(PAGER_BLE_ADVERTISING);
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    struct ble_gap_conn_desc desc;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "connect failed status=%d", event->connect.status);
            adv_start();
            return 0;
        }
        s_conn = event->connect.conn_handle;
        s_peer_name[0] = 0;
        ESP_LOGI(TAG, "Connected; requesting encryption");
        set_state(PAGER_BLE_CONNECTED);
        // HID 主机要求加密链路；已配对的设备会直接用保存的密钥。
        ble_gap_security_initiate(s_conn);
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Disconnected reason=%d", event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_peer_name[0] = 0;
        adv_start();
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (s_conn == BLE_HS_CONN_HANDLE_NONE) adv_start();
        return 0;
    case BLE_GAP_EVENT_ENC_CHANGE:
        if (event->enc_change.status == 0) {
            ESP_LOGI(TAG, "Encrypted; ready to page");
            set_state(PAGER_BLE_READY);
            // 有的主机只在加密后才让读设备名，所以等加密完成再读。
            read_peer_name(event->enc_change.conn_handle);
        } else {
            ESP_LOGW(TAG, "Encryption failed status=%d", event->enc_change.status);
        }
        return 0;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        // 对方删掉了配对又重新配：丢掉旧密钥，接受新的。
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0)
            ble_store_util_delete_peer(&desc.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        struct ble_sm_io io = {.action = event->passkey.params.action};
        if (io.action == BLE_SM_IOACT_NUMCMP) io.numcmp_accept = 1;
        ble_sm_inject_io(event->passkey.conn_handle, &io);
        return 0;
    }
    default:
        return 0;
    }
}

static void hidd_event(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)handler_args; (void)base; (void)event_data;
    switch ((esp_hidd_event_t)id) {
    case ESP_HIDD_START_EVENT:
        adv_start();
        break;
    case ESP_HIDD_DISCONNECT_EVENT:
        adv_start();
        break;
    default:
        break;
    }
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_store_config_init(void);

// PassportSim 绑定蓝牙模拟时要求镜像里有这两个函数，否则拒绝模拟。翻页器从不关蓝牙，
// 链接器会把它们裁掉，这里留个引用让它们保留下来（只多占几百字节）。
static void *volatile s_keep_for_sim[] = {(void *)esp_bt_controller_disable,
                                          (void *)esp_bt_controller_deinit};

esp_err_t pager_ble_start(const char *name, pager_ble_state_cb_t state_cb)
{
    (void)s_keep_for_sim[0];
    s_state_cb = state_cb;
    hid_config.device_name = name;

    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_bt_controller_init(&cfg), TAG, "controller init");
    ESP_RETURN_ON_ERROR(esp_bt_controller_enable(ESP_BT_MODE_BLE), TAG, "controller enable");
    ESP_RETURN_ON_ERROR(esp_nimble_init(), TAG, "nimble init");

    memset(&s_adv_fields, 0, sizeof(s_adv_fields));
    s_adv_fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    s_adv_fields.appearance = APPEARANCE_HID;
    s_adv_fields.appearance_is_present = 1;
    s_adv_fields.name = (uint8_t *)name;
    s_adv_fields.name_len = strlen(name);
    s_adv_fields.name_is_complete = 1;
    s_adv_fields.uuids16 = &s_hid_uuid;
    s_adv_fields.num_uuids16 = 1;
    s_adv_fields.uuids16_is_complete = 1;

    // 无屏幕输入的"直接配对"，配对信息保存在 NVS，下次自动回连。
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ID | BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ID | BLE_SM_PAIR_KEY_DIST_ENC;

    ESP_RETURN_ON_ERROR(esp_hidd_dev_init(&hid_config, ESP_HID_TRANSPORT_BLE, hidd_event, &s_dev),
                        TAG, "hidd init");
    // 读到真实电量前先报 100%，免得电脑上一连上就提示电量低。
    esp_hidd_dev_battery_set(s_dev, 100);
    ble_svc_gap_device_name_set(name);
    ble_svc_gap_device_appearance_set(APPEARANCE_HID);

    ble_store_config_init();
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

static void send_consumer(pager_key_t key, bool down)
{
    uint8_t report[2] = {0};
    if (down) {
        report[0] = key.code & 0xFF;
        report[1] = key.code >> 8;
    }
    esp_hidd_dev_input_set(s_dev, 0, PAGER_RPT_CONSUMER, report, sizeof(report));
}

static void send_touch(bool down, uint16_t x, uint16_t y)
{
    uint8_t report[6] = {
        down ? 1 : 0,
        1,
        x & 0xFF,
        x >> 8,
        y & 0xFF,
        y >> 8,
    };
    esp_hidd_dev_input_set(s_dev, 0, PAGER_RPT_TOUCH, report, sizeof(report));
}

static void swipe_page(bool next_page)
{
    const int32_t left_x = 5898;    // 18%
    const int32_t right_x = 26869;  // 82%
    const uint16_t y = 18022;       // 55%
    const int32_t start_x = next_page ? right_x : left_x;
    const int32_t end_x = next_page ? left_x : right_x;

    for (int step = 0; step <= 5; ++step) {
        const int32_t x = start_x + ((end_x - start_x) * step) / 5;
        send_touch(true, (uint16_t)x, y);
        vTaskDelay(pdMS_TO_TICKS(step == 0 ? 35 : 25));
    }
    send_touch(false, (uint16_t)end_x, y);
}

bool pager_ble_tap(pager_key_t key)
{
    // 真实主机（Mac、安卓）都会先加密再订阅；没加密时也照发，方便模拟器里不配对的测试主机收键。
    if (s_state == PAGER_BLE_ADVERTISING) return false;
    if (key.report_id == PAGER_RPT_TOUCH) {
        swipe_page(key.code == PAGER_TOUCH_NEXT);
        return true;
    }
    send_consumer(key, true);
    vTaskDelay(pdMS_TO_TICKS(30));
    send_consumer(key, false);
    return true;
}

void pager_ble_set_battery(int percent)
{
    if (s_dev && percent >= 0 && percent <= 100) esp_hidd_dev_battery_set(s_dev, (uint8_t)percent);
}

void pager_ble_forget_all(void)
{
    ESP_LOGI(TAG, "Forgetting all bonds");
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    ble_store_clear();
}
