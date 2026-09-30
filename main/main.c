#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "lvgl.h"
#include "smoking_model.h"
#include "smoking_store.h"

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

LV_FONT_DECLARE(smoking_font_18);

static const char *TAG = "tools_app";

#define INPUT_QUEUE_DEPTH 8
#define AUDIO_QUEUE_DEPTH 4
#define IDLE_BACKLIGHT_SEC 60u
#define UI_REFRESH_MS 250
#define AUDIO_HZ 16000u
#define BEEP_SAMPLES 960u

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;

typedef enum {
    AUDIO_BEEP_CLICK = 1,
    AUDIO_BEEP_DONE,
} audio_event_t;

static smoking_model_t s_model;
static QueueHandle_t s_input_queue;
static QueueHandle_t s_audio_queue;
static TaskHandle_t s_input_task;
static bool s_input_ready;
static bool s_battery_ok;
static bool s_audio_ok;
static bool s_backlight_on = true;
static uint32_t s_last_input_sec;
static lv_obj_t *s_screen;
static lv_font_t s_cjk_font;
static bool s_count_wait_ok_release;
static int16_t s_beep_samples[BEEP_SAMPLES];

static uint32_t now_sec(void)
{
    time_t raw = time(NULL);
    if (raw < 0) raw = 0;
    return (uint32_t)raw;
}

static int32_t today_day(void)
{
    return (int32_t)(now_sec() / 86400u);
}

static void format_clock(uint32_t now, char *buf, size_t len)
{
    time_t raw = (time_t)now;
    struct tm tm_now;
    localtime_r(&raw, &tm_now);
    snprintf(buf, len, "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
}

static void format_duration(uint32_t sec, char *buf, size_t len)
{
    uint32_t h = sec / 3600u;
    uint32_t m = (sec / 60u) % 60u;
    uint32_t s = sec % 60u;
    if (h) {
        snprintf(buf, len, "%lu:%02lu:%02lu", (unsigned long)h,
                 (unsigned long)m, (unsigned long)s);
    } else {
        snprintf(buf, len, "%02lu:%02lu", (unsigned long)m, (unsigned long)s);
    }
}

static void beep(audio_event_t event)
{
    if (!s_audio_queue) return;
    (void)xQueueSend(s_audio_queue, &event, 0);
}

static void set_common_text(lv_obj_t *obj, uint32_t color)
{
    lv_obj_set_style_text_font(obj, &s_cjk_font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, int x, int y,
                       int w, uint32_t color)
{
    lv_obj_t *obj = lv_label_create(parent);
    set_common_text(obj, color);
    lv_obj_set_width(obj, w);
    lv_label_set_text(obj, text);
    lv_obj_set_pos(obj, x, y);
    return obj;
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
    lv_obj_set_style_border_width(obj, 2, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(border), 0);
    return obj;
}

static void render_header(lv_obj_t *root, uint32_t now)
{
    char clock[16];
    format_clock(now, clock, sizeof(clock));
    label(root, clock, 12, 8, 80, 0x1F2937);

    if (!s_battery_ok) return;
    int soc = bsp_battery_soc();
    if (soc < 0) return;
    char text[16];
    snprintf(text, sizeof(text), "%d%%", soc);
    lv_obj_t *battery = label(root, text, 178, 8, 50, soc < 20 ? 0xB91C1C : 0x166534);
    lv_obj_set_style_text_align(battery, LV_TEXT_ALIGN_RIGHT, 0);
}

static void render_clock_only(lv_obj_t *root, uint32_t now)
{
    char clock[16];
    format_clock(now, clock, sizeof(clock));
    label(root, clock, 12, 8, 80, 0x1F2937);
}

static void render_home(lv_obj_t *root, uint32_t now)
{
    label(root, "小工具", 12, 32, 120, 0x111827);
    if (s_model.storage_warning || s_model.dirty) {
        label(root, "待保存", 170, 32, 58, 0xB91C1C);
    }

    smoking_feature_t selected = smoking_model_selected_feature(&s_model);
    for (int i = -1; i <= 1; i++) {
        int index = ((int)selected + i + (int)SMOKING_FEATURE_COUNT) %
                    (int)SMOKING_FEATURE_COUNT;
        bool is_selected = i == 0;
        uint32_t bg = is_selected ? 0xDBEAFE : 0xF8FAFC;
        uint32_t bd = is_selected ? 0x2563EB : 0xCBD5E1;
        lv_obj_t *card = panel(root, 14, 62 + (i + 1) * 42, 212, 34, bg, bd);
        lv_obj_t *name = label(card, smoking_feature_name((smoking_feature_t)index),
                               10, 5, 180, is_selected ? 0x1D4ED8 : 0x334155);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    }

    if (selected == SMOKING_FEATURE_SMOKE) {
        smoking_totals_t today = smoking_model_today(&s_model, today_day());
        smoking_totals_t week = smoking_model_last_7_days(&s_model, today_day());
        char dur[24];
        char line[72];
        int y = 196;
        format_duration(today.duration_sec, dur, sizeof(dur));
        snprintf(line, sizeof(line), "今日 %lu 根  %s",
                 (unsigned long)today.cigarettes, dur);
        label(root, line, 14, y, 212, 0x111827);
        y += 24;
        format_duration(week.duration_sec, dur, sizeof(dur));
        snprintf(line, sizeof(line), "七天 %lu 根  %s",
                 (unsigned long)week.cigarettes, dur);
        label(root, line, 14, y, 212, 0x111827);
        y += 24;
        if (s_model.persisted.last.valid) {
            format_duration(s_model.persisted.last.duration_sec, dur, sizeof(dur));
            snprintf(line, sizeof(line), "上次 %u 根  %s",
                     s_model.persisted.last.cigarettes, dur);
            label(root, line, 14, y, 212, 0x475569);
            y += 24;
            format_duration(smoking_model_last_interval_sec(&s_model, now),
                            dur, sizeof(dur));
            snprintf(line, sizeof(line), "间隔 %s", dur);
            label(root, line, 14, y, 212, 0x475569);
        } else {
            label(root, "还没有抽烟记录", 14, y, 212, 0x64748B);
        }
    } else {
        label(root, "OK 进入  长按调时间", 14, 208, 212, 0x475569);
        label(root, "UP/DOWN 循环切换", 14, 234, 212, 0x475569);
    }
}

static void render_smoking_active(lv_obj_t *root, uint32_t now)
{
    label(root, "抽烟中", 14, 46, 120, 0x991B1B);
    char dur[24];
    format_duration(now > s_model.smoking_started_at ?
                    now - s_model.smoking_started_at : 0, dur, sizeof(dur));
    lv_obj_t *timer = label(root, dur, 22, 116, 196, 0x111827);
    lv_obj_set_style_text_font(timer, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(timer, LV_TEXT_ALIGN_CENTER, 0);
    label(root, "长按结束抽烟", 52, 210, 150, 0x475569);
}

static void render_count_select(lv_obj_t *root)
{
    label(root, "记录根数", 14, 48, 140, 0x111827);
    char dur[24];
    char line[48];
    format_duration(s_model.pending_duration_sec, dur, sizeof(dur));
    snprintf(line, sizeof(line), "持续 %s", dur);
    label(root, line, 14, 82, 180, 0x475569);
    snprintf(line, sizeof(line), "%u 根", s_model.pending_cigarettes);
    lv_obj_t *count = label(root, line, 64, 134, 112, 0x1D4ED8);
    lv_obj_set_style_text_align(count, LV_TEXT_ALIGN_CENTER, 0);
    if (s_count_wait_ok_release) {
        label(root, "选择根数", 76, 218, 100, 0xB45309);
    } else {
        label(root, "UP 减少  DOWN 增加", 24, 218, 190, 0x475569);
    }
    label(root, "OK 保存", 76, 246, 100, 0x475569);
}

static void render_sudoku(lv_obj_t *root)
{
    char title[32];
    snprintf(title, sizeof(title), "数独 第 %u 关", s_model.persisted.sudoku.level + 1);
    label(root, title, 14, 38, 150, 0x111827);
    label(root, s_model.persisted.sudoku.mode == SMOKING_SUDOKU_EDIT_DIGIT ?
          "改数字" : "选空格", 156, 38, 70, 0x2563EB);

    size_t selected_cell = 0;
    uint8_t selected_empty = s_model.persisted.sudoku.selected_empty;
    size_t seen = 0;
    for (size_t i = 0; i < SMOKING_SUDOKU_CELLS; i++) {
        if (smoking_model_sudoku_cell_fixed(&s_model, i)) continue;
        if (seen == selected_empty) {
            selected_cell = i;
            break;
        }
        seen++;
    }

    for (size_t cell = 0; cell < SMOKING_SUDOKU_CELLS; cell++) {
        int row = (int)(cell / SMOKING_SUDOKU_SIZE);
        int col = (int)(cell % SMOKING_SUDOKU_SIZE);
        bool fixed = smoking_model_sudoku_cell_fixed(&s_model, cell);
        bool selected = cell == selected_cell;
        bool error = smoking_model_sudoku_cell_error(&s_model, cell);
        uint32_t bg = fixed ? 0xE2E8F0 : (selected ? 0xFEF3C7 : 0xFFFFFF);
        uint32_t bd = error ? 0xDC2626 : (selected ? 0xD97706 : 0xCBD5E1);
        lv_obj_t *box = panel(root, 20 + col * 40, 74 + row * 40, 34, 34, bg, bd);
        char value[4] = "";
        uint8_t v = smoking_model_sudoku_cell_value(&s_model, cell);
        if (v) snprintf(value, sizeof(value), "%u", v);
        lv_obj_t *num = label(box, value, 0, 4, 30, fixed ? 0x334155 : 0x111827);
        lv_obj_set_style_text_font(num, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_align(num, LV_TEXT_ALIGN_CENTER, 0);
    }
    label(root, "OK 切换  双击重置", 22, 266, 196, 0x475569);
    lv_obj_t *back = label(root, "长按返回", 22, 290, 196, 0x475569);
    lv_obj_set_style_text_align(back, LV_TEXT_ALIGN_CENTER, 0);
}

static void render_settings(lv_obj_t *root)
{
    label(root, "设置", 14, 44, 80, 0x111827);
    bool time_selected = s_model.selected_setting == SMOKING_SETTING_TIME;
    bool wifi_selected = s_model.selected_setting == SMOKING_SETTING_WIFI;
    bool editing = s_model.editing_time;

    panel(root, 14, 78, 212, 82,
          time_selected ? 0xDBEAFE : 0xF8FAFC,
          time_selected ? 0x2563EB : 0xCBD5E1);
    label(root, "时间设置", 28, 98, 100, 0x111827);
    char value[24];
    snprintf(value, sizeof(value), "%02d:%02d:00", s_model.edit_hour, s_model.edit_minute);
    lv_obj_t *time_label = label(root, value, 28, 128, 160, 0x2563EB);
    lv_obj_set_style_text_font(time_label, &lv_font_montserrat_20, 0);
    label(root, editing ?
          (s_model.edit_minute_selected ? "编辑分钟" : "编辑小时") :
          "OK 进入编辑",
          28, 150, 160, editing ? 0xB45309 : 0x64748B);

    panel(root, 14, 172, 212, 50,
          wifi_selected ? 0xDBEAFE : 0xFFFFFF,
          wifi_selected ? 0x2563EB : 0xCBD5E1);
    label(root, "Wi-Fi 设置", 28, 188, 100, wifi_selected ? 0x1D4ED8 : 0x475569);
    label(root, "待接入", 156, 188, 62, 0x94A3B8);

    if (editing) {
        label(root, "UP/DOWN 调整", 20, 246, 200, 0x475569);
        label(root, "OK 切字段  长按保存", 20, 272, 200, 0x475569);
    } else {
        label(root, "UP/DOWN 选择", 20, 246, 200, 0x475569);
        label(root, "OK 进入  长按返回", 20, 272, 200, 0x475569);
    }
}

static void render_ui(void)
{
    uint32_t now = now_sec();
    if (!s_screen) {
        s_screen = lv_obj_create(NULL);
        lv_screen_load(s_screen);
    }
    lv_obj_remove_style_all(s_screen);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0xF1F5F9), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clean(s_screen);

    if (s_model.persisted.screen == SMOKING_SCREEN_ACTIVE) {
        render_clock_only(s_screen, now);
    } else {
        render_header(s_screen, now);
    }
    switch ((smoking_screen_t)s_model.persisted.screen) {
    case SMOKING_SCREEN_HOME:
        render_home(s_screen, now);
        break;
    case SMOKING_SCREEN_ACTIVE:
        render_smoking_active(s_screen, now);
        break;
    case SMOKING_SCREEN_COUNT_SELECT:
        render_count_select(s_screen);
        break;
    case SMOKING_SCREEN_SUDOKU:
        render_sudoku(s_screen);
        break;
    case SMOKING_SCREEN_SETTINGS:
        render_settings(s_screen);
        break;
    default:
        smoking_model_return_home(&s_model);
        render_home(s_screen, now);
        break;
    }
}

static void request_render(void)
{
    if (!bsp_lvgl_lock(500)) return;
    render_ui();
    bsp_lvgl_unlock();
}

static void queue_save_if_dirty(void)
{
    if (!s_model.dirty) return;
    smoking_persist_state_t snapshot = smoking_model_persist_snapshot(&s_model);
    esp_err_t err = smoking_store_save_async(&snapshot);
    if (err == ESP_OK) {
        s_model.dirty = false;
    } else {
        s_model.storage_warning = true;
    }
}

static void store_status(esp_err_t result, void *user)
{
    (void)user;
    s_model.storage_warning = result != ESP_OK;
}

static void save_time_and_return_home(void)
{
    time_t raw = (time_t)now_sec();
    struct tm tm_now;
    localtime_r(&raw, &tm_now);
    tm_now.tm_hour = s_model.edit_hour;
    tm_now.tm_min = s_model.edit_minute;
    tm_now.tm_sec = 0;
    time_t updated = mktime(&tm_now);
    struct timeval tv = {
        .tv_sec = updated,
        .tv_usec = 0,
    };
    if (updated >= 0) {
        settimeofday(&tv, NULL);
    }
    smoking_model_return_home(&s_model);
    s_backlight_on = true;
    s_last_input_sec = now_sec();
    bsp_display_backlight(100);
}

static bool count_select_ready_for_input(const input_event_t *input)
{
    if (!s_count_wait_ok_release) return true;
    if (input->btn != BSP_BTN_OK) {
        s_count_wait_ok_release = false;
        return true;
    }

    if (input->event == BSP_BTN_PRESS) {
        s_count_wait_ok_release = false;
    }

    return !s_count_wait_ok_release;
}

static void process_input(const input_event_t *input)
{
    uint32_t now = now_sec();
    if (!s_backlight_on) {
        bsp_display_backlight(100);
        s_backlight_on = true;
        s_last_input_sec = now;
        request_render();
        return;
    }
    s_last_input_sec = now;

    smoking_screen_t screen = (smoking_screen_t)s_model.persisted.screen;
    if (screen == SMOKING_SCREEN_HOME) {
        if (input->event == BSP_BTN_LONG && input->btn == BSP_BTN_OK) {
            smoking_model_enter_time_settings(&s_model, now);
        } else if (input->event == BSP_BTN_CLICK && input->btn == BSP_BTN_UP) {
            smoking_model_home_prev(&s_model);
        } else if (input->event == BSP_BTN_CLICK && input->btn == BSP_BTN_DOWN) {
            smoking_model_home_next(&s_model);
        } else if (input->event == BSP_BTN_CLICK && input->btn == BSP_BTN_OK) {
            smoking_model_enter_selected(&s_model, now);
        }
    } else if (screen == SMOKING_SCREEN_ACTIVE) {
        if (input->event == BSP_BTN_LONG && input->btn == BSP_BTN_OK) {
            smoking_model_finish(&s_model, now);
            s_count_wait_ok_release = true;
            beep(AUDIO_BEEP_DONE);
        }
    } else if (screen == SMOKING_SCREEN_COUNT_SELECT) {
        if (!count_select_ready_for_input(input)) {
            request_render();
            return;
        } else if (input->event == BSP_BTN_CLICK && input->btn == BSP_BTN_UP) {
            smoking_model_count_down(&s_model);
        } else if (input->event == BSP_BTN_CLICK && input->btn == BSP_BTN_DOWN) {
            smoking_model_count_up(&s_model);
        } else if (input->event == BSP_BTN_CLICK && input->btn == BSP_BTN_OK) {
            smoking_model_save_pending(&s_model, now);
            queue_save_if_dirty();
            beep(AUDIO_BEEP_DONE);
        }
    } else if (screen == SMOKING_SCREEN_SUDOKU) {
        if (input->event == BSP_BTN_LONG && input->btn == BSP_BTN_OK) {
            smoking_model_return_home(&s_model);
        } else if (input->event == BSP_BTN_DOUBLE && input->btn == BSP_BTN_OK) {
            smoking_model_sudoku_reset(&s_model);
            queue_save_if_dirty();
        } else if (input->event == BSP_BTN_CLICK && input->btn == BSP_BTN_OK) {
            smoking_model_sudoku_click(&s_model);
            queue_save_if_dirty();
        } else if (input->event == BSP_BTN_CLICK && input->btn == BSP_BTN_UP) {
            smoking_model_sudoku_up(&s_model);
            queue_save_if_dirty();
        } else if (input->event == BSP_BTN_CLICK && input->btn == BSP_BTN_DOWN) {
            smoking_model_sudoku_down(&s_model);
            queue_save_if_dirty();
        }
    } else if (screen == SMOKING_SCREEN_SETTINGS) {
        if (input->event == BSP_BTN_LONG && input->btn == BSP_BTN_OK) {
            if (s_model.editing_time) {
                save_time_and_return_home();
            } else {
                smoking_model_return_home(&s_model);
            }
        } else if (input->event == BSP_BTN_CLICK && input->btn == BSP_BTN_OK) {
            smoking_model_settings_click(&s_model);
        } else if (input->event == BSP_BTN_CLICK && input->btn == BSP_BTN_UP) {
            smoking_model_settings_up(&s_model);
        } else if (input->event == BSP_BTN_CLICK && input->btn == BSP_BTN_DOWN) {
            smoking_model_settings_down(&s_model);
        }
    }
    request_render();
}

static void input_task(void *arg)
{
    (void)arg;
    input_event_t input;
    TickType_t last_refresh = xTaskGetTickCount();
    for (;;) {
        if (xQueueReceive(s_input_queue, &input, pdMS_TO_TICKS(UI_REFRESH_MS)) == pdTRUE) {
            process_input(&input);
        }
        uint32_t now = now_sec();
        if (s_backlight_on && smoking_model_idle_backlight_allowed(&s_model) &&
            now - s_last_input_sec >= IDLE_BACKLIGHT_SEC) {
            bsp_display_backlight(0);
            s_backlight_on = false;
        }
        TickType_t tick_now = xTaskGetTickCount();
        if (tick_now - last_refresh >= pdMS_TO_TICKS(1000)) {
            request_render();
            queue_save_if_dirty();
            last_refresh = tick_now;
        }
    }
}

static void audio_task(void *arg)
{
    (void)arg;
    audio_event_t event;
    for (;;) {
        if (xQueueReceive(s_audio_queue, &event, portMAX_DELAY) != pdTRUE) continue;
        if (!s_audio_ok) continue;
        uint32_t period = event == AUDIO_BEEP_DONE ? 40u : 70u;
        for (size_t i = 0; i < BEEP_SAMPLES; i++) {
            s_beep_samples[i] = ((i / period) & 1u) ? 1800 : -1800;
        }
        if (bsp_audio_set_format(AUDIO_HZ, 16, 1) == ESP_OK) {
            (void)bsp_audio_write(s_beep_samples, sizeof(s_beep_samples));
        }
    }
}

static esp_err_t input_dispatch_init(void)
{
    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(input_event_t));
    if (!s_input_queue) return ESP_ERR_NO_MEM;
    if (xTaskCreate(input_task, "tools_input", 4096, NULL, 5, &s_input_task) != pdPASS) {
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    if (!s_input_ready || !s_input_queue) return;
    const input_event_t input = { .btn = btn, .event = ev };
    (void)xQueueSend(s_input_queue, &input, 0);
}

void app_main(void)
{
    ESP_LOGI(TAG, "AI Passport 小工具启动");
    smoking_model_init(&s_model);
    s_cjk_font = smoking_font_18;
    s_cjk_font.fallback = &lv_font_montserrat_20;

    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err != ESP_OK) {
        ESP_LOGW(TAG, "NVS 初始化失败: %s", esp_err_to_name(nvs_err));
        s_model.storage_warning = true;
    }

    bsp_i2c_init();
    bsp_i2c_scan();

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败，检查 SPI 引脚(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);
    s_last_input_sec = now_sec();

    s_battery_ok = bsp_battery_init() == ESP_OK;
    s_audio_ok = bsp_audio_init() == ESP_OK;
    s_audio_queue = xQueueCreate(AUDIO_QUEUE_DEPTH, sizeof(audio_event_t));
    if (s_audio_queue) {
        (void)xTaskCreate(audio_task, "tools_audio", 3072, NULL, 3, NULL);
    }

    if (nvs_err == ESP_OK) {
        smoking_persist_state_t restored;
        bool loaded = false;
        esp_err_t load_err = smoking_store_load(&restored, &loaded);
        if (load_err == ESP_OK && loaded) {
            smoking_model_restore(&s_model, &restored);
        } else if (load_err != ESP_OK && load_err != ESP_ERR_INVALID_CRC) {
            ESP_LOGW(TAG, "读取抽烟状态失败: %s", esp_err_to_name(load_err));
            s_model.storage_warning = true;
        }
        esp_err_t store_err = smoking_store_init(store_status, NULL);
        if (store_err != ESP_OK) {
            ESP_LOGW(TAG, "保存任务启动失败: %s", esp_err_to_name(store_err));
            s_model.storage_warning = true;
        }
    }

    esp_err_t input_err = input_dispatch_init();
    esp_err_t button_err = input_err == ESP_OK ?
        bsp_button_init(on_key, NULL) : ESP_ERR_INVALID_STATE;
    if (input_err != ESP_OK || button_err != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败: input=%s button=%s",
                 esp_err_to_name(input_err), esp_err_to_name(button_err));
    } else {
        s_input_ready = true;
    }

    request_render();
    ESP_LOGI(TAG, "就绪: Battery=%d Audio=%d NVS=%d",
             s_battery_ok, s_audio_ok, nvs_err == ESP_OK);
}
