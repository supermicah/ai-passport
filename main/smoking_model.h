#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SMOKING_MODEL_VERSION 1u
#define SMOKING_HISTORY_DAYS 7u
#define SMOKING_DEFAULT_DAILY_GOAL 20u
#define SMOKING_MIN_DAILY_GOAL 1u
#define SMOKING_MAX_DAILY_GOAL 20u
#define SMOKING_RESERVED_GAME_BYTES 28u
#define SMOKING_FOCUS_WORK_SEC (25u * 60u)
#define SMOKING_FOCUS_BREAK_SEC (5u * 60u)

typedef enum {
    SMOKING_SCREEN_HOME = 0,
    SMOKING_SCREEN_ACTIVE,
    SMOKING_SCREEN_COUNT_SELECT,
    SMOKING_SCREEN_SETTINGS = 4,
    SMOKING_SCREEN_FOCUS,
} smoking_screen_t;

typedef enum {
    SMOKING_FEATURE_SMOKE = 0,
    SMOKING_FEATURE_FOCUS,
    SMOKING_FEATURE_SETTINGS,
    SMOKING_FEATURE_COUNT,
} smoking_feature_t;

typedef enum {
    SMOKING_SETTING_TIME = 0,
    SMOKING_SETTING_GOAL,
    SMOKING_SETTING_WIFI,
    SMOKING_SETTING_COUNT,
} smoking_setting_t;

typedef enum {
    SMOKING_FOCUS_WORK = 0,
    SMOKING_FOCUS_BREAK,
} smoking_focus_mode_t;

typedef struct {
    int32_t day;
    uint32_t cigarettes;
    uint32_t duration_sec;
} smoking_day_stat_t;

typedef struct {
    int32_t day;
    uint32_t start_sec;
    uint32_t duration_sec;
    uint8_t cigarettes;
    uint8_t valid;
} smoking_record_t;

typedef struct {
    uint32_t version;
    uint8_t screen;
    uint8_t selected_feature;
    uint16_t daily_goal;
    uint32_t completed_count;
    smoking_day_stat_t days[SMOKING_HISTORY_DAYS];
    smoking_record_t last;
    uint8_t reserved_game[SMOKING_RESERVED_GAME_BYTES];
} smoking_persist_state_t;

typedef struct {
    smoking_persist_state_t persisted;
    uint32_t smoking_started_at;
    uint32_t pending_duration_sec;
    uint8_t pending_cigarettes;
    bool dirty;
    bool storage_warning;
    int edit_hour;
    int edit_minute;
    bool edit_minute_selected;
    uint8_t selected_setting;
    bool editing_time;
    bool editing_goal;
    uint8_t focus_mode;
    bool focus_running;
    uint32_t focus_started_at;
    uint32_t focus_remaining_sec;
} smoking_model_t;

typedef struct {
    uint32_t cigarettes;
    uint32_t duration_sec;
} smoking_totals_t;

void smoking_model_init(smoking_model_t *model);
void smoking_model_restore(smoking_model_t *model, const smoking_persist_state_t *state);
smoking_persist_state_t smoking_model_persist_snapshot(const smoking_model_t *model);
const char *smoking_feature_name(smoking_feature_t feature);

smoking_feature_t smoking_model_selected_feature(const smoking_model_t *model);
void smoking_model_home_next(smoking_model_t *model);
void smoking_model_home_prev(smoking_model_t *model);
void smoking_model_enter_selected(smoking_model_t *model, uint32_t now_sec);
void smoking_model_enter_time_settings(smoking_model_t *model, uint32_t now_sec);
void smoking_model_return_home(smoking_model_t *model);

void smoking_model_start(smoking_model_t *model, uint32_t now_sec);
void smoking_model_finish(smoking_model_t *model, uint32_t now_sec);
void smoking_model_count_up(smoking_model_t *model);
void smoking_model_count_down(smoking_model_t *model);
void smoking_model_save_pending(smoking_model_t *model, uint32_t now_sec);
smoking_totals_t smoking_model_today(const smoking_model_t *model, int32_t today);
smoking_totals_t smoking_model_last_7_days(const smoking_model_t *model, int32_t today);
uint32_t smoking_model_last_interval_sec(const smoking_model_t *model, uint32_t now_sec);
uint16_t smoking_model_daily_goal(const smoking_model_t *model);
uint8_t smoking_model_goal_streak(const smoking_model_t *model, int32_t today);
uint8_t smoking_model_goal_best_streak(const smoking_model_t *model, int32_t today);

bool smoking_model_idle_backlight_allowed(const smoking_model_t *model);
void smoking_model_time_up(smoking_model_t *model);
void smoking_model_time_down(smoking_model_t *model);
void smoking_model_time_click(smoking_model_t *model);
void smoking_model_settings_up(smoking_model_t *model);
void smoking_model_settings_down(smoking_model_t *model);
void smoking_model_settings_click(smoking_model_t *model);

void smoking_model_enter_focus(smoking_model_t *model);
void smoking_model_focus_click(smoking_model_t *model, uint32_t now_sec);
void smoking_model_focus_reset(smoking_model_t *model);
void smoking_model_focus_toggle_mode(smoking_model_t *model);
bool smoking_model_focus_tick(smoking_model_t *model, uint32_t now_sec);
uint32_t smoking_model_focus_remaining(const smoking_model_t *model, uint32_t now_sec);
const char *smoking_model_focus_mode_name(const smoking_model_t *model);
