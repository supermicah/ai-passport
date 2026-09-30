#include <assert.h>
#include <string.h>

#include "smoking_model.h"

#define DAY(n) ((uint32_t)(n) * 86400u)

static void test_smoking_record_flow(void)
{
    smoking_model_t model;
    smoking_model_init(&model);

    smoking_model_start(&model, DAY(10) + 3600);
    assert(model.persisted.screen == SMOKING_SCREEN_ACTIVE);
    smoking_model_finish(&model, DAY(10) + 3660);
    assert(model.persisted.screen == SMOKING_SCREEN_COUNT_SELECT);
    smoking_model_count_up(&model);
    assert(model.pending_cigarettes == 2);
    smoking_model_save_pending(&model, DAY(10) + 3660);

    assert(model.persisted.screen == SMOKING_SCREEN_HOME);
    assert(model.persisted.completed_count == 1);
    assert(model.persisted.last.valid == 1);
    assert(model.persisted.last.day == 10);
    assert(model.persisted.last.start_sec == 3600);
    assert(model.persisted.last.duration_sec == 60);
    assert(model.persisted.last.cigarettes == 2);
}

static void test_stats_and_interval(void)
{
    smoking_model_t model;
    smoking_model_init(&model);

    smoking_model_start(&model, DAY(4) + 86390);
    smoking_model_finish(&model, DAY(4) + 86410);
    smoking_model_save_pending(&model, DAY(5) + 10);
    smoking_model_start(&model, DAY(5) + 120);
    smoking_model_finish(&model, DAY(5) + 125);
    smoking_model_count_up(&model);
    smoking_model_save_pending(&model, DAY(5) + 125);

    smoking_totals_t today = smoking_model_today(&model, 5);
    assert(today.cigarettes == 2);
    assert(today.duration_sec == 5);
    smoking_totals_t week = smoking_model_last_7_days(&model, 5);
    assert(week.cigarettes == 3);
    assert(week.duration_sec == 25);
    assert(smoking_model_last_interval_sec(&model, DAY(6) + 125) == 86400);
}

static void test_history_replaces_oldest(void)
{
    smoking_model_t model;
    smoking_model_init(&model);
    for (uint32_t day = 1; day <= 8; day++) {
        smoking_model_start(&model, DAY(day) + 1);
        smoking_model_finish(&model, DAY(day) + 2);
        smoking_model_save_pending(&model, DAY(day) + 2);
    }
    assert(smoking_model_today(&model, 1).cigarettes == 0);
    assert(smoking_model_today(&model, 8).cigarettes == 1);
    assert(smoking_model_last_7_days(&model, 8).cigarettes == 7);
}

static void test_restore_sanitizes_runtime_and_version(void)
{
    smoking_model_t model;
    smoking_model_init(&model);
    smoking_persist_state_t state = model.persisted;
    state.screen = SMOKING_SCREEN_ACTIVE;
    state.selected_feature = 99;

    smoking_model_restore(&model, &state);
    assert(model.persisted.screen == SMOKING_SCREEN_HOME);
    assert(model.persisted.selected_feature == SMOKING_FEATURE_SMOKE);
    assert(model.smoking_started_at == 0);

    state = model.persisted;
    state.version = 99;
    state.selected_feature = 99;
    smoking_model_restore(&model, &state);
    assert(model.persisted.version == SMOKING_MODEL_VERSION);
    assert(model.persisted.selected_feature == SMOKING_FEATURE_SMOKE);
}

static size_t current_empty_cell(const smoking_model_t *model)
{
    uint8_t selected = model->persisted.sudoku.selected_empty;
    size_t seen = 0;
    for (size_t i = 0; i < SMOKING_SUDOKU_CELLS; i++) {
        if (smoking_model_sudoku_cell_fixed(model, i)) continue;
        if (seen == selected) return i;
        seen++;
    }
    return 0;
}

static void test_sudoku_flow(void)
{
    smoking_model_t model;
    smoking_model_init(&model);
    model.persisted.selected_feature = SMOKING_FEATURE_SUDOKU;
    smoking_model_enter_selected(&model, 0);
    assert(model.persisted.screen == SMOKING_SCREEN_SUDOKU);
    assert(model.persisted.sudoku.mode == SMOKING_SUDOKU_SELECT_CELL);

    size_t first = current_empty_cell(&model);
    smoking_model_sudoku_click(&model);
    assert(model.persisted.sudoku.mode == SMOKING_SUDOKU_EDIT_DIGIT);
    assert(smoking_model_sudoku_cell_value(&model, first) == 1);
    smoking_model_sudoku_up(&model);
    assert(smoking_model_sudoku_cell_value(&model, first) == 5);
    smoking_model_sudoku_down(&model);
    assert(smoking_model_sudoku_cell_value(&model, first) == 1);
    smoking_model_sudoku_click(&model);
    assert(model.persisted.sudoku.mode == SMOKING_SUDOKU_SELECT_CELL);
    smoking_model_sudoku_down(&model);
    assert(current_empty_cell(&model) != first);

    smoking_model_sudoku_reset(&model);
    assert(smoking_model_sudoku_cell_value(&model, first) == 0);

    for (size_t i = 0; i < SMOKING_SUDOKU_CELLS; i++) {
        if (!smoking_model_sudoku_cell_fixed(&model, i)) {
            for (uint8_t v = 1; v <= 5; v++) {
                model.persisted.sudoku.values[i] = v;
                if (!smoking_model_sudoku_cell_error(&model, i)) break;
            }
        }
    }
    assert(smoking_model_sudoku_complete(&model));
    uint8_t old_level = model.persisted.sudoku.level;
    smoking_model_sudoku_down(&model);
    assert(model.persisted.sudoku.level == (uint8_t)(old_level + 1));
}

static void test_idle_policy_and_feature_cycle(void)
{
    smoking_model_t model;
    smoking_model_init(&model);
    assert(smoking_model_selected_feature(&model) == SMOKING_FEATURE_SMOKE);
    smoking_model_home_prev(&model);
    assert(smoking_model_selected_feature(&model) == SMOKING_FEATURE_SETTINGS);
    smoking_model_home_next(&model);
    assert(smoking_model_selected_feature(&model) == SMOKING_FEATURE_SMOKE);
    smoking_model_home_next(&model);
    assert(smoking_model_selected_feature(&model) == SMOKING_FEATURE_SUDOKU);
    smoking_model_home_next(&model);
    assert(smoking_model_selected_feature(&model) == SMOKING_FEATURE_SETTINGS);
    smoking_model_home_next(&model);
    assert(smoking_model_selected_feature(&model) == SMOKING_FEATURE_SMOKE);

    assert(smoking_model_idle_backlight_allowed(&model));
    smoking_model_start(&model, 1);
    assert(!smoking_model_idle_backlight_allowed(&model));
    smoking_model_finish(&model, 2);
    assert(smoking_model_idle_backlight_allowed(&model));

    smoking_model_return_home(&model);
    model.persisted.selected_feature = SMOKING_FEATURE_SUDOKU;
    smoking_model_enter_selected(&model, 10);
    assert(model.persisted.screen == SMOKING_SCREEN_SUDOKU);
    assert(smoking_model_idle_backlight_allowed(&model));

    smoking_model_return_home(&model);
    model.persisted.selected_feature = SMOKING_FEATURE_SETTINGS;
    smoking_model_enter_selected(&model, 10);
    assert(model.persisted.screen == SMOKING_SCREEN_SETTINGS);
    assert(smoking_model_idle_backlight_allowed(&model));
}

static void test_settings_menu_and_time_edit(void)
{
    smoking_model_t model;
    smoking_model_init(&model);
    model.persisted.selected_feature = SMOKING_FEATURE_SETTINGS;
    smoking_model_enter_selected(&model, DAY(2) + 13u * 3600u + 24u * 60u + 59u);

    assert(model.persisted.screen == SMOKING_SCREEN_SETTINGS);
    assert(model.selected_setting == SMOKING_SETTING_TIME);
    assert(!model.editing_time);
    assert(model.edit_hour == 13);
    assert(model.edit_minute == 24);
    assert(!model.edit_minute_selected);

    smoking_model_settings_down(&model);
    assert(model.selected_setting == SMOKING_SETTING_WIFI);
    assert(!model.editing_time);
    smoking_model_settings_down(&model);
    assert(model.selected_setting == SMOKING_SETTING_TIME);

    smoking_model_settings_click(&model);
    assert(model.editing_time);
    assert(!model.edit_minute_selected);
    smoking_model_settings_down(&model);
    assert(model.edit_hour == 14);
    smoking_model_settings_click(&model);
    assert(model.edit_minute_selected);
    smoking_model_settings_up(&model);
    assert(model.edit_minute == 23);
}

int main(void)
{
    test_smoking_record_flow();
    test_stats_and_interval();
    test_history_replaces_oldest();
    test_restore_sanitizes_runtime_and_version();
    test_sudoku_flow();
    test_idle_policy_and_feature_cycle();
    test_settings_menu_and_time_edit();
    return 0;
}
