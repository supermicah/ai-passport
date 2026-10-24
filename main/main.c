// Passport Pager：把 AI Passport 变成蓝牙翻页器。
// UP = 上一页，DOWN = 下一页，功能键单击切换翻页方式，双击进入"忘记配对"确认。
#include <assert.h>
#include <inttypes.h>
#include <string.h>
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "lvgl.h"
#include "misc/lv_text_private.h"  // lv_text_encoded_next：逐个解码 UTF-8 码点
#include "nvs.h"
#include "nvs_flash.h"
#include "pager_ble.h"
#include "pager_keys.h"

static const char *TAG = "passport_pager";
#define DEVICE_NAME "Passport Pager"
#define FORGET_CONFIRM_MS 5000
#define BATTERY_POLL_MS 30000

typedef enum { EV_PREV, EV_NEXT, EV_MODE, EV_FORGET_ASK, EV_BLE } ev_kind_t;
typedef struct {
    ev_kind_t kind;
    pager_ble_state_t ble;
    char peer[PAGER_PEER_NAME_MAX];
} ev_t;

static QueueHandle_t s_events;
static lv_obj_t *s_status, *s_mode, *s_hint, *s_last, *s_footer, *s_bt_icon, *s_batt;
static int s_battery = -1;  // 电量百分比，-1 表示读不到
static char s_peer[PAGER_PEER_NAME_MAX];  // 已连接设备的名字，空串表示还不知道
static pager_mode_t s_pager_mode = PAGER_MODE_CLICK;
static pager_ble_state_t s_ble = PAGER_BLE_ADVERTISING;
static TickType_t s_forget_deadline;  // 非 0 表示正在等确认

static void on_button(bsp_btn_t button, bsp_btn_ev_t event, void *user)
{
    (void)user;
    ev_t ev = {0};
    if (button == BSP_BTN_UP && event == BSP_BTN_PRESS) ev.kind = EV_PREV;
    else if (button == BSP_BTN_DOWN && event == BSP_BTN_PRESS) ev.kind = EV_NEXT;
    // 单击要等驱动排除双击后才发出，所以单击和双击不会同时触发。
    else if (button == BSP_BTN_OK && event == BSP_BTN_CLICK) ev.kind = EV_MODE;
    else if (button == BSP_BTN_OK && event == BSP_BTN_DOUBLE) ev.kind = EV_FORGET_ASK;
    else return;
    (void)xQueueSend(s_events, &ev, 0);
}

static void load_mode(void)
{
    nvs_handle_t nvs;
    if (nvs_open("pager", NVS_READONLY, &nvs) != ESP_OK) return;
    uint8_t mode;
    if (nvs_get_u8(nvs, "mode", &mode) == ESP_OK && mode < PAGER_MODE_COUNT) s_pager_mode = mode;
    nvs_close(nvs);
}

static void save_mode(void)
{
    nvs_handle_t nvs;
    if (nvs_open("pager", NVS_READWRITE, &nvs) != ESP_OK) return;
    if (nvs_set_u8(nvs, "mode", s_pager_mode) == ESP_OK) nvs_commit(nvs);
    nvs_close(nvs);
}

static void on_ble_state(pager_ble_state_t state, const char *peer_name)
{
    ev_t ev = {.kind = EV_BLE, .ble = state};
    strlcpy(ev.peer, peer_name, sizeof(ev.peer));
    (void)xQueueSend(s_events, &ev, 0);
}

// 中文用 tools/gen_fonts.sh 从思源黑体生成的子集：16px 正文，24px 标题和模式。
LV_FONT_DECLARE(pager_font_16);
LV_FONT_DECLARE(pager_font_24);
static lv_font_t s_font, s_font_big;

static const char *const STATUS_TEXT[] = {"等待配对", "正在连接…", "已连接"};
static const uint32_t STATUS_COLOR[] = {0xF5B942, 0x7FB8FF, 0x5BD68A};
#define TXT_TITLE      "蓝牙翻页器"
#define TXT_FOOTER     "上键 上一页　下键 下一页\n功能键 切换　双击 清除配对"
#define TXT_FORGET     "忘记配对？"
#define TXT_FORGET_HINT "功能键：清除所有配对\n上 / 下键：取消"
#define TXT_NEXT       "下一页 ▶"
#define TXT_PREV       "◀ 上一页"
#define TXT_OFFLINE    "未连接，先在蓝牙里配对"
#define TXT_CLEARED    "配对已清除"
#define TXT_CONNECTED_TO "已连接到 %s"

static void fonts_init(void)
{
    s_font = pager_font_16;
    s_font.fallback = &lv_font_montserrat_16;
    s_font_big = pager_font_24;
    s_font_big.fallback = &lv_font_montserrat_20;
}

// 启动时把界面上所有固定文案逐字查一遍，缺字记到日志里（U+XXXX）。
static void check_glyphs(void)
{
    int missing = 0, checked = 0;
    const struct {
        const char *text;
        const lv_font_t *font;
        int px;
    } rendered_texts[] = {
        {TXT_TITLE, &s_font_big, 24},
        {STATUS_TEXT[0], &s_font, 16},
        {STATUS_TEXT[1], &s_font, 16},
        {STATUS_TEXT[2], &s_font, 16},
        {TXT_FOOTER, &s_font, 16},
        {TXT_FORGET, &s_font_big, 24},
        {TXT_FORGET_HINT, &s_font, 16},
        {TXT_NEXT, &s_font, 16},
        {TXT_PREV, &s_font, 16},
        {TXT_OFFLINE, &s_font, 16},
        {TXT_CLEARED, &s_font, 16},
        {TXT_CONNECTED_TO, &s_font, 16},
        {"0123456789%-", &s_font, 16},
        {LV_SYMBOL_BLUETOOTH LV_SYMBOL_BATTERY_FULL LV_SYMBOL_BATTERY_3
         LV_SYMBOL_BATTERY_2 LV_SYMBOL_BATTERY_1 LV_SYMBOL_BATTERY_EMPTY, &s_font, 16},
    };
    for (size_t i = 0; i < sizeof(rendered_texts) / sizeof(rendered_texts[0]); ++i) {
        uint32_t ofs = 0;
        while (rendered_texts[i].text[ofs]) {
            uint32_t cp = lv_text_encoded_next(rendered_texts[i].text, &ofs);
            if (cp < 0x20) continue;
            lv_font_glyph_dsc_t g = {0};
            ++checked;
            if (!lv_font_get_glyph_dsc(rendered_texts[i].font, &g, cp, 0) || g.is_placeholder) {
                ESP_LOGW(TAG, "Missing glyph U+%04" PRIX32 " (%dpx)", cp, rendered_texts[i].px);
                ++missing;
            }
        }
    }
    const struct {
        const char *text;
        const lv_font_t *font;
        int px;
    } mode_texts[] = {
        {pager_mode_name(PAGER_MODE_CLICK), &s_font_big, 24},
        {pager_mode_hint(PAGER_MODE_CLICK), &s_font, 16},
        {pager_mode_name(PAGER_MODE_VOLUME), &s_font_big, 24},
        {pager_mode_hint(PAGER_MODE_VOLUME), &s_font, 16},
    };
    for (size_t i = 0; i < sizeof(mode_texts) / sizeof(mode_texts[0]); ++i) {
        uint32_t ofs = 0;
        while (mode_texts[i].text[ofs]) {
            uint32_t cp = lv_text_encoded_next(mode_texts[i].text, &ofs);
            if (cp < 0x20) continue;
            lv_font_glyph_dsc_t g = {0};
            ++checked;
            if (!lv_font_get_glyph_dsc(mode_texts[i].font, &g, cp, 0) || g.is_placeholder) {
                ESP_LOGW(TAG, "Missing glyph U+%04" PRIX32 " (%dpx)", cp, mode_texts[i].px);
                ++missing;
            }
        }
    }
    ESP_LOGI(TAG, "Glyph check: %d of %d glyphs missing", missing, checked);
}

static lv_obj_t *label(lv_obj_t *parent, uint32_t color, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_width(l, 210);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(l, &s_font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_line_space(l, 3, 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    return l;
}

// 调用方必须持有 LVGL 锁。
static void render_battery(void)
{
    const char *icon = s_battery < 0   ? LV_SYMBOL_BATTERY_EMPTY
                       : s_battery >= 80 ? LV_SYMBOL_BATTERY_FULL
                       : s_battery >= 55 ? LV_SYMBOL_BATTERY_3
                       : s_battery >= 30 ? LV_SYMBOL_BATTERY_2
                       : s_battery >= 10 ? LV_SYMBOL_BATTERY_1
                                         : LV_SYMBOL_BATTERY_EMPTY;
    if (s_battery < 0) lv_label_set_text_fmt(s_batt, "%s --", icon);
    else lv_label_set_text_fmt(s_batt, "%s %d%%", icon, s_battery);
    lv_obj_set_style_text_color(s_batt, lv_color_hex(s_battery >= 0 && s_battery < 15 ? 0xE5534B : 0xC8D2DA), 0);
    lv_obj_align_to(s_bt_icon, s_batt, LV_ALIGN_OUT_LEFT_MID, -8, 0);
}

// 调用方必须持有 LVGL 锁。
static void render(void)
{
    if (s_ble == PAGER_BLE_READY && s_peer[0]) lv_label_set_text_fmt(s_status, TXT_CONNECTED_TO, s_peer);
    else lv_label_set_text(s_status, STATUS_TEXT[s_ble]);
    lv_obj_set_style_text_color(s_status, lv_color_hex(STATUS_COLOR[s_ble]), 0);
    // 右上角：蓝牙图标灰 = 未连接，蓝 = 正在连接，绿 = 已连接。
    static const uint32_t bt_color[] = {0x5A6670, 0x7FB8FF, 0x5BD68A};
    lv_obj_set_style_text_color(s_bt_icon, lv_color_hex(bt_color[s_ble]), 0);
    render_battery();
    lv_obj_t *chip = lv_obj_get_parent(s_mode);
    if (s_forget_deadline) {
        lv_obj_set_style_bg_color(chip, lv_color_hex(0xE5534B), 0);
        lv_label_set_text(s_mode, TXT_FORGET);
        lv_label_set_text(s_hint, TXT_FORGET_HINT);
        return;
    }
    lv_obj_set_style_bg_color(chip, lv_color_hex(0x5BD68A), 0);
    lv_label_set_text(s_mode, pager_mode_name(s_pager_mode));
    lv_label_set_text(s_hint, pager_mode_hint(s_pager_mode));
}

static void show_last(const char *text)
{
    if (bsp_lvgl_lock(100)) {
        lv_label_set_text(s_last, text);
        bsp_lvgl_unlock();
    }
}

static void refresh(void)
{
    if (bsp_lvgl_lock(100)) {
        render();
        bsp_lvgl_unlock();
    }
}

static void build_ui(void)
{
    assert(bsp_lvgl_lock(0));
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_t *title = label(screen, 0xFFFFFF, 32);
    lv_obj_set_style_text_font(title, &s_font_big, 0);
    lv_label_set_text(title, TXT_TITLE);
    s_status = label(screen, 0xF5B942, 64);
    // 设备名可能很长，只占一行，放不下就用省略号。
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(s_status, lv_font_get_line_height(&s_font));

    // 右上角状态栏。屏幕四角是半径 30 的圆角，往里收一点免得被切掉。
    s_batt = lv_label_create(screen);
    lv_obj_set_style_text_font(s_batt, &s_font, 0);
    lv_obj_align(s_batt, LV_ALIGN_TOP_RIGHT, -30, 8);
    s_bt_icon = lv_label_create(screen);
    lv_obj_set_style_text_font(s_bt_icon, &s_font, 0);
    lv_label_set_text(s_bt_icon, LV_SYMBOL_BLUETOOTH);

    // 当前模式放在一个圆角色块里，一眼能看到。
    lv_obj_t *chip = lv_obj_create(screen);
    lv_obj_remove_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(chip, 150, 48);
    lv_obj_set_style_radius(chip, 24, 0);
    lv_obj_set_style_border_width(chip, 0, 0);
    lv_obj_set_style_pad_all(chip, 0, 0);
    lv_obj_align(chip, LV_ALIGN_TOP_MID, 0, 96);
    s_mode = lv_label_create(chip);
    lv_obj_set_style_text_font(s_mode, &s_font_big, 0);
    lv_obj_set_style_text_color(s_mode, lv_color_hex(0x10151A), 0);
    lv_obj_center(s_mode);

    s_hint = label(screen, 0x9DB8C4, 158);
    s_last = label(screen, 0xFFC6BB, 214);
    s_footer = label(screen, 0x6B7A86, 248);
    lv_label_set_text(s_footer, TXT_FOOTER);
    lv_label_set_text(s_last, "");
    render();
    bsp_lvgl_unlock();
}

static void page(pager_dir_t dir)
{
    if (s_forget_deadline) {  // 确认界面里 UP/DOWN 是取消
        s_forget_deadline = 0;
        refresh();
        return;
    }
    const char *text = pager_ble_tap(pager_key_for(s_pager_mode, dir))
                           ? (dir == PAGER_NEXT ? TXT_NEXT : TXT_PREV)
                           : TXT_OFFLINE;
    ESP_LOGI(TAG, "%s (%s)", text, pager_mode_name(s_pager_mode));
    show_last(text);
}

static void handle(const ev_t *ev)
{
    switch (ev->kind) {
    case EV_PREV: page(PAGER_PREV); break;
    case EV_NEXT: page(PAGER_NEXT); break;
    case EV_MODE:
        if (s_forget_deadline) {
            s_forget_deadline = 0;
            pager_ble_forget_all();
            show_last(TXT_CLEARED);
        } else {
            s_pager_mode = pager_next_mode(s_pager_mode);
            save_mode();
            ESP_LOGI(TAG, "Mode: %s", pager_mode_name(s_pager_mode));
        }
        refresh();
        break;
    case EV_FORGET_ASK:
        s_forget_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(FORGET_CONFIRM_MS);
        refresh();
        break;
    case EV_BLE:
        s_ble = ev->ble;
        strlcpy(s_peer, ev->peer, sizeof(s_peer));
        if (s_ble == PAGER_BLE_READY) show_last("");
        refresh();
        break;
    }
}

static void poll_battery(void)
{
    int soc = bsp_battery_soc();
    if (soc == s_battery) return;
    s_battery = soc;
    ESP_LOGI(TAG, "Battery: %d%%", soc);
    if (soc >= 0) pager_ble_set_battery(soc);
    refresh();
}

void app_main(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "Running partition: %s", running->label);
    s_events = xQueueCreate(16, sizeof(ev_t));
    assert(s_events);

    // NVS 和系统共用。初始化失败也不擦除，只是记不住配对和模式。
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) ESP_LOGW(TAG, "NVS unavailable (%s); pairing will not persist", esp_err_to_name(err));
    else load_mode();

    ESP_ERROR_CHECK(bsp_display_init());
    assert(bsp_lvgl_init());
    fonts_init();
    check_glyphs();
    build_ui();
    bsp_display_backlight(20);
    ESP_ERROR_CHECK(bsp_button_init(on_button, NULL));
    // 板子上没有电量计时（比如模拟器）返回 NOT_FOUND，右上角显示 "--"。
    if (bsp_battery_init() != ESP_OK) ESP_LOGW(TAG, "Battery gauge unavailable");
#if CONFIG_PAGER_SIM_NO_BLE
    ESP_LOGW(TAG, "Simulator UI build: Bluetooth not started");
#else
    ESP_ERROR_CHECK(pager_ble_start(DEVICE_NAME, on_ble_state));
#endif

    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK &&
        ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_ERROR_CHECK(esp_ota_mark_app_valid_cancel_rollback());
        ESP_LOGI(TAG, "Trial boot confirmed valid");
    }
    ESP_LOGI(TAG, "Ready: mode=%s", pager_mode_name(s_pager_mode));
    poll_battery();
    TickType_t next_battery = xTaskGetTickCount() + pdMS_TO_TICKS(BATTERY_POLL_MS);

    while (true) {
        ev_t ev;
        if (xQueueReceive(s_events, &ev, pdMS_TO_TICKS(500)) == pdTRUE) handle(&ev);
        if (s_forget_deadline && (int32_t)(xTaskGetTickCount() - s_forget_deadline) > 0) {
            s_forget_deadline = 0;
            refresh();
        }
        if ((int32_t)(xTaskGetTickCount() - next_battery) >= 0) {
            next_battery += pdMS_TO_TICKS(BATTERY_POLL_MS);
            poll_battery();
        }
    }
}
