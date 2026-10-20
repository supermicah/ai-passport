#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "key_guard.h"
#include "lvgl.h"
#include "smoking_model.h"
#include "smoking_store.h"
#include "tools_radio.h"
#include "tools_weather.h"
#include "tools_wifi.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_rtc_time.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

LV_FONT_DECLARE(smoking_font_18);

static const char *TAG = "tools_app";

#define INPUT_QUEUE_DEPTH 8
#define AUDIO_QUEUE_DEPTH 4
#define IDLE_BACKLIGHT_SEC 20u
#define IDLE_DEEP_SLEEP_SEC (3u * 60u)
#define SCREEN_OFF_DEEP_SLEEP_SEC (3u * 60u)
#define IDLE_SLEEP_TIMER_WAKE_SEC 0u
#define UI_REFRESH_MS 1000
#define AUDIO_HZ 16000u
#define BEEP_SAMPLES 960u
#define CLOCK_SAVE_INTERVAL_SEC (5u * 60u)
#define CLOCK_DEEP_SLEEP_ELAPSED_MAX_SEC (30u * 24u * 60u * 60u)
#define CLOCK_BOOTSTRAP_UNIX_SEC 1792410292u
#define CLOCK_BOOTSTRAP_FUTURE_TOLERANCE_SEC 120u
#define POWER_LOG_INTERVAL_SEC (5u * 60u)
#define BATTERY_WAKE_ZERO_FILTER_SEC 20u
#define BATTERY_ZERO_VALID_MAX_MV 3300
#define BACKLIGHT_NORMAL_PERCENT 15u
#define BACKLIGHT_RADIO_PERCENT 25u
#define BACKLIGHT_SMOKING_PERCENT 12u
#define BACKLIGHT_SMOKING_DIM_PERCENT 2u
#define BACKLIGHT_ACTIVE_DIM_SEC 10u
#define LOCAL_TIME_OFFSET_SEC (8u * 3600u)
#define DEEP_SLEEP_MAGIC 0x544F4F4Cu

#define POWER_FLAG_BACKLIGHT 0x01u
#define POWER_FLAG_WIFI 0x02u
#define POWER_FLAG_WIFI_PROVISIONING 0x04u
#define POWER_FLAG_RADIO_WANTED 0x08u
#define POWER_FLAG_RADIO_ACTIVE 0x10u

#define UI_BG 0x07120F
#define UI_SURFACE 0x10211B
#define UI_SURFACE_2 0x173026
#define UI_SURFACE_3 0x203A2F
#define UI_LINE 0x2B473B
#define UI_TEXT 0xEEF1E8
#define UI_MUTED 0xA8C2B2
#define UI_DIM 0x6F887B
#define UI_GREEN 0x20E47C
#define UI_GREEN_DARK 0x0F5E3B
#define UI_AMBER 0xF7B955
#define UI_BLUE 0x38BDF8
#define UI_RED 0xF87171

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
static bool s_entering_deep_sleep;
static bool s_clock_persistence_enabled;
static bool s_manual_screen_off;
static uint32_t s_last_input_sec;
static uint32_t s_last_clock_save_sec;
static uint32_t s_last_power_log_sec;
static uint32_t s_battery_zero_filter_until_sec;
static uint64_t s_boot_rtc_us;
static uint8_t s_backlight_percent = BACKLIGHT_NORMAL_PERCENT;
static lv_obj_t *s_screen;
static lv_font_t s_cjk_font;
static bool s_count_wait_ok_release;
static bool s_ignore_wake_tail;
static int16_t s_beep_samples[BEEP_SAMPLES];
static RTC_DATA_ATTR uint32_t s_deep_sleep_magic;
static RTC_DATA_ATTR uint32_t s_deep_sleep_count;
static RTC_DATA_ATTR uint32_t s_deep_sleep_clock_sec;
static RTC_DATA_ATTR uint64_t s_deep_sleep_rtc_us;
static RTC_DATA_ATTR int s_deep_sleep_battery_soc = -1;

#define KEY_GUARD_MAX_US (10LL * 1000 * 1000)
static key_guard_t s_key_guard;

static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user);
static void log_deep_sleep_warning(const char *step, esp_err_t err);

static uint32_t now_sec(void)
{
    time_t raw = time(NULL);
    if (raw < 0) raw = 0;
    return (uint32_t)raw;
}

static int filtered_battery_soc(void)
{
    int soc = bsp_battery_soc();
    if (soc == 0 && s_deep_sleep_battery_soc > 0 &&
        now_sec() < s_battery_zero_filter_until_sec) {
        int mv = bsp_battery_mv();
        if (mv < 0 || mv > BATTERY_ZERO_VALID_MAX_MV) {
            return s_deep_sleep_battery_soc;
        }
    }
    if (soc > 0) s_deep_sleep_battery_soc = soc;
    return soc;
}

static int32_t today_day(void)
{
    return (int32_t)((now_sec() + LOCAL_TIME_OFFSET_SEC) / 86400u);
}

static uint32_t model_time_sec(uint32_t now)
{
    return now + LOCAL_TIME_OFFSET_SEC;
}

static void init_timezone(void)
{
    setenv("TZ", "CST-8", 1);
    tzset();
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

static uint8_t desired_backlight_percent(uint32_t now)
{
    smoking_screen_t screen = (smoking_screen_t)s_model.persisted.screen;
    if (screen == SMOKING_SCREEN_ACTIVE) {
        if (now - s_last_input_sec >= BACKLIGHT_ACTIVE_DIM_SEC) {
            return BACKLIGHT_SMOKING_DIM_PERCENT;
        }
        return BACKLIGHT_SMOKING_PERCENT;
    }
    if (screen == SMOKING_SCREEN_RADIO) return BACKLIGHT_RADIO_PERCENT;
    return BACKLIGHT_NORMAL_PERCENT;
}

static void set_backlight_percent(uint8_t percent)
{
    bsp_display_backlight(percent);
    s_backlight_percent = percent;
    s_backlight_on = percent > 0;
}

static void apply_active_backlight(uint32_t now)
{
    if (!s_backlight_on || s_manual_screen_off) return;
    uint8_t percent = desired_backlight_percent(now);
    if (percent != s_backlight_percent) {
        set_backlight_percent(percent);
    }
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

static lv_obj_t *label_one_line(lv_obj_t *parent, const char *text, int x, int y,
                                int w, uint32_t color)
{
    lv_obj_t *obj = label(parent, text, x, y, w, color);
    lv_label_set_long_mode(obj, LV_LABEL_LONG_MODE_CLIP);
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
    lv_obj_set_style_border_width(obj, border ? 1 : 0, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(border), 0);
    return obj;
}

static lv_obj_t *pill(lv_obj_t *parent, int x, int y, int w, int h,
                      uint32_t color, uint32_t border)
{
    lv_obj_t *obj = panel(parent, x, y, w, h, color, border);
    lv_obj_set_style_radius(obj, h / 2, 0);
    return obj;
}

static void render_section_mark(lv_obj_t *root, int x, int y, uint32_t color)
{
    panel(root, x, y, 4, 18, color, 0);
    panel(root, x + 8, y + 7, 28, 3, color, 0);
}

static void render_battery_pill(lv_obj_t *root, int x, int y, uint32_t color)
{
    if (!s_battery_ok) return;
    int soc = filtered_battery_soc();
    if (soc < 0) return;
    uint32_t accent = soc < 20 ? UI_RED : color;
    pill(root, x, y, 36, 18, 0x0B1713, accent);
    panel(root, x + 35, y + 5, 3, 8, accent, 0);
    int fill = soc;
    if (fill > 100) fill = 100;
    if (fill < 0) fill = 0;
    int fill_w = 4 + (fill * 24) / 100;
    pill(root, x + 4, y + 5, fill_w, 8, accent, 0);
}

static void render_setting_row(lv_obj_t *root, int y, const char *title,
                               const char *value, const char *hint,
                               bool selected, bool active)
{
    uint32_t bg = selected ? UI_SURFACE_3 : UI_SURFACE;
    uint32_t bd = selected ? UI_GREEN : UI_LINE;
    uint32_t title_color = selected ? UI_TEXT : UI_MUTED;
    uint32_t value_color = active ? UI_AMBER : (selected ? UI_GREEN : UI_TEXT);

    panel(root, 14, y, 212, 48, bg, bd);
    panel(root, 20, y + 10, 3, 28, active ? UI_AMBER : (selected ? UI_GREEN : UI_LINE), 0);
    label_one_line(root, title, 28, y + 5, 70, title_color);
    lv_obj_t *value_label = label_one_line(root, value, 120, y + 5, 90, value_color);
    lv_obj_set_style_text_align(value_label, LV_TEXT_ALIGN_RIGHT, 0);
    label_one_line(root, hint, 28, y + 26, 182, selected ? UI_GREEN : UI_DIM);
}

static const char *wifi_status_text(tools_wifi_status_t status)
{
    switch (status.state) {
    case TOOLS_WIFI_TIME_SYNCED:
        return "已同步";
    case TOOLS_WIFI_CONNECTED:
        return "同步中";
    case TOOLS_WIFI_CONNECTING:
        return "连接中";
    case TOOLS_WIFI_PHONE_CONNECTED:
        return "手机已连";
    case TOOLS_WIFI_SETUP_READY:
        return "AP已开";
    case TOOLS_WIFI_NO_CREDENTIALS:
        return "未配网";
    case TOOLS_WIFI_FAILED:
        return "失败";
    case TOOLS_WIFI_STARTING:
        return "启动中";
    case TOOLS_WIFI_OFF:
    default:
        return "未启动";
    }
}

static const char *wifi_hint_text(tools_wifi_status_t status)
{
    if (status.provisioning) return TOOLS_WIFI_SETUP_URL;
    if (status.state == TOOLS_WIFI_CONNECTED || status.state == TOOLS_WIFI_TIME_SYNCED) {
        return status.ip[0] ? status.ip : "长按清除";
    }
    if (status.state == TOOLS_WIFI_CONNECTING || status.state == TOOLS_WIFI_STARTING) {
        return "等待网络";
    }
    if (status.ssid[0] != '\0') return "OK 连接";
    return "OK 开AP";
}

static bool wifi_ready_for_radio(tools_wifi_status_t status)
{
    return status.state == TOOLS_WIFI_CONNECTED ||
           status.state == TOOLS_WIFI_TIME_SYNCED;
}

static const char *radio_state_text(tools_radio_state_t state)
{
    switch (state) {
    case TOOLS_RADIO_CONNECTING:
        return "正在连接";
    case TOOLS_RADIO_BUFFERING:
        return "正在缓冲";
    case TOOLS_RADIO_PLAYING:
        return "正在播放";
    case TOOLS_RADIO_RECONNECTING:
        return "正在重连";
    case TOOLS_RADIO_ERROR:
        return "暂时无法播放";
    case TOOLS_RADIO_STOPPED:
    default:
        return "已暂停";
    }
}

static void render_header(lv_obj_t *root, uint32_t now)
{
    char clock[16];
    format_clock(now, clock, sizeof(clock));
    label(root, "TOOLS//", 12, 8, 78, UI_GREEN);
    lv_obj_t *clock_label = label(root, clock, 88, 8, 70, UI_TEXT);
    lv_obj_set_style_text_font(clock_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(clock_label, LV_TEXT_ALIGN_CENTER, 0);
    render_battery_pill(root, 176, 8, UI_GREEN);
}

static void render_clock_only(lv_obj_t *root, uint32_t now)
{
    char clock[16];
    format_clock(now, clock, sizeof(clock));
    label(root, "REC//", 12, 8, 70, UI_RED);
    lv_obj_t *clock_label = label(root, clock, 88, 8, 70, UI_TEXT);
    lv_obj_set_style_text_font(clock_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(clock_label, LV_TEXT_ALIGN_CENTER, 0);
}

static void render_home(lv_obj_t *root, uint32_t now)
{
    render_section_mark(root, 14, 35, UI_GREEN);
    label(root, "小工具", 56, 30, 112, UI_TEXT);
    if (s_model.storage_warning || s_model.dirty) {
        label(root, "待保存", 168, 32, 58, UI_AMBER);
    }

    tools_wifi_status_t wifi = tools_wifi_status();
    tools_weather_status_t weather = tools_weather_status();
    char line[80];
    snprintf(line, sizeof(line), "%s%s", weather.summary,
             weather.updating ? " 更新中" : "");
    label_one_line(root, line, 14, 58, 212, weather.valid ? UI_GREEN : UI_MUTED);

    snprintf(line, sizeof(line), "时间 %s  Wi-Fi %s",
             wifi.time_synced ? "网络" :
             (s_clock_persistence_enabled ? "离线" : "未同步"),
             wifi_status_text(wifi));
    label_one_line(root, line, 14, 80, 212, wifi.time_synced ? UI_GREEN : UI_AMBER);

    smoking_totals_t today = smoking_model_today(&s_model, today_day());
    panel(root, 12, 103, 216, 88, UI_SURFACE, UI_LINE);
    snprintf(line, sizeof(line), "今日 %lu/%u 根",
             (unsigned long)today.cigarettes, smoking_model_daily_goal(&s_model));
    label_one_line(root, line, 24, 109, 188, UI_TEXT);
    snprintf(line, sizeof(line), "目标%u天  最好%u天",
             smoking_model_goal_streak(&s_model, today_day()),
             smoking_model_goal_best_streak(&s_model, today_day()));
    label_one_line(root, line, 24, 130, 188, UI_MUTED);
    uint16_t goal = smoking_model_daily_goal(&s_model);
    uint32_t capped = today.cigarettes > goal ? goal : today.cigarettes;
    int fill_w = goal ? (int)(capped * 68u / goal) : 0;
    pill(root, 24, 151, 68, 8, 0x0B1713, UI_LINE);
    if (fill_w > 0) pill(root, 24, 151, fill_w, 8, UI_GREEN, 0);
    uint32_t interval = smoking_model_last_interval_sec(&s_model, model_time_sec(now));
    if (interval) {
        char dur[24];
        format_duration(interval, dur, sizeof(dur));
        snprintf(line, sizeof(line), "上次间隔 %s", dur);
    } else {
        snprintf(line, sizeof(line), "上次还没有记录");
    }
    label_one_line(root, line, 24, 166, 188, UI_MUTED);

    static const smoking_feature_t feature_order[] = {
        SMOKING_FEATURE_SMOKE,
        SMOKING_FEATURE_FOCUS,
        SMOKING_FEATURE_RADIO,
        SMOKING_FEATURE_SETTINGS,
    };
    smoking_feature_t selected = smoking_model_selected_feature(&s_model);
    for (int i = 0; i < (int)(sizeof(feature_order) / sizeof(feature_order[0])); i++) {
        smoking_feature_t feature = feature_order[i];
        bool is_selected = selected == feature;
        int y = 196 + i * 24;
        uint32_t bg = is_selected ? UI_SURFACE_3 : UI_SURFACE;
        uint32_t bd = is_selected ? UI_GREEN : UI_LINE;
        panel(root, 12, y, 216, 23, bg, bd);
        if (is_selected) panel(root, 18, y + 5, 3, 13, UI_GREEN, 0);
        snprintf(line, sizeof(line), "APP %02d", i + 1);
        label_one_line(root, line, 28, y + 1, 66, is_selected ? UI_GREEN : UI_DIM);
        label_one_line(root, smoking_feature_name(feature), 100, y + 1, 86,
                       is_selected ? UI_TEXT : UI_MUTED);
        lv_obj_t *arrow = label_one_line(root, is_selected ? ">" : "//", 196, y + 1, 24,
                                         is_selected ? UI_GREEN : UI_DIM);
        lv_obj_set_style_text_align(arrow, LV_TEXT_ALIGN_RIGHT, 0);
    }

    if (selected == SMOKING_FEATURE_FOCUS) {
        uint32_t remaining = smoking_model_focus_remaining(&s_model, now);
        char dur[24];
        format_duration(remaining, dur, sizeof(dur));
        snprintf(line, sizeof(line), "%s %s",
                 smoking_model_focus_mode_name(&s_model), dur);
        label_one_line(root, line, 14, 294, 120, UI_MUTED);
        lv_obj_t *hint = label_one_line(root, "OK 进入", 132, 294, 94, UI_GREEN);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_RIGHT, 0);
    } else if (selected == SMOKING_FEATURE_SETTINGS) {
        label_one_line(root, "OK 进入设置", 14, 294, 212, UI_GREEN);
    } else if (selected == SMOKING_FEATURE_RADIO) {
        label_one_line(root, "OK 打开收音机", 14, 294, 212, UI_GREEN);
    } else {
        label_one_line(root, "OK 开始抽烟记录", 14, 294, 212, UI_GREEN);
    }
}

static lv_obj_t *radio_box(lv_obj_t *parent, int x, int y, int w, int h,
                           uint32_t color, int radius)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    return obj;
}

static void render_radio_header(lv_obj_t *root, uint32_t now)
{
    char clock[16];
    format_clock(now, clock, sizeof(clock));
    label(root, "RADIO//", 12, 10, 74, UI_AMBER);
    lv_obj_t *clock_label = label(root, clock, 88, 8, 70, UI_TEXT);
    lv_obj_set_style_text_font(clock_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(clock_label, LV_TEXT_ALIGN_RIGHT, 0);
    render_battery_pill(root, 176, 10, UI_AMBER);
}

static void render_radio(lv_obj_t *root, uint32_t now)
{
    lv_obj_set_style_bg_color(root, lv_color_hex(UI_BG), 0);
    render_radio_header(root, now);

    tools_wifi_status_t wifi = tools_wifi_status();
    tools_radio_set_network_ready(wifi_ready_for_radio(wifi));
    tools_radio_status_t radio = tools_radio_status();

    char line[96];
    snprintf(line, sizeof(line), "CH %02u / %02u",
             (unsigned)(radio.station_index + 1u),
             (unsigned)radio.station_count);
    label(root, line, 14, 48, 96, UI_AMBER);
    lv_obj_t *net = label(root, wifi_ready_for_radio(wifi) ? "在线" : "离线",
                          166, 48, 60, wifi_ready_for_radio(wifi) ? UI_GREEN : UI_DIM);
    lv_obj_set_style_text_align(net, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_t *name = label(root, radio.station_name, 14, 72, 212, UI_TEXT);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    label(root, radio.station_desc, 14, 106, 212, UI_MUTED);

    lv_obj_t *dial = radio_box(root, 14, 136, 212, 72, UI_SURFACE, 8);
    lv_obj_set_style_border_color(dial, lv_color_hex(UI_LINE), 0);
    lv_obj_set_style_border_width(dial, 1, 0);
    const int track_left = 16;
    const int track_right = 196;
    for (int i = 0; i <= 20; i += 2) {
        int x = track_left + i * (track_right - track_left) / 20;
        int h = (i % 4 == 0) ? 12 : 7;
        radio_box(dial, x, 34 - h, (i % 4 == 0) ? 2 : 1, h,
                  (i % 4 == 0) ? UI_LINE : 0x7F5A29, 1);
    }
    label(dial, "88", 5, 6, 30, UI_MUTED);
    label(dial, "96", 78, 6, 30, UI_MUTED);
    label(dial, "108", 172, 6, 34, UI_MUTED);
    radio_box(dial, track_left, 36, track_right - track_left + 1, 2, UI_LINE, 1);
    size_t count = radio.station_count ? radio.station_count : 1;
    int needle = track_left + (int)((track_right - track_left) *
                 (count > 1 ? radio.station_index : 0) / (count > 1 ? count - 1 : 1));
    if (count > 1) {
        int inset = (track_right - track_left) / 8;
        needle = track_left + inset +
                 (int)(((track_right - track_left) - inset * 2) *
                       radio.station_index / (count - 1));
    }
    lv_obj_t *glow = radio_box(dial, needle - 9, 34, 18, 8, UI_AMBER, 4);
    lv_obj_set_style_opa(glow, LV_OPA_20, 0);
    radio_box(dial, needle - 1, 28, 3, 22, UI_AMBER, 1);

    lv_obj_t *meter = radio_box(root, 14, 218, 212, 42, 0x11262E, 7);
    uint8_t seed = (uint8_t)(now + radio.station_index * 17u);
    for (int i = 0; i < 18; i++) {
        bool moving = radio.state == TOOLS_RADIO_PLAYING ||
                      radio.state == TOOLS_RADIO_BUFFERING;
        int h = moving ? 4 + ((seed + i * 7) % 25) : 4;
        radio_box(meter, 8 + i * 11, 34 - h, 6, h, UI_AMBER, 2);
    }

    bool active = radio.state == TOOLS_RADIO_CONNECTING ||
                  radio.state == TOOLS_RADIO_BUFFERING ||
                  radio.state == TOOLS_RADIO_PLAYING ||
                  radio.state == TOOLS_RADIO_RECONNECTING;
    lv_obj_t *icon = label(root, active ? "II" : ">", 16, 269, 26,
                           radio.state == TOOLS_RADIO_ERROR ? UI_RED : UI_AMBER);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_20, 0);
    const char *detail = radio.detail[0] ? radio.detail : radio_state_text(radio.state);
    snprintf(line, sizeof(line), "%s  音量%u%%", detail, radio.volume);
    label_one_line(root, line, 44, 272, 182, UI_TEXT);
    label(root, "UP/DOWN 换台  OK 播放", 14, 294, 212, UI_MUTED);
}

static void render_focus(lv_obj_t *root, uint32_t now)
{
    render_section_mark(root, 14, 42, UI_BLUE);
    label(root, "番茄钟", 56, 38, 100, UI_TEXT);
    lv_obj_t *mode = label(root, smoking_model_focus_mode_name(&s_model), 154, 38, 72, UI_BLUE);
    lv_obj_set_style_text_align(mode, LV_TEXT_ALIGN_RIGHT, 0);

    uint32_t remaining = smoking_model_focus_remaining(&s_model, now);
    char dur[24];
    format_duration(remaining, dur, sizeof(dur));
    panel(root, 18, 84, 204, 96, UI_SURFACE, UI_LINE);
    panel(root, 28, 94, 184, 4, s_model.focus_running ? UI_BLUE : UI_LINE, 0);
    lv_obj_t *timer = label(root, dur, 22, 118, 196, UI_TEXT);
    lv_obj_set_style_text_font(timer, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(timer, LV_TEXT_ALIGN_CENTER, 0);

    panel(root, 24, 202, 192, 34, s_model.focus_running ? 0x132A3A : UI_SURFACE, UI_BLUE);
    lv_obj_t *action = label(root, s_model.focus_running ? "OK 暂停" : "OK 开始",
                             42, 208, 156, s_model.focus_running ? UI_AMBER : UI_GREEN);
    lv_obj_set_style_text_align(action, LV_TEXT_ALIGN_CENTER, 0);
    label(root, "UP/DOWN 切换模式", 42, 250, 170, UI_MUTED);
    label(root, "长按返回", 76, 276, 100, UI_DIM);
}

static void render_smoking_active(lv_obj_t *root, uint32_t now)
{
    render_section_mark(root, 14, 42, UI_RED);
    label(root, "抽烟中", 56, 38, 120, UI_TEXT);
    char dur[24];
    uint32_t model_now = model_time_sec(now);
    format_duration(model_now > s_model.smoking_started_at ?
                    model_now - s_model.smoking_started_at : 0, dur, sizeof(dur));
    panel(root, 18, 86, 204, 104, 0x221413, 0x5A2523);
    panel(root, 28, 96, 184, 4, UI_RED, 0);
    lv_obj_t *timer = label(root, dur, 22, 122, 196, UI_TEXT);
    lv_obj_set_style_text_font(timer, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(timer, LV_TEXT_ALIGN_CENTER, 0);
    panel(root, 34, 218, 172, 34, 0x241918, UI_RED);
    lv_obj_t *hint = label(root, "长按结束抽烟", 52, 224, 136, UI_AMBER);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
}

static void render_count_select(lv_obj_t *root)
{
    render_section_mark(root, 14, 42, UI_GREEN);
    label(root, "记录根数", 56, 38, 140, UI_TEXT);
    char dur[24];
    char line[48];
    format_duration(s_model.pending_duration_sec, dur, sizeof(dur));
    snprintf(line, sizeof(line), "持续 %s", dur);
    label(root, line, 24, 78, 180, UI_MUTED);
    snprintf(line, sizeof(line), "%u 根", s_model.pending_cigarettes);
    panel(root, 42, 112, 156, 78, UI_SURFACE, UI_GREEN);
    lv_obj_t *count = label(root, line, 64, 136, 112, UI_GREEN);
    lv_obj_set_style_text_align(count, LV_TEXT_ALIGN_CENTER, 0);
    if (s_count_wait_ok_release) {
        label(root, "选择根数", 76, 218, 100, UI_AMBER);
    } else {
        label(root, "UP 减少  DOWN 增加", 24, 218, 190, UI_MUTED);
    }
    label(root, "OK 保存", 76, 246, 100, UI_GREEN);
}

static void render_settings(lv_obj_t *root)
{
    render_section_mark(root, 14, 35, UI_GREEN);
    label(root, "设置", 56, 31, 80, UI_TEXT);
    bool time_selected = s_model.selected_setting == SMOKING_SETTING_TIME;
    bool goal_selected = s_model.selected_setting == SMOKING_SETTING_GOAL;
    bool today_selected = s_model.selected_setting == SMOKING_SETTING_TODAY;
    bool wifi_selected = s_model.selected_setting == SMOKING_SETTING_WIFI;

    char time_value[24];
    snprintf(time_value, sizeof(time_value), "%02d:%02d", s_model.edit_hour,
             s_model.edit_minute);
    render_setting_row(root, 56, "时间", time_value,
                       s_model.editing_time ?
                       (s_model.edit_minute_selected ? "编辑分钟" : "编辑小时") :
                       "OK 编辑时间",
                       time_selected, s_model.editing_time);

    char goal_value[16];
    snprintf(goal_value, sizeof(goal_value), "%u 根",
             smoking_model_daily_goal(&s_model));
    render_setting_row(root, 106, "目标", goal_value,
                       s_model.editing_goal ? "UP/DOWN 调整" : "OK 编辑目标",
                       goal_selected, s_model.editing_goal);

    char today_value[16];
    snprintf(today_value, sizeof(today_value), "%lu 根",
             (unsigned long)smoking_model_today_cigarettes(&s_model,
                                                           s_model.edit_today_day));
    render_setting_row(root, 156, "今日已抽", today_value,
                       s_model.editing_today ? "UP/DOWN 调整" : "OK 编辑今日",
                       today_selected, s_model.editing_today);

    tools_wifi_status_t wifi = tools_wifi_status();
    render_setting_row(root, 206, "Wi-Fi", wifi_status_text(wifi),
                       wifi_hint_text(wifi), wifi_selected, wifi.provisioning);

    if (s_model.editing_time) {
        label_one_line(root, "UP/DOWN 调整", 20, 264, 200, UI_MUTED);
        label_one_line(root, "OK 切字段  长按保存", 20, 288, 200, UI_GREEN);
    } else if (s_model.editing_goal) {
        label_one_line(root, "UP 减少  DOWN 增加", 20, 264, 200, UI_MUTED);
        label_one_line(root, "OK 返回  长按返回", 20, 288, 200, UI_GREEN);
    } else if (s_model.editing_today) {
        label_one_line(root, "UP 减少  DOWN 增加", 20, 264, 200, UI_MUTED);
        label_one_line(root, "OK 返回  长按返回", 20, 288, 200, UI_GREEN);
    } else if (wifi_selected) {
        label_one_line(root, TOOLS_WIFI_SETUP_SSID, 20, 264, 200, UI_MUTED);
        label_one_line(root, "长按清除Wi-Fi", 20, 288, 200, UI_AMBER);
    } else {
        label_one_line(root, "UP/DOWN 选择", 20, 264, 200, UI_MUTED);
        label_one_line(root, "OK 进入  长按返回", 20, 288, 200, UI_GREEN);
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
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(UI_BG), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_clean(s_screen);

    if (s_model.persisted.screen == SMOKING_SCREEN_ACTIVE) {
        render_clock_only(s_screen, now);
    } else if (s_model.persisted.screen != SMOKING_SCREEN_RADIO) {
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
    case SMOKING_SCREEN_SETTINGS:
        render_settings(s_screen);
        break;
    case SMOKING_SCREEN_FOCUS:
        render_focus(s_screen, now);
        break;
    case SMOKING_SCREEN_RADIO:
        render_radio(s_screen, now);
        break;
    default:
        smoking_model_return_home(&s_model);
        render_home(s_screen, now);
        break;
    }
}

static void enter_radio_screen(void)
{
    smoking_model_enter_radio(&s_model);
    apply_active_backlight(now_sec());
    esp_err_t wifi_err = tools_wifi_reconnect();
    if (wifi_err != ESP_OK) {
        ESP_LOGW(TAG, "收音机启动 Wi-Fi 失败: %s", esp_err_to_name(wifi_err));
    }
    esp_err_t radio_err = tools_radio_init();
    if (radio_err != ESP_OK) {
        ESP_LOGW(TAG, "收音机初始化失败: %s", esp_err_to_name(radio_err));
    }
    tools_wifi_status_t wifi = tools_wifi_status();
    tools_radio_set_network_ready(wifi_ready_for_radio(wifi));
}

static void leave_radio_screen(void)
{
    tools_radio_stop();
    log_deep_sleep_warning("radio Wi-Fi stop", tools_wifi_stop_all());
    smoking_model_return_home(&s_model);
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
    esp_err_t err = smoking_store_save(&snapshot);
    if (err == ESP_OK) {
        s_model.dirty = false;
    } else {
        ESP_LOGW(TAG, "保存抽烟状态失败: %s", esp_err_to_name(err));
        s_model.storage_warning = true;
    }
}

static void store_status(esp_err_t result, void *user)
{
    (void)user;
    s_model.storage_warning = result != ESP_OK;
}

static void restore_clock_if_available(void)
{
    uint32_t saved = 0;
    bool loaded = false;
    esp_err_t err = smoking_store_load_clock(&saved, &loaded);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "读取离线时间失败: %s", esp_err_to_name(err));
        s_model.storage_warning = true;
        return;
    }
    s_clock_persistence_enabled = true;
    uint32_t restored = loaded ? saved : CLOCK_BOOTSTRAP_UNIX_SEC;
    if (loaded && saved < CLOCK_BOOTSTRAP_UNIX_SEC) {
        restored = CLOCK_BOOTSTRAP_UNIX_SEC;
        ESP_LOGI(TAG, "offline clock behind firmware time, advance to bootstrap");
    }
    if (s_deep_sleep_magic == DEEP_SLEEP_MAGIC &&
        s_deep_sleep_clock_sec > 0 && s_deep_sleep_rtc_us > 0) {
        uint64_t rtc_now = s_boot_rtc_us ? s_boot_rtc_us : esp_rtc_get_time_us();
        if (rtc_now > s_deep_sleep_rtc_us) {
            uint64_t elapsed = (rtc_now - s_deep_sleep_rtc_us) / 1000000ULL;
            if (elapsed > 0 && elapsed <= CLOCK_DEEP_SLEEP_ELAPSED_MAX_SEC) {
                uint64_t candidate = (uint64_t)s_deep_sleep_clock_sec + elapsed;
                if (candidate <= UINT32_MAX && candidate > restored) {
                    restored = (uint32_t)candidate;
                    ESP_LOGI(TAG, "deep sleep offline clock +%llu sec",
                             (unsigned long long)elapsed);
                }
            }
        }
    }
    if (now_sec() >= restored) return;

    const struct timeval tv = {
        .tv_sec = (time_t)restored,
        .tv_usec = 0,
    };
    if (settimeofday(&tv, NULL) == 0) {
        ESP_LOGI(TAG, "已恢复离线时间: %lu", (unsigned long)restored);
        if (!loaded || restored != saved) {
            esp_err_t save_err = smoking_store_save_clock(restored);
            if (save_err == ESP_OK) {
                s_last_clock_save_sec = restored;
            } else {
                ESP_LOGW(TAG, "save adjusted offline clock failed: %s",
                         esp_err_to_name(save_err));
                s_model.storage_warning = true;
            }
        }
    } else {
        ESP_LOGW(TAG, "恢复离线时间失败");
        s_model.storage_warning = true;
    }
}

static void save_clock_if_due(bool force)
{
    if (force) {
        s_clock_persistence_enabled = true;
    }
    if (!s_clock_persistence_enabled) return;

    uint32_t now = now_sec();
    if (!force && s_last_clock_save_sec &&
        now - s_last_clock_save_sec < CLOCK_SAVE_INTERVAL_SEC) {
        return;
    }
    esp_err_t err = smoking_store_save_clock(now);
    if (err == ESP_OK) {
        s_last_clock_save_sec = now;
    } else {
        ESP_LOGW(TAG, "保存离线时间失败: %s", esp_err_to_name(err));
        s_model.storage_warning = true;
    }
}

static void log_power_if_due(bool force)
{
    if (!s_battery_ok) return;
    uint32_t now = now_sec();
    if (!force && s_last_power_log_sec &&
        now - s_last_power_log_sec < POWER_LOG_INTERVAL_SEC) {
        return;
    }

    tools_wifi_status_t wifi = tools_wifi_status();
    tools_radio_status_t radio = tools_radio_status();
    uint8_t flags = 0;
    if (s_backlight_on) flags |= POWER_FLAG_BACKLIGHT;
    if (wifi.state != TOOLS_WIFI_OFF) flags |= POWER_FLAG_WIFI;
    if (wifi.provisioning) flags |= POWER_FLAG_WIFI_PROVISIONING;
    if (radio.wanted_playing) flags |= POWER_FLAG_RADIO_WANTED;
    if (radio.state == TOOLS_RADIO_PLAYING ||
        radio.state == TOOLS_RADIO_CONNECTING ||
        radio.state == TOOLS_RADIO_BUFFERING ||
        radio.state == TOOLS_RADIO_RECONNECTING) {
        flags |= POWER_FLAG_RADIO_ACTIVE;
    }

    int soc = filtered_battery_soc();
    int mv = bsp_battery_mv();
    smoking_power_sample_t sample = {
        .time_sec = now,
        .voltage_mv = (int16_t)(mv < 0 ? -1 : mv),
        .soc = (int8_t)(soc < 0 ? -1 : soc),
        .screen = s_model.persisted.screen,
        .flags = flags,
    };
    esp_err_t err = smoking_store_append_power_sample(&sample);
    if (err == ESP_OK) {
        s_last_power_log_sec = now;
    } else {
        ESP_LOGW(TAG, "保存电量记录失败: %s", esp_err_to_name(err));
    }
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
        save_clock_if_due(true);
    }
    smoking_model_return_home(&s_model);
    s_last_input_sec = now_sec();
    set_backlight_percent(desired_backlight_percent(s_last_input_sec));
}

static void toggle_wifi_provisioning(void)
{
    tools_wifi_status_t status = tools_wifi_status();
    if (status.provisioning) {
        tools_wifi_stop_provisioning();
        return;
    }

    bool connected = status.state == TOOLS_WIFI_CONNECTED ||
                     status.state == TOOLS_WIFI_TIME_SYNCED;
    esp_err_t err = ESP_OK;
    if (!connected && status.state != TOOLS_WIFI_CONNECTING) {
        err = tools_wifi_reconnect();
        if (err == ESP_OK) {
            status = tools_wifi_status();
            if (status.state == TOOLS_WIFI_CONNECTING ||
                status.state == TOOLS_WIFI_CONNECTED ||
                status.state == TOOLS_WIFI_TIME_SYNCED) {
                return;
            }
        } else {
            ESP_LOGW(TAG, "重连已保存 Wi-Fi 失败: %s", esp_err_to_name(err));
        }
    }

    err = tools_wifi_start_provisioning();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "启动 Wi-Fi 配网失败: %s", esp_err_to_name(err));
    }
}

static void ensure_wifi_settings_connection(void)
{
    tools_wifi_status_t status = tools_wifi_status();
    if (status.provisioning ||
        status.state == TOOLS_WIFI_CONNECTING ||
        status.state == TOOLS_WIFI_CONNECTED ||
        status.state == TOOLS_WIFI_TIME_SYNCED) {
        return;
    }
    esp_err_t err = tools_wifi_reconnect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "设置页 Wi-Fi 自动重连失败: %s", esp_err_to_name(err));
    }
}

static void clear_wifi_credentials(void)
{
    tools_wifi_stop_provisioning();
    esp_err_t err = tools_wifi_clear_credentials();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "清除 Wi-Fi 凭据失败: %s", esp_err_to_name(err));
    }
    (void)tools_wifi_stop_all();
}

static void log_deep_sleep_warning(const char *step, esp_err_t err)
{
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "deep sleep 继续：%s 失败: %s", step, esp_err_to_name(err));
    }
}

static bool deep_sleep_allowed(void)
{
    if (s_entering_deep_sleep) return false;
    if (s_model.focus_running) return false;
    tools_radio_status_t radio = tools_radio_status();
    if (radio.wanted_playing || radio.state == TOOLS_RADIO_PLAYING ||
        radio.state == TOOLS_RADIO_CONNECTING ||
        radio.state == TOOLS_RADIO_BUFFERING ||
        radio.state == TOOLS_RADIO_RECONNECTING) {
        return false;
    }
    tools_wifi_status_t wifi = tools_wifi_status();
    return !wifi.provisioning;
}

static uint32_t deep_sleep_idle_sec(void)
{
    if (s_manual_screen_off) return SCREEN_OFF_DEEP_SLEEP_SEC;
    if (!s_backlight_on) return IDLE_BACKLIGHT_SEC + SCREEN_OFF_DEEP_SLEEP_SEC;
    return IDLE_DEEP_SLEEP_SEC;
}

static void restore_button_after_sleep_abort(void)
{
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    s_input_ready = false;
    esp_err_t err = bsp_button_init(on_key, NULL);
    if (err == ESP_OK) {
        s_input_ready = true;
    } else {
        ESP_LOGE(TAG, "恢复按键失败: %s", esp_err_to_name(err));
    }
    s_entering_deep_sleep = false;
    s_last_input_sec = now_sec();
}

static void enter_idle_deep_sleep(void)
{
    if (!deep_sleep_allowed()) return;
    s_entering_deep_sleep = true;
    uint32_t sleep_clock = now_sec();
    uint32_t idle_sec = deep_sleep_idle_sec();
    ESP_LOGI(TAG, "空闲 %u 秒，进入 deep sleep", idle_sec);

    if (s_model.persisted.screen == SMOKING_SCREEN_COUNT_SELECT) {
        smoking_model_save_pending(&s_model, model_time_sec(sleep_clock));
    }
    queue_save_if_dirty();
    save_clock_if_due(true);
    log_power_if_due(true);

    int button_level = 1;
    esp_err_t err = bsp_button_prepare_deep_sleep(&button_level);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "按键 deep-sleep 唤醒准备失败: %s", esp_err_to_name(err));
        restore_button_after_sleep_abort();
        return;
    }
    if (button_level == 0) {
        ESP_LOGI(TAG, "按键仍按下，延后 deep sleep");
        restore_button_after_sleep_abort();
        return;
    }

    err = esp_deep_sleep_enable_gpio_wakeup(1ULL << BSP_BTN_GPIO,
                                           ESP_GPIO_WAKEUP_GPIO_LOW);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GPIO 唤醒配置失败: %s", esp_err_to_name(err));
        restore_button_after_sleep_abort();
        return;
    }
#if IDLE_SLEEP_TIMER_WAKE_SEC > 0
    err = esp_sleep_enable_timer_wakeup((uint64_t)IDLE_SLEEP_TIMER_WAKE_SEC * 1000000ULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "RTC 定时兜底唤醒配置失败: %s", esp_err_to_name(err));
    }
#endif

    log_deep_sleep_warning("weather stop", tools_weather_stop());
    tools_radio_stop();
    log_deep_sleep_warning("Wi-Fi stop", tools_wifi_stop_all());

    s_deep_sleep_clock_sec = sleep_clock;
    s_deep_sleep_rtc_us = esp_rtc_get_time_us();
    if (s_battery_ok) {
        int soc = filtered_battery_soc();
        if (soc > 0) s_deep_sleep_battery_soc = soc;
    }
    log_deep_sleep_warning("CW2017 suspend", bsp_battery_sleep());
    log_deep_sleep_warning("ES8311 suspend", bsp_audio_sleep());
    log_deep_sleep_warning("I2S pin release", bsp_audio_prepare_deep_sleep());
    log_deep_sleep_warning("shared I2C pin release", bsp_i2c_prepare_deep_sleep());

    if (!bsp_lvgl_lock(1000)) {
        ESP_LOGE(TAG, "deep sleep 前无法停止 LVGL 刷屏，重启恢复外设");
        esp_restart();
    }
    log_deep_sleep_warning("ST7789 suspend", bsp_display_prepare_deep_sleep());
    if (s_deep_sleep_magic != DEEP_SLEEP_MAGIC) s_deep_sleep_count = 0;
    s_deep_sleep_magic = DEEP_SLEEP_MAGIC;
    s_deep_sleep_count++;
    esp_deep_sleep_start();
    ESP_LOGE(TAG, "esp_deep_sleep_start 意外返回，重启恢复外设");
    esp_restart();
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
        if (s_manual_screen_off && input->event != BSP_BTN_PRESS) {
            s_ignore_wake_tail = true;
            return;
        }
        s_manual_screen_off = false;
        set_backlight_percent(desired_backlight_percent(now));
        s_ignore_wake_tail = input->event == BSP_BTN_PRESS;
        s_last_input_sec = now;
        request_render();
        return;
    }
    if (s_ignore_wake_tail) {
        if (input->event == BSP_BTN_CLICK ||
            input->event == BSP_BTN_DOUBLE ||
            input->event == BSP_BTN_LONG) {
            s_ignore_wake_tail = false;
            s_last_input_sec = now;
            request_render();
            return;
        }
        s_last_input_sec = now;
        request_render();
        return;
    }
    s_last_input_sec = now;

    smoking_screen_t screen = (smoking_screen_t)s_model.persisted.screen;
    if (screen == SMOKING_SCREEN_HOME) {
        if (input->event == BSP_BTN_LONG && input->btn == BSP_BTN_OK) {
            s_manual_screen_off = true;
            set_backlight_percent(0);
            request_render();
            return;
        } else if (input->event == BSP_BTN_CLICK && input->btn == BSP_BTN_UP) {
            smoking_model_home_prev(&s_model);
        } else if (input->event == BSP_BTN_CLICK && input->btn == BSP_BTN_DOWN) {
            smoking_model_home_next(&s_model);
        } else if (input->event == BSP_BTN_CLICK && input->btn == BSP_BTN_OK) {
            if (smoking_model_selected_feature(&s_model) == SMOKING_FEATURE_SETTINGS) {
                smoking_model_enter_time_settings(&s_model, model_time_sec(now));
                if (s_model.selected_setting == SMOKING_SETTING_WIFI) {
                    ensure_wifi_settings_connection();
                }
                apply_active_backlight(now);
            } else if (smoking_model_selected_feature(&s_model) == SMOKING_FEATURE_RADIO) {
                enter_radio_screen();
            } else {
                smoking_model_enter_selected(&s_model, model_time_sec(now));
                apply_active_backlight(now);
            }
        }
    } else if (screen == SMOKING_SCREEN_ACTIVE) {
        if (input->event == BSP_BTN_LONG && input->btn == BSP_BTN_OK) {
            smoking_model_finish(&s_model, model_time_sec(now));
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
            smoking_model_save_pending(&s_model, model_time_sec(now));
            queue_save_if_dirty();
            beep(AUDIO_BEEP_DONE);
        }
    } else if (screen == SMOKING_SCREEN_SETTINGS) {
        if (input->event == BSP_BTN_LONG && input->btn == BSP_BTN_OK) {
            if (s_model.editing_time) {
                save_time_and_return_home();
            } else if (!s_model.editing_goal && !s_model.editing_today &&
                       s_model.selected_setting == SMOKING_SETTING_WIFI) {
                clear_wifi_credentials();
                beep(AUDIO_BEEP_DONE);
            } else {
                if (s_model.selected_setting == SMOKING_SETTING_WIFI) {
                    log_deep_sleep_warning("settings Wi-Fi stop", tools_wifi_stop_all());
                }
                smoking_model_return_home(&s_model);
                queue_save_if_dirty();
            }
        } else if (input->event == BSP_BTN_CLICK && input->btn == BSP_BTN_OK) {
            if (!s_model.editing_time && !s_model.editing_goal && !s_model.editing_today &&
                s_model.selected_setting == SMOKING_SETTING_WIFI) {
                toggle_wifi_provisioning();
            } else {
                smoking_model_settings_click(&s_model);
                queue_save_if_dirty();
            }
        } else if (input->event == BSP_BTN_CLICK && input->btn == BSP_BTN_UP) {
            smoking_model_settings_up(&s_model);
            if (s_model.selected_setting == SMOKING_SETTING_WIFI) {
                ensure_wifi_settings_connection();
            }
            queue_save_if_dirty();
        } else if (input->event == BSP_BTN_CLICK && input->btn == BSP_BTN_DOWN) {
            smoking_model_settings_down(&s_model);
            if (s_model.selected_setting == SMOKING_SETTING_WIFI) {
                ensure_wifi_settings_connection();
            }
            queue_save_if_dirty();
        }
    } else if (screen == SMOKING_SCREEN_FOCUS) {
        if (input->event == BSP_BTN_LONG && input->btn == BSP_BTN_OK) {
            smoking_model_return_home(&s_model);
        } else if (input->event == BSP_BTN_CLICK && input->btn == BSP_BTN_OK) {
            smoking_model_focus_click(&s_model, now);
        } else if (input->event == BSP_BTN_CLICK &&
                   (input->btn == BSP_BTN_UP || input->btn == BSP_BTN_DOWN)) {
            smoking_model_focus_toggle_mode(&s_model);
        }
    } else if (screen == SMOKING_SCREEN_RADIO) {
        if (input->event == BSP_BTN_LONG && input->btn == BSP_BTN_OK) {
            leave_radio_screen();
        } else if (input->event == BSP_BTN_CLICK && input->btn == BSP_BTN_OK) {
            tools_radio_toggle();
        } else if (input->event == BSP_BTN_CLICK && input->btn == BSP_BTN_UP) {
            tools_radio_prev();
        } else if (input->event == BSP_BTN_CLICK && input->btn == BSP_BTN_DOWN) {
            tools_radio_next();
        } else if (input->event == BSP_BTN_LONG && input->btn == BSP_BTN_UP) {
            tools_radio_volume_down();
        } else if (input->event == BSP_BTN_LONG && input->btn == BSP_BTN_DOWN) {
            tools_radio_volume_up();
        }
    }
    apply_active_backlight(now);
    request_render();
}

static void input_task(void *arg)
{
    (void)arg;
    input_event_t input;
    TickType_t last_refresh = xTaskGetTickCount();
    bool weather_network_ready = false;
    for (;;) {
        if (xQueueReceive(s_input_queue, &input, pdMS_TO_TICKS(UI_REFRESH_MS)) == pdTRUE) {
            process_input(&input);
        }
        uint32_t now = now_sec();
        apply_active_backlight(now);
        if (s_backlight_on && smoking_model_idle_backlight_allowed(&s_model) &&
            now - s_last_input_sec >= IDLE_BACKLIGHT_SEC) {
            set_backlight_percent(0);
        }
        if (deep_sleep_allowed() && now - s_last_input_sec >= deep_sleep_idle_sec()) {
            enter_idle_deep_sleep();
        }
        TickType_t tick_now = xTaskGetTickCount();
        if (tick_now - last_refresh >= pdMS_TO_TICKS(1000)) {
            if (tools_wifi_take_time_synced()) {
                save_clock_if_due(true);
                tools_weather_request_refresh();
            }
            tools_wifi_status_t wifi = tools_wifi_status();
            tools_radio_set_network_ready(wifi_ready_for_radio(wifi));
            bool network_ready = wifi_ready_for_radio(wifi);
            if (network_ready && !weather_network_ready) {
                esp_err_t weather_err = tools_weather_init();
                if (weather_err == ESP_OK) {
                    tools_weather_request_refresh();
                } else {
                    ESP_LOGW(TAG, "weather refresh start failed: %s",
                             esp_err_to_name(weather_err));
                }
            }
            weather_network_ready = network_ready;
            if (smoking_model_focus_tick(&s_model, now)) {
                beep(AUDIO_BEEP_DONE);
            }
            if (s_backlight_on) request_render();
            queue_save_if_dirty();
            save_clock_if_due(false);
            log_power_if_due(false);
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

static void input_dispatch_deinit(void) {
    s_input_ready = false;
    if (s_input_task) {
        vTaskDelete(s_input_task);
        s_input_task = NULL;
    }
    if (s_input_queue) {
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
    }
}

static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    // Consume the guard before the readiness check: a release can arrive after the
    // post-init voltage sample but before input dispatch is ready, and dropping it
    // here would leave the guard armed to swallow the next real press.
    const bool was_armed = s_key_guard.armed;
    if (key_guard_consume(&s_key_guard, ev == BSP_BTN_RELEASE, esp_timer_get_time())) {
        // The release and the deadline both arrive on this task; log the disarm
        // transition once it happens.
        if (was_armed && !s_key_guard.armed) {
            ESP_LOGI(TAG, "唤醒键已松手,按键恢复响应");
        }
        return;
    }
    if (!s_input_ready || !s_input_queue) return;
    const input_event_t input = { .btn = btn, .event = ev };
    (void)xQueueSend(s_input_queue, &input, 0);
}

void app_main(void)
{
    s_boot_rtc_us = esp_rtc_get_time_us();
    ESP_LOGI(TAG, "AI Passport 小工具启动");
    const uint32_t wake_causes = esp_sleep_get_wakeup_causes();
    if (wake_causes & ~(1u << ESP_SLEEP_WAKEUP_UNDEFINED)) {
        ESP_LOGI(TAG, "休眠唤醒原因位图: 0x%02x", (unsigned)wake_causes);
    }
    if (s_deep_sleep_magic == DEEP_SLEEP_MAGIC) {
        ESP_LOGI(TAG, "deep sleep 唤醒: cause=%d count=%lu",
                 (int)esp_sleep_get_wakeup_cause(),
                 (unsigned long)s_deep_sleep_count);
        s_battery_zero_filter_until_sec =
            CLOCK_BOOTSTRAP_UNIX_SEC + BATTERY_WAKE_ZERO_FILTER_SEC;
    }
    if (wake_causes & (1u << ESP_SLEEP_WAKEUP_GPIO)) {
        key_guard_arm(&s_key_guard, esp_timer_get_time(), KEY_GUARD_MAX_US);
        ESP_LOGI(TAG, "按键唤醒:先屏蔽按键动作,直到那颗键松手");
    }
    init_timezone();
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
    set_backlight_percent(BACKLIGHT_NORMAL_PERCENT);
    s_last_input_sec = now_sec();

    s_battery_ok = bsp_battery_init() == ESP_OK;
    s_audio_ok = bsp_audio_init() == ESP_OK;
    s_audio_queue = xQueueCreate(AUDIO_QUEUE_DEPTH, sizeof(audio_event_t));
    if (s_audio_queue) {
        (void)xTaskCreate(audio_task, "tools_audio", 3072, NULL, 3, NULL);
    }

    if (nvs_err == ESP_OK) {
        esp_err_t store_partition_err = smoking_store_partition_init();
        if (store_partition_err != ESP_OK) {
            ESP_LOGW(TAG, "小工具数据分区初始化失败: %s",
                     esp_err_to_name(store_partition_err));
            s_model.storage_warning = true;
        }
        smoking_persist_state_t restored;
        bool loaded = false;
        if (store_partition_err == ESP_OK) {
            esp_err_t load_err = smoking_store_load(&restored, &loaded);
            if (load_err == ESP_OK && loaded) {
                smoking_model_restore(&s_model, &restored);
            } else if (load_err != ESP_OK && load_err != ESP_ERR_INVALID_CRC) {
                ESP_LOGW(TAG, "读取抽烟状态失败: %s", esp_err_to_name(load_err));
                s_model.storage_warning = true;
            }
            restore_clock_if_available();
        }
        s_last_input_sec = now_sec();
        esp_err_t store_err = smoking_store_init(store_status, NULL);
        if (store_err != ESP_OK) {
            ESP_LOGW(TAG, "保存任务启动失败: %s", esp_err_to_name(store_err));
            s_model.storage_warning = true;
        }
    }
    if (s_deep_sleep_magic == DEEP_SLEEP_MAGIC) {
        uint32_t filter_until = now_sec() + BATTERY_WAKE_ZERO_FILTER_SEC;
        if (filter_until > s_battery_zero_filter_until_sec) {
            s_battery_zero_filter_until_sec = filter_until;
        }
    }

    esp_err_t input_err = input_dispatch_init();
    esp_err_t button_err = input_err == ESP_OK ?
        bsp_button_init(on_key, NULL) : ESP_ERR_INVALID_STATE;
    if (input_err != ESP_OK || button_err != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败: input=%s button=%s",
                 esp_err_to_name(input_err), esp_err_to_name(button_err));
        if (input_err == ESP_OK) {
            input_dispatch_deinit();
        }
    } else {
        if (s_key_guard.armed) {
            const bool released = bsp_button_read_mv() >= BSP_BTN_MV_RELEASED_MIN;
            key_guard_sync_released(&s_key_guard, released);
            if (!s_key_guard.armed) {
                ESP_LOGI(TAG, "唤醒键在按键驱动启动前已松手,提前撤防");
            }
        }
        s_input_ready = true;
    }

    request_render();
    log_power_if_due(true);
    ESP_LOGI(TAG, "就绪: Battery=%d Audio=%d NVS=%d",
             s_battery_ok, s_audio_ok, nvs_err == ESP_OK);
}
