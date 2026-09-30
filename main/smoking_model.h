#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SMOKING_MODEL_VERSION 1u
#define SMOKING_HISTORY_DAYS 7u
#define SMOKING_SUDOKU_SIZE 5u
#define SMOKING_SUDOKU_CELLS (SMOKING_SUDOKU_SIZE * SMOKING_SUDOKU_SIZE)
#define SMOKING_SUDOKU_LEVELS 3u

typedef enum {
    SMOKING_SCREEN_HOME = 0,
    SMOKING_SCREEN_ACTIVE,
    SMOKING_SCREEN_COUNT_SELECT,
    SMOKING_SCREEN_SUDOKU,
    SMOKING_SCREEN_SETTINGS,
} smoking_screen_t;

typedef enum {
    SMOKING_FEATURE_SMOKE = 0,
    SMOKING_FEATURE_SUDOKU,
    SMOKING_FEATURE_SETTINGS,
    SMOKING_FEATURE_COUNT,
} smoking_feature_t;

typedef enum {
    SMOKING_SUDOKU_EDIT_DIGIT = 0,
    SMOKING_SUDOKU_SELECT_CELL,
} smoking_sudoku_mode_t;

typedef enum {
    SMOKING_SETTING_TIME = 0,
    SMOKING_SETTING_WIFI,
    SMOKING_SETTING_COUNT,
} smoking_setting_t;

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
    uint8_t level;
    uint8_t selected_empty;
    uint8_t mode;
    uint8_t values[SMOKING_SUDOKU_CELLS];
} smoking_sudoku_state_t;

typedef struct {
    uint32_t version;
    uint8_t screen;
    uint8_t selected_feature;
    uint16_t reserved;
    uint32_t completed_count;
    smoking_day_stat_t days[SMOKING_HISTORY_DAYS];
    smoking_record_t last;
    smoking_sudoku_state_t sudoku;
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

void smoking_model_sudoku_reset(smoking_model_t *model);
void smoking_model_sudoku_click(smoking_model_t *model);
void smoking_model_sudoku_up(smoking_model_t *model);
void smoking_model_sudoku_down(smoking_model_t *model);
uint8_t smoking_model_sudoku_cell_value(const smoking_model_t *model, size_t cell);
bool smoking_model_sudoku_cell_fixed(const smoking_model_t *model, size_t cell);
bool smoking_model_sudoku_cell_error(const smoking_model_t *model, size_t cell);
bool smoking_model_sudoku_complete(const smoking_model_t *model);

bool smoking_model_idle_backlight_allowed(const smoking_model_t *model);
void smoking_model_time_up(smoking_model_t *model);
void smoking_model_time_down(smoking_model_t *model);
void smoking_model_time_click(smoking_model_t *model);
void smoking_model_settings_up(smoking_model_t *model);
void smoking_model_settings_down(smoking_model_t *model);
void smoking_model_settings_click(smoking_model_t *model);
