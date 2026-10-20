#include "page_turner.h"

#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "esp_bt.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_hidd.h"
#include "esp_log.h"
#include "esp_nimble_hci.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "lvgl.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "services/gap/ble_svc_gap.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define DEVICE_NAME "PassportTurner"

static const char *TAG = "page_turner";
static const char *NVS_NAMESPACE = "turner";
static const char *NVS_MODE_KEY = "mode";
void ble_store_config_init(void);

#define INPUT_QUEUE_DEPTH 8
#define UI_QUEUE_DEPTH 6
#define BACKLIGHT_ACTIVE_PERCENT 12
#define BACKLIGHT_DIM_PERCENT 3
#define SCREEN_DIM_IDLE_MS (10 * 1000)
#define SCREEN_OFF_IDLE_MS (25 * 1000)
#define UI_IDLE_POLL_MS 1000
#define UI_BATTERY_REFRESH_MS (60 * 1000)
#define HID_BATTERY_REFRESH_MS (5 * 60 * 1000)
#define HID_KEY_HOLD_MS 80
#define UI_BG 0x0B1020
#define UI_SURFACE 0x151C2F
#define UI_SURFACE_2 0x1F2B46
#define UI_TEXT 0xF4F7FB
#define UI_MUTED 0x93A4BD
#define UI_LINE 0x31415F
#define UI_BLUE 0x4CC9F0
#define UI_GREEN 0x7CFF9B
#define UI_AMBER 0xFFD166
#define UI_RED 0xFF6B6B

typedef enum {
    TURNER_MODE_PAGE = 0,
    TURNER_MODE_ARROW,
    TURNER_MODE_VOLUME,
    TURNER_MODE_COUNT,
} turner_mode_t;

typedef enum {
    UI_EVENT_RENDER = 1,
    UI_EVENT_CONNECTED,
    UI_EVENT_DISCONNECTED,
    UI_EVENT_STARTED,
    UI_EVENT_ERROR,
} ui_event_t;

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;

static QueueHandle_t s_input_queue;
static QueueHandle_t s_ui_queue;
static esp_hidd_dev_t *s_hid;
static bool s_connected;
static bool s_ble_started;
static bool s_battery_ok;
static bool s_screen_awake = true;
static turner_mode_t s_mode = TURNER_MODE_VOLUME;
static lv_obj_t *s_screen;
static uint8_t s_addr_type;
static TickType_t s_last_activity_tick;
static TickType_t s_last_battery_ui_tick;
static TickType_t s_last_battery_hid_tick;

static void request_render(void);

static const uint8_t s_report_map[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x06,        // Usage (Keyboard)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
    0x19, 0xE0,        //   Usage Minimum (Left Control)
    0x29, 0xE7,        //   Usage Maximum (Right GUI)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x08,        //   Report Count (8)
    0x81, 0x02,        //   Input (Data, Variable, Absolute)
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x08,        //   Report Size (8)
    0x81, 0x03,        //   Input (Constant)
    0x95, 0x05,        //   Report Count (5)
    0x75, 0x01,        //   Report Size (1)
    0x05, 0x08,        //   Usage Page (LEDs)
    0x19, 0x01,        //   Usage Minimum (Num Lock)
    0x29, 0x05,        //   Usage Maximum (Kana)
    0x91, 0x02,        //   Output (Data, Variable, Absolute)
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x03,        //   Report Size (3)
    0x91, 0x03,        //   Output (Constant)
    0x95, 0x06,        //   Report Count (6)
    0x75, 0x08,        //   Report Size (8)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x65,        //   Logical Maximum (101)
    0x05, 0x07,        //   Usage Page (Keyboard/Keypad)
    0x19, 0x00,        //   Usage Minimum (Reserved)
    0x29, 0x65,        //   Usage Maximum (Keyboard Application)
    0x81, 0x00,        //   Input (Data, Array)
    0xC0,              // End Collection
    0x05, 0x0C,        // Usage Page (Consumer)
    0x09, 0x01,        // Usage (Consumer Control)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x02,        //   Report ID (2)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x03,  //   Logical Maximum (1023)
    0x19, 0x00,        //   Usage Minimum (0)
    0x2A, 0xFF, 0x03,  //   Usage Maximum (1023)
    0x75, 0x10,        //   Report Size (16)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x00,        //   Input (Data, Array)
    0xC0,              // End Collection
};

static esp_hid_raw_report_map_t s_report_maps[] = {
    {
        .data = s_report_map,
        .len = sizeof(s_report_map),
    },
};

static const esp_hid_device_config_t s_hid_config = {
    .vendor_id = 0x16C0,
    .product_id = 0x27DB,
    .version = 0x0100,
    .device_name = DEVICE_NAME,
    .manufacturer_name = "AI Passport",
    .serial_number = "poke7-turner",
    .report_maps = s_report_maps,
    .report_maps_len = 1,
};

static const char *mode_name(turner_mode_t mode)
{
    switch (mode) {
    case TURNER_MODE_ARROW:
        return "LEFT / RIGHT";
    case TURNER_MODE_VOLUME:
        return "VOL UP / DOWN";
    case TURNER_MODE_PAGE:
    default:
        return "PAGE UP / DOWN";
    }
}

static const char *up_action(turner_mode_t mode)
{
    return mode == TURNER_MODE_ARROW ? "LEFT" :
           mode == TURNER_MODE_VOLUME ? "VOL UP" : "PAGE UP";
}

static const char *down_action(turner_mode_t mode)
{
    return mode == TURNER_MODE_ARROW ? "RIGHT" :
           mode == TURNER_MODE_VOLUME ? "VOL DOWN" : "PAGE DOWN";
}

static void save_mode(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;
    (void)nvs_set_u8(handle, NVS_MODE_KEY, (uint8_t)s_mode);
    (void)nvs_commit(handle);
    nvs_close(handle);
}

static void load_mode(void)
{
    nvs_handle_t handle;
    uint8_t mode = TURNER_MODE_VOLUME;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return;
    if (nvs_get_u8(handle, NVS_MODE_KEY, &mode) == ESP_OK && mode < TURNER_MODE_COUNT) {
        s_mode = (turner_mode_t)mode;
    }
    nvs_close(handle);
}

static lv_obj_t *panel(lv_obj_t *parent, int x, int y, int w, int h,
                       uint32_t color, uint32_t border)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, 8, 0);
    lv_obj_set_style_border_width(obj, border ? 1 : 0, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(border), 0);
    return obj;
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, int x, int y,
                       int w, uint32_t color)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_width(obj, w);
    lv_obj_set_style_text_font(obj, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_MODE_CLIP);
    lv_label_set_text(obj, text);
    return obj;
}

static void render_battery(lv_obj_t *root)
{
    if (!s_battery_ok) return;
    int soc = bsp_battery_soc();
    if (soc < 0) return;
    if (soc > 100) soc = 100;
    uint32_t color = soc < 20 ? UI_RED : UI_GREEN;
    panel(root, 186, 13, 36, 16, 0x0A1324, color);
    panel(root, 221, 18, 3, 6, color, 0);
    int fill = 4 + soc * 24 / 100;
    panel(root, 190, 18, fill, 6, color, 0);
}

static void render_ui(void)
{
    if (!s_screen) {
        s_screen = lv_obj_create(NULL);
        lv_screen_load(s_screen);
    }
    lv_obj_remove_style_all(s_screen);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clean(s_screen);

    label(s_screen, "PAGE TURNER", 14, 14, 150, UI_BLUE);
    render_battery(s_screen);

    panel(s_screen, 14, 48, 212, 86, UI_SURFACE, s_connected ? UI_GREEN : UI_LINE);
    label(s_screen, s_connected ? "CONNECTED" :
          (s_ble_started ? "PAIRING MODE" : "STARTING BLE"),
          28, 62, 184, s_connected ? UI_GREEN : UI_AMBER);
    label(s_screen, "Bluetooth name:", 28, 88, 184, UI_MUTED);
    label(s_screen, DEVICE_NAME, 28, 108, 184, UI_TEXT);

    panel(s_screen, 14, 150, 212, 74, UI_SURFACE_2, UI_LINE);
    label(s_screen, "MODE", 28, 164, 56, UI_MUTED);
    lv_obj_t *mode = label(s_screen, mode_name(s_mode), 84, 164, 128, UI_TEXT);
    lv_obj_set_style_text_align(mode, LV_TEXT_ALIGN_RIGHT, 0);
    label(s_screen, "OK: change mode", 28, 194, 184, UI_BLUE);

    panel(s_screen, 14, 240, 96, 54, UI_SURFACE, UI_BLUE);
    label(s_screen, "UP", 28, 250, 34, UI_MUTED);
    label(s_screen, up_action(s_mode), 28, 270, 68, UI_TEXT);
    panel(s_screen, 130, 240, 96, 54, UI_SURFACE, UI_BLUE);
    label(s_screen, "DOWN", 144, 250, 50, UI_MUTED);
    label(s_screen, down_action(s_mode), 144, 270, 68, UI_TEXT);
    label(s_screen, "Pair from phone/Poke7 Bluetooth settings", 14, 304, 212, UI_MUTED);
}

static void request_render(void)
{
    ui_event_t event = UI_EVENT_RENDER;
    if (s_ui_queue) {
        (void)xQueueSend(s_ui_queue, &event, 0);
    }
}

static void render_if_possible(void)
{
    if (!s_screen_awake) return;
    if (!bsp_lvgl_lock(500)) return;
    render_ui();
    bsp_lvgl_unlock();
}

static uint32_t ticks_elapsed_ms(TickType_t start, TickType_t now)
{
    return (uint32_t)((now - start) * portTICK_PERIOD_MS);
}

static void set_screen_backlight(uint8_t percent)
{
    bsp_display_backlight(percent);
    s_screen_awake = percent > 0;
}

static void note_activity(void)
{
    s_last_activity_tick = xTaskGetTickCount();
    if (!s_screen_awake) {
        set_screen_backlight(BACKLIGHT_ACTIVE_PERCENT);
        request_render();
    }
}

static void apply_power_policy(void)
{
    if (!s_screen_awake) return;
    TickType_t now = xTaskGetTickCount();
    uint32_t idle_ms = ticks_elapsed_ms(s_last_activity_tick, now);
    if (idle_ms >= SCREEN_OFF_IDLE_MS) {
        set_screen_backlight(0);
    } else if (idle_ms >= SCREEN_DIM_IDLE_MS) {
        bsp_display_backlight(BACKLIGHT_DIM_PERCENT);
    }
}

static esp_err_t start_advertising(void)
{
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    fields.appearance = 0x03C1;
    fields.appearance_is_present = 1;
    fields.name = (const uint8_t *)DEVICE_NAME;
    fields.name_len = strlen(DEVICE_NAME);
    fields.name_is_complete = 1;
    ble_uuid16_t hid_uuid = BLE_UUID16_INIT(0x1812);
    fields.uuids16 = &hid_uuid;
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "set adv fields failed: %d", rc);
        return ESP_FAIL;
    }

    struct ble_gap_adv_params params;
    memset(&params, 0, sizeof(params));
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    params.itvl_min = BLE_GAP_ADV_ITVL_MS(250);
    params.itvl_max = BLE_GAP_ADV_ITVL_MS(400);

    rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &params, NULL, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "start adv failed: %d", rc);
        return ESP_FAIL;
    }
    s_ble_started = true;
    request_render();
    return ESP_OK;
}

static void on_hid_event(void *handler_args, esp_event_base_t base,
                         int32_t id, void *event_data)
{
    (void)handler_args;
    (void)base;
    esp_hidd_event_t event = (esp_hidd_event_t)id;
    esp_hidd_event_data_t *param = (esp_hidd_event_data_t *)event_data;
    ui_event_t ui_event = UI_EVENT_RENDER;

    switch (event) {
    case ESP_HIDD_START_EVENT:
        ESP_LOGI(TAG, "HID started");
        (void)start_advertising();
        ui_event = UI_EVENT_STARTED;
        break;
    case ESP_HIDD_CONNECT_EVENT:
        ESP_LOGI(TAG, "HID connected");
        s_connected = true;
        s_last_battery_hid_tick = 0;
        ui_event = UI_EVENT_CONNECTED;
        break;
    case ESP_HIDD_DISCONNECT_EVENT:
        ESP_LOGI(TAG, "HID disconnected: reason=%d", param ? param->disconnect.reason : -1);
        s_connected = false;
        s_ble_started = false;
        (void)start_advertising();
        ui_event = UI_EVENT_DISCONNECTED;
        break;
    case ESP_HIDD_STOP_EVENT:
        s_connected = false;
        s_ble_started = false;
        break;
    default:
        break;
    }
    if (s_ui_queue) (void)xQueueSend(s_ui_queue, &ui_event, 0);
}

static void page_turner_ble_host_task(void *param)
{
    (void)param;
    ESP_LOGI(TAG, "BLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static esp_err_t init_ble_hid(void)
{
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "classic memory release failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_bt_controller_init(&bt_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = esp_nimble_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    err = esp_hidd_dev_init(&s_hid_config, ESP_HID_TRANSPORT_BLE, on_hid_event, &s_hid);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HID init failed: %s", esp_err_to_name(err));
        return err;
    }

    ble_svc_gap_device_name_set(DEVICE_NAME);
    ble_svc_gap_device_appearance_set(0x03C1);
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_store_config_init();

    err = esp_nimble_enable(page_turner_ble_host_task);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE enable failed: %s", esp_err_to_name(err));
    }
    return err;
}

static esp_err_t send_keyboard_usage(uint8_t usage)
{
    if (!s_hid || (!s_connected && !esp_hidd_dev_connected(s_hid))) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t report[8] = {0};
    report[2] = usage;
    esp_err_t err = esp_hidd_dev_input_set(s_hid, 0, 1, report, sizeof(report));
    vTaskDelay(pdMS_TO_TICKS(HID_KEY_HOLD_MS));
    memset(report, 0, sizeof(report));
    esp_err_t release_err = esp_hidd_dev_input_set(s_hid, 0, 1, report, sizeof(report));
    return err == ESP_OK ? release_err : err;
}

static esp_err_t send_consumer_usage(uint16_t usage)
{
    if (!s_hid || (!s_connected && !esp_hidd_dev_connected(s_hid))) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t report[2] = {
        (uint8_t)(usage & 0xffu),
        (uint8_t)(usage >> 8),
    };
    esp_err_t err = esp_hidd_dev_input_set(s_hid, 0, 2, report, sizeof(report));
    vTaskDelay(pdMS_TO_TICKS(HID_KEY_HOLD_MS));
    report[0] = 0;
    report[1] = 0;
    esp_err_t release_err = esp_hidd_dev_input_set(s_hid, 0, 2, report, sizeof(report));
    return err == ESP_OK ? release_err : err;
}

static void send_turn(bool next)
{
    esp_err_t err;
    switch (s_mode) {
    case TURNER_MODE_ARROW:
        err = send_keyboard_usage(next ? 0x4F : 0x50); // Right / Left
        break;
    case TURNER_MODE_VOLUME:
        err = send_consumer_usage(next ? 0x00EA : 0x00E9); // Vol- / Vol+
        break;
    case TURNER_MODE_PAGE:
    default:
        err = send_keyboard_usage(next ? 0x4E : 0x4B); // PageDown / PageUp
        break;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "send key failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "sent %s", next ? down_action(s_mode) : up_action(s_mode));
    }
    request_render();
}

static void process_input(const input_event_t *input)
{
    if (input->event != BSP_BTN_PRESS && input->event != BSP_BTN_CLICK) return;
    ESP_LOGI(TAG, "button btn=%d event=%d mv=%d connected=%d mode=%s",
             input->btn, input->event, bsp_button_read_mv(),
             s_hid ? esp_hidd_dev_connected(s_hid) : 0, mode_name(s_mode));
    note_activity();
    if (input->event == BSP_BTN_PRESS && input->btn == BSP_BTN_UP) {
        send_turn(false);
    } else if (input->event == BSP_BTN_PRESS && input->btn == BSP_BTN_DOWN) {
        send_turn(true);
    } else if (input->event == BSP_BTN_CLICK && input->btn == BSP_BTN_OK) {
        s_mode = (turner_mode_t)(((int)s_mode + 1) % TURNER_MODE_COUNT);
        save_mode();
        request_render();
    }
}

static void input_task(void *arg)
{
    (void)arg;
    input_event_t input;
    for (;;) {
        if (xQueueReceive(s_input_queue, &input, portMAX_DELAY) == pdTRUE) {
            process_input(&input);
        }
    }
}

static void publish_battery_if_due(bool force)
{
    if (!s_hid || !s_battery_ok) return;
    TickType_t now = xTaskGetTickCount();
    if (!force &&
        ticks_elapsed_ms(s_last_battery_hid_tick, now) < HID_BATTERY_REFRESH_MS) {
        return;
    }
    int soc = bsp_battery_soc();
    if (soc < 0) return;
    if (soc > 100) soc = 100;
    if (esp_hidd_dev_battery_set(s_hid, (uint8_t)soc) == ESP_OK) {
        s_last_battery_hid_tick = now;
    }
}

static void ui_task(void *arg)
{
    (void)arg;
    ui_event_t event;
    render_if_possible();
    for (;;) {
        bool got_event = xQueueReceive(s_ui_queue, &event,
                                       pdMS_TO_TICKS(UI_IDLE_POLL_MS)) == pdTRUE;
        if (got_event) {
            if (event == UI_EVENT_CONNECTED || event == UI_EVENT_STARTED) {
                publish_battery_if_due(true);
            }
            render_if_possible();
        }

        TickType_t now = xTaskGetTickCount();
        if (s_screen_awake &&
            ticks_elapsed_ms(s_last_battery_ui_tick, now) >= UI_BATTERY_REFRESH_MS) {
            s_last_battery_ui_tick = now;
            render_if_possible();
        }
        publish_battery_if_due(false);
        apply_power_policy();
    }
}

static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    if (!s_input_queue) return;
    const input_event_t input = {
        .btn = btn,
        .event = ev,
    };
    (void)xQueueSend(s_input_queue, &input, 0);
}

void page_turner_app_main(void)
{
    ESP_LOGI(TAG, "AI Passport page turner start");
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "default NVS needs init recovery: %s", esp_err_to_name(nvs_err));
        nvs_err = ESP_OK;
    }
    if (nvs_err == ESP_OK) load_mode();

    bsp_i2c_init();
    bsp_i2c_scan();
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "display init failed");
        return;
    }
    s_last_activity_tick = xTaskGetTickCount();
    s_last_battery_ui_tick = s_last_activity_tick;
    s_last_battery_hid_tick = 0;
    set_screen_backlight(BACKLIGHT_ACTIVE_PERCENT);
    s_battery_ok = bsp_battery_init() == ESP_OK;

    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(input_event_t));
    s_ui_queue = xQueueCreate(UI_QUEUE_DEPTH, sizeof(ui_event_t));
    if (!s_input_queue || !s_ui_queue) {
        ESP_LOGE(TAG, "queue init failed");
        return;
    }
    (void)xTaskCreate(input_task, "turner_input", 3072, NULL, 5, NULL);
    (void)xTaskCreate(ui_task, "turner_ui", 4096, NULL, 4, NULL);

    esp_err_t button_err = bsp_button_init(on_key, NULL);
    if (button_err != ESP_OK) {
        ESP_LOGE(TAG, "button init failed: %s", esp_err_to_name(button_err));
    }

    esp_err_t ble_err = init_ble_hid();
    if (ble_err != ESP_OK) {
        ESP_LOGE(TAG, "BLE HID init failed: %s", esp_err_to_name(ble_err));
        ui_event_t event = UI_EVENT_ERROR;
        (void)xQueueSend(s_ui_queue, &event, 0);
    }
}
